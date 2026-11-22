/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "gnss_ble.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/settings/settings.h>

LOG_MODULE_DECLARE(gnss_sample, CONFIG_GNSS_SAMPLE_LOG_LEVEL);

#define GNSS_BLE_JSON_BUF_LEN 768
#define GNSS_BLE_CMD_MAX_LEN 64

/* 6b6f6f00-39f0-4d47-9c30-47c0c53b3d01 */
#define BT_UUID_GNSS_SERVICE_VAL \
	BT_UUID_128_ENCODE(0x6b6f6f00, 0x39f0, 0x4d47, 0x9c30, 0x47c0c53b3d01)
/* 6b6f6f01-39f0-4d47-9c30-47c0c53b3d01 */
#define BT_UUID_GNSS_TX_VAL \
	BT_UUID_128_ENCODE(0x6b6f6f01, 0x39f0, 0x4d47, 0x9c30, 0x47c0c53b3d01)
/* 6b6f6f02-39f0-4d47-9c30-47c0c53b3d01 */
#define BT_UUID_GNSS_RX_VAL \
	BT_UUID_128_ENCODE(0x6b6f6f02, 0x39f0, 0x4d47, 0x9c30, 0x47c0c53b3d01)

static struct bt_uuid_128 gnss_service_uuid = BT_UUID_INIT_128(BT_UUID_GNSS_SERVICE_VAL);
static struct bt_uuid_128 gnss_tx_uuid = BT_UUID_INIT_128(BT_UUID_GNSS_TX_VAL);
static struct bt_uuid_128 gnss_rx_uuid = BT_UUID_INIT_128(BT_UUID_GNSS_RX_VAL);

static struct bt_conn *current_conn;
static atomic_t notify_enabled;
static atomic_t stream_enabled = ATOMIC_INIT(1);
static atomic_t stream_rate_hz = ATOMIC_INIT(CONFIG_GNSS_SAMPLE_BLE_STREAM_RATE_HZ);
static atomic_t ble_dropped;

static char last_gga[NRF_MODEM_GNSS_NMEA_MAX_LEN];
static char last_rmc[NRF_MODEM_GNSS_NMEA_MAX_LEN];

struct ble_line {
	char data[GNSS_BLE_JSON_BUF_LEN];
};

K_MSGQ_DEFINE(ble_tx_queue, sizeof(struct ble_line), CONFIG_GNSS_SAMPLE_BLE_TX_QUEUE_LEN, 4);
static struct k_work ble_tx_work;

static struct bt_conn_cb conn_callbacks;

static ssize_t gnss_rx_write(struct bt_conn *conn,
			     const struct bt_gatt_attr *attr,
			     const void *buf,
			     uint16_t len,
			     uint16_t offset,
			     uint8_t flags);
static void ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value);

BT_GATT_SERVICE_DEFINE(gnss_service,
	BT_GATT_PRIMARY_SERVICE(&gnss_service_uuid),
	BT_GATT_CHARACTERISTIC(&gnss_tx_uuid.uuid,
			       BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_NONE,
			       NULL, NULL, NULL),
	BT_GATT_CCC(ccc_cfg_changed,
		    BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CHARACTERISTIC(&gnss_rx_uuid.uuid,
			       BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE_ENCRYPT,
			       NULL, gnss_rx_write, NULL),
);

static void ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);

	atomic_set(&notify_enabled, (value == BT_GATT_CCC_NOTIFY) ? 1 : 0);
	LOG_INF("BLE notifications %s", atomic_get(&notify_enabled) ? "enabled" : "disabled");
}

static int ble_enqueue_line(const char *line)
{
	struct ble_line item = { 0 };
	size_t copy_len = MIN(strlen(line), sizeof(item.data) - 1U);
	memcpy(item.data, line, copy_len);

	int err = k_msgq_put(&ble_tx_queue, &item, K_NO_WAIT);
	if (err) {
		struct ble_line dropped;
		if (k_msgq_get(&ble_tx_queue, &dropped, K_NO_WAIT) == 0) {
			atomic_inc(&ble_dropped);
			(void)k_msgq_put(&ble_tx_queue, &item, K_NO_WAIT);
		}
	}

	k_work_submit(&ble_tx_work);
	return 0;
}

static void ble_tx_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	if (!current_conn || !atomic_get(&notify_enabled) || !atomic_get(&stream_enabled)) {
		return;
	}

	struct ble_line item;
	while (k_msgq_get(&ble_tx_queue, &item, K_NO_WAIT) == 0) {
		int err = bt_gatt_notify(current_conn, &gnss_service.attrs[2], item.data,
					 strlen(item.data));
		if (err) {
			LOG_WRN("BLE notify failed: %d", err);
			break;
		}
	}
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		LOG_WRN("BLE connect failed: %u", err);
		return;
	}

	current_conn = bt_conn_ref(conn);
	LOG_INF("BLE connected");
	(void)bt_conn_set_security(conn, BT_SECURITY_L2);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(reason);

	if (current_conn) {
		bt_conn_unref(current_conn);
		current_conn = NULL;
	}

	atomic_clear(&notify_enabled);
	LOG_INF("BLE disconnected");
}

static bool nmea_is_sentence(const char *line, const char *prefix)
{
	return (strncmp(line, prefix, strlen(prefix)) == 0);
}

static bool pvt_datetime_valid(const struct nrf_modem_gnss_datetime *datetime)
{
	return datetime &&
	       datetime->year >= 2024 &&
	       datetime->month >= 1 && datetime->month <= 12 &&
	       datetime->day >= 1 && datetime->day <= 31 &&
	       datetime->hour <= 23 &&
	       datetime->minute <= 59 &&
	       datetime->seconds <= 60;
}

void gnss_ble_set_last_nmea(const struct nrf_modem_gnss_nmea_data_frame *nmea)
{
	if (!nmea) {
		return;
	}

	if (nmea_is_sentence(nmea->nmea_str, "$GPGGA") || nmea_is_sentence(nmea->nmea_str, "$GNGGA")) {
		strncpy(last_gga, nmea->nmea_str, sizeof(last_gga) - 1U);
		last_gga[sizeof(last_gga) - 1U] = '\0';
	} else if (nmea_is_sentence(nmea->nmea_str, "$GPRMC") ||
		   nmea_is_sentence(nmea->nmea_str, "$GNRMC")) {
		strncpy(last_rmc, nmea->nmea_str, sizeof(last_rmc) - 1U);
		last_rmc[sizeof(last_rmc) - 1U] = '\0';
	}
}

static ssize_t gnss_rx_write(struct bt_conn *conn,
			     const struct bt_gatt_attr *attr,
			     const void *buf,
			     uint16_t len,
			     uint16_t offset,
			     uint8_t flags)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(offset);
	ARG_UNUSED(flags);

	char cmd[GNSS_BLE_CMD_MAX_LEN] = {0};
	size_t copy = MIN((size_t)len, sizeof(cmd) - 1U);
	memcpy(cmd, buf, copy);

	if (strcmp(cmd, "START_STREAM") == 0) {
		atomic_set(&stream_enabled, 1);
		gnss_ble_notify_status("STREAM_STARTED", "Streaming enabled", "");
		return len;
	}

	if (strcmp(cmd, "STOP_STREAM") == 0) {
		atomic_set(&stream_enabled, 0);
		gnss_ble_notify_status("STREAM_STOPPED", "Streaming disabled", "");
		return len;
	}

	if (strncmp(cmd, "SET_RATE:", 9) == 0) {
		long rate = strtol(cmd + 9, NULL, 10);
		if (rate >= 1 && rate <= 5) {
			atomic_set(&stream_rate_hz, (int)rate);
			gnss_ble_notify_status("RATE_UPDATED", "Stream rate updated", cmd + 9);
		} else {
			gnss_ble_notify_status("INVALID_RATE", "SET_RATE must be between 1 and 5",
					       cmd + 9);
		}
		return len;
	}

	if (strcmp(cmd, "REQUEST_STATUS") == 0) {
		gnss_ble_notify_status("STATUS", "Status requested", "");
		return len;
	}

	gnss_ble_notify_status("UNKNOWN_COMMAND", "Unsupported command", cmd);
	return len;
}

void gnss_ble_notify_status(const char *code, const char *message, const char *details)
{
	char buf[GNSS_BLE_JSON_BUF_LEN];
	snprintk(buf, sizeof(buf),
		 "{\"type\":\"status\",\"code\":\"%s\",\"message\":\"%s\",\"details\":\"%s\"}\n",
		 code ? code : "", message ? message : "", details ? details : "");
	(void)ble_enqueue_line(buf);
}

void gnss_ble_notify_pvt(const struct nrf_modem_gnss_pvt_data_frame *pvt,
			 uint8_t tracked,
			 uint8_t used,
			 uint8_t unhealthy,
			 const struct gnss_ble_metrics *metrics)
{
	static int64_t last_sent_ms;
	int hz = atomic_get(&stream_rate_hz);
	int64_t now_ms = k_uptime_get();
	int64_t min_period = 1000 / MAX(hz, 1);

	if (!pvt || !atomic_get(&stream_enabled)) {
		return;
	}

	if ((now_ms - last_sent_ms) < min_period) {
		return;
	}

	last_sent_ms = now_ms;

	char json[GNSS_BLE_JSON_BUF_LEN];
	bool valid = (pvt->flags & NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID) != 0;
	uint32_t accepted = metrics ? metrics->fixes_accepted : 0U;
	uint32_t rejected = metrics ? metrics->fixes_rejected : 0U;
	uint32_t nmea_d = metrics ? metrics->nmea_dropped : 0U;
	char timestamp[25] = {0};
	bool have_timestamp = false;

	if (pvt_datetime_valid(&pvt->datetime)) {
		snprintk(timestamp, sizeof(timestamp),
			 "%04u-%02u-%02uT%02u:%02u:%02uZ",
			 pvt->datetime.year, pvt->datetime.month, pvt->datetime.day,
			 pvt->datetime.hour, pvt->datetime.minute, pvt->datetime.seconds);
		have_timestamp = true;
	}

	if (valid) {
		if (have_timestamp) {
			snprintk(json, sizeof(json),
				 "{\"ts_ms\":%lld,\"timestamp\":\"%s\",\"fix_valid\":true,"
				 "\"lat\":%.06f,\"lon\":%.06f,\"accuracy_m\":%.01f,"
				 "\"alt_m\":%.01f,\"alt_acc_m\":%.01f,"
				 "\"speed_mps\":%.01f,\"speed_acc_mps\":%.01f,"
				 "\"v_speed_mps\":%.01f,\"v_speed_acc_mps\":%.01f,"
				 "\"heading_deg\":%.01f,\"heading_acc_deg\":%.01f,"
				 "\"pdop\":%.01f,\"hdop\":%.01f,\"vdop\":%.01f,\"tdop\":%.01f,"
				 "\"tracked\":%u,\"used\":%u,\"unhealthy\":%u,"
				 "\"nmea_gga\":\"%s\",\"nmea_rmc\":\"%s\","
				 "\"counters\":{\"accepted\":%u,\"rejected\":%u,\"nmea_dropped\":%u,"
				 "\"ble_dropped\":%u}}\n",
				 now_ms, timestamp,
				 pvt->latitude, pvt->longitude, (double)pvt->accuracy,
				 (double)pvt->altitude, (double)pvt->altitude_accuracy,
				 (double)pvt->speed, (double)pvt->speed_accuracy,
				 (double)pvt->vertical_speed, (double)pvt->vertical_speed_accuracy,
				 (double)pvt->heading, (double)pvt->heading_accuracy,
				 (double)pvt->pdop, (double)pvt->hdop, (double)pvt->vdop, (double)pvt->tdop,
				 tracked, used, unhealthy,
				 last_gga, last_rmc, accepted, rejected, nmea_d,
				 (uint32_t)atomic_get(&ble_dropped));
		} else {
			snprintk(json, sizeof(json),
				 "{\"ts_ms\":%lld,\"fix_valid\":true,\"lat\":%.06f,\"lon\":%.06f,"
				 "\"accuracy_m\":%.01f,\"alt_m\":%.01f,\"alt_acc_m\":%.01f,"
				 "\"speed_mps\":%.01f,\"speed_acc_mps\":%.01f,"
				 "\"v_speed_mps\":%.01f,\"v_speed_acc_mps\":%.01f,"
				 "\"heading_deg\":%.01f,\"heading_acc_deg\":%.01f,"
				 "\"pdop\":%.01f,\"hdop\":%.01f,\"vdop\":%.01f,\"tdop\":%.01f,"
				 "\"tracked\":%u,\"used\":%u,\"unhealthy\":%u,"
				 "\"nmea_gga\":\"%s\",\"nmea_rmc\":\"%s\","
				 "\"counters\":{\"accepted\":%u,\"rejected\":%u,\"nmea_dropped\":%u,"
				 "\"ble_dropped\":%u}}\n",
				 now_ms,
				 pvt->latitude, pvt->longitude, (double)pvt->accuracy,
				 (double)pvt->altitude, (double)pvt->altitude_accuracy,
				 (double)pvt->speed, (double)pvt->speed_accuracy,
				 (double)pvt->vertical_speed, (double)pvt->vertical_speed_accuracy,
				 (double)pvt->heading, (double)pvt->heading_accuracy,
				 (double)pvt->pdop, (double)pvt->hdop, (double)pvt->vdop, (double)pvt->tdop,
				 tracked, used, unhealthy,
				 last_gga, last_rmc, accepted, rejected, nmea_d,
				 (uint32_t)atomic_get(&ble_dropped));
		}
	} else {
		if (have_timestamp) {
			snprintk(json, sizeof(json),
				 "{\"ts_ms\":%lld,\"timestamp\":\"%s\",\"fix_valid\":false,"
				 "\"lat\":null,\"lon\":null,\"alt_m\":null,"
				 "\"accuracy_m\":%.01f,\"alt_acc_m\":%.01f,"
				 "\"speed_mps\":%.01f,\"speed_acc_mps\":%.01f,"
				 "\"v_speed_mps\":%.01f,\"v_speed_acc_mps\":%.01f,"
				 "\"heading_deg\":%.01f,\"heading_acc_deg\":%.01f,"
				 "\"pdop\":%.01f,\"hdop\":%.01f,\"vdop\":%.01f,\"tdop\":%.01f,"
				 "\"tracked\":%u,\"used\":%u,\"unhealthy\":%u,"
				 "\"nmea_gga\":\"%s\",\"nmea_rmc\":\"%s\","
				 "\"counters\":{\"accepted\":%u,\"rejected\":%u,\"nmea_dropped\":%u,"
				 "\"ble_dropped\":%u}}\n",
				 now_ms, timestamp,
				 (double)pvt->accuracy, (double)pvt->altitude_accuracy,
				 (double)pvt->speed, (double)pvt->speed_accuracy,
				 (double)pvt->vertical_speed, (double)pvt->vertical_speed_accuracy,
				 (double)pvt->heading, (double)pvt->heading_accuracy,
				 (double)pvt->pdop, (double)pvt->hdop, (double)pvt->vdop, (double)pvt->tdop,
				 tracked, used, unhealthy,
				 last_gga, last_rmc, accepted, rejected, nmea_d,
				 (uint32_t)atomic_get(&ble_dropped));
		} else {
			snprintk(json, sizeof(json),
				 "{\"ts_ms\":%lld,\"fix_valid\":false,\"lat\":null,\"lon\":null,\"alt_m\":null,"
				 "\"accuracy_m\":%.01f,\"alt_acc_m\":%.01f,"
				 "\"speed_mps\":%.01f,\"speed_acc_mps\":%.01f,"
				 "\"v_speed_mps\":%.01f,\"v_speed_acc_mps\":%.01f,"
				 "\"heading_deg\":%.01f,\"heading_acc_deg\":%.01f,"
				 "\"pdop\":%.01f,\"hdop\":%.01f,\"vdop\":%.01f,\"tdop\":%.01f,"
				 "\"tracked\":%u,\"used\":%u,\"unhealthy\":%u,"
				 "\"nmea_gga\":\"%s\",\"nmea_rmc\":\"%s\","
				 "\"counters\":{\"accepted\":%u,\"rejected\":%u,\"nmea_dropped\":%u,"
				 "\"ble_dropped\":%u}}\n",
				 now_ms,
				 (double)pvt->accuracy, (double)pvt->altitude_accuracy,
				 (double)pvt->speed, (double)pvt->speed_accuracy,
				 (double)pvt->vertical_speed, (double)pvt->vertical_speed_accuracy,
				 (double)pvt->heading, (double)pvt->heading_accuracy,
				 (double)pvt->pdop, (double)pvt->hdop, (double)pvt->vdop, (double)pvt->tdop,
				 tracked, used, unhealthy,
				 last_gga, last_rmc, accepted, rejected, nmea_d,
				 (uint32_t)atomic_get(&ble_dropped));
		}
	}

	(void)ble_enqueue_line(json);
}

bool gnss_ble_stream_enabled(void)
{
	return atomic_get(&stream_enabled) != 0;
}

uint8_t gnss_ble_stream_rate_hz(void)
{
	return (uint8_t)atomic_get(&stream_rate_hz);
}

uint32_t gnss_ble_dropped_count(void)
{
	return (uint32_t)atomic_get(&ble_dropped);
}

int gnss_ble_init(void)
{
	int err;

	conn_callbacks.connected = connected;
	conn_callbacks.disconnected = disconnected;
	bt_conn_cb_register(&conn_callbacks);
	k_work_init(&ble_tx_work, ble_tx_work_fn);

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("Failed to enable BLE: %d", err);
		return err;
	}

	if (IS_ENABLED(CONFIG_SETTINGS)) {
		settings_load();
	}

	err = bt_set_name(CONFIG_GNSS_SAMPLE_BLE_DEVICE_NAME);
	if (err) {
		LOG_WRN("Failed to set BLE name: %d", err);
	}

	const struct bt_data ad[] = {
		BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
		BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_GNSS_SERVICE_VAL),
	};
	const struct bt_data sd[] = {
		BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_GNSS_SAMPLE_BLE_DEVICE_NAME,
			sizeof(CONFIG_GNSS_SAMPLE_BLE_DEVICE_NAME) - 1),
	};

	err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (err) {
		LOG_ERR("Failed to start BLE advertising: %d", err);
		return err;
	}

	LOG_INF("BLE advertising started as %s", CONFIG_GNSS_SAMPLE_BLE_DEVICE_NAME);
	return 0;
}
