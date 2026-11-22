/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "gnss_uart_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

LOG_MODULE_DECLARE(gnss_sample, CONFIG_GNSS_SAMPLE_LOG_LEVEL);

#define GNSS_UART_NODE DT_ALIAS(gnss_uart)

#if !DT_NODE_HAS_STATUS(GNSS_UART_NODE, okay)
#error "GNSS UART bridge requires a devicetree alias named gnss-uart"
#endif

#define JSON_BUF_LEN 512
#define CMD_BUF_LEN 64

static const struct device *const bridge_uart = DEVICE_DT_GET(GNSS_UART_NODE);

struct uart_cmd {
	char data[CMD_BUF_LEN];
};

static atomic_t stream_enabled = ATOMIC_INIT(1);
static atomic_t stream_rate_hz = ATOMIC_INIT(CONFIG_GNSS_SAMPLE_UART_BRIDGE_STREAM_RATE_HZ);
static atomic_t rx_command_count;
static atomic_t rx_overflow_count;

static char last_gga[NRF_MODEM_GNSS_NMEA_MAX_LEN];
static char last_rmc[NRF_MODEM_GNSS_NMEA_MAX_LEN];
static char rx_line[CMD_BUF_LEN];
static size_t rx_line_len;
static int64_t last_publish_ms;

K_MSGQ_DEFINE(uart_cmd_queue, sizeof(struct uart_cmd), 4, 4);
static struct k_work uart_cmd_work;

static void uart_write_str(const char *s)
{
	while (*s != '\0') {
		uart_poll_out(bridge_uart, *s++);
	}
}

static void json_append_escaped(char *buf, size_t len, const char *value)
{
	size_t used = strnlen(buf, len);

	if (used >= len) {
		return;
	}

	for (const char *p = value; *p != '\0'; ++p) {
		if (used >= len - 1U) {
			return;
		}

		if (*p == '\\' || *p == '"') {
			if (used < len - 2U) {
				buf[used++] = '\\';
				buf[used++] = *p;
				buf[used] = '\0';
			}
		} else if (*p == '\r') {
			continue;
		} else if (*p == '\n') {
			continue;
		} else {
			buf[used++] = *p;
			buf[used] = '\0';
		}
	}
}

static bool nmea_is_sentence(const char *line, const char *prefix)
{
	return strncmp(line, prefix, strlen(prefix)) == 0;
}

void gnss_uart_bridge_set_last_nmea(const struct nrf_modem_gnss_nmea_data_frame *nmea)
{
	if (!nmea) {
		return;
	}

	if (nmea_is_sentence(nmea->nmea_str, "$GPGGA") ||
	    nmea_is_sentence(nmea->nmea_str, "$GNGGA")) {
		strncpy(last_gga, nmea->nmea_str, sizeof(last_gga) - 1U);
		last_gga[sizeof(last_gga) - 1U] = '\0';
	} else if (nmea_is_sentence(nmea->nmea_str, "$GPRMC") ||
		   nmea_is_sentence(nmea->nmea_str, "$GNRMC")) {
		strncpy(last_rmc, nmea->nmea_str, sizeof(last_rmc) - 1U);
		last_rmc[sizeof(last_rmc) - 1U] = '\0';
	}
}

static void append_optional_nmea(char *json, size_t len)
{
	if (last_gga[0] != '\0') {
		strncat(json, ",\"nmea_gga\":\"", len - strlen(json) - 1U);
		json_append_escaped(json, len, last_gga);
		strncat(json, "\"", len - strlen(json) - 1U);
	}

	if (last_rmc[0] != '\0') {
		strncat(json, ",\"nmea_rmc\":\"", len - strlen(json) - 1U);
		json_append_escaped(json, len, last_rmc);
		strncat(json, "\"", len - strlen(json) - 1U);
	}
}

void gnss_uart_bridge_publish_status(const char *code, const char *message, const char *details)
{
	char json[JSON_BUF_LEN];

	snprintk(json, sizeof(json),
		 "{\"type\":\"status\",\"ts_ms\":%lld,\"code\":\"%s\",\"message\":\"%s\",\"details\":\"",
		 k_uptime_get(), code ? code : "", message ? message : "");
	json_append_escaped(json, sizeof(json), details ? details : "");
	strncat(json, "\"}\n", sizeof(json) - strlen(json) - 1U);
	uart_write_str(json);
}

void gnss_uart_bridge_send_text(const char *text)
{
	if (!text) {
		return;
	}

	uart_write_str(text);
	uart_write_str("\r\n");
}

static void handle_command(char *cmd)
{
	atomic_inc(&rx_command_count);

	if (strcmp(cmd, "PING") == 0) {
		gnss_uart_bridge_publish_status("PONG", "UART bridge is alive", "");
		return;
	}

	if (strcmp(cmd, "START_STREAM") == 0) {
		atomic_set(&stream_enabled, 1);
		gnss_uart_bridge_publish_status("STREAM_STARTED", "Streaming enabled", "");
		return;
	}

	if (strcmp(cmd, "STOP_STREAM") == 0) {
		atomic_set(&stream_enabled, 0);
		gnss_uart_bridge_publish_status("STREAM_STOPPED", "Streaming disabled", "");
		return;
	}

	if (strncmp(cmd, "SET_RATE:", 9) == 0) {
		long rate = strtol(cmd + 9, NULL, 10);

		if (rate >= 1 && rate <= 5) {
			atomic_set(&stream_rate_hz, (int)rate);
			gnss_uart_bridge_publish_status("RATE_UPDATED", "Stream rate updated", cmd + 9);
		} else {
			gnss_uart_bridge_publish_status("INVALID_RATE", "SET_RATE must be 1..5", cmd + 9);
		}
		return;
	}

	if (strcmp(cmd, "REQUEST_STATUS") == 0) {
		char details[96];

		snprintk(details, sizeof(details), "stream=%s rate_hz=%u rx_commands=%u rx_overflows=%u",
			 atomic_get(&stream_enabled) ? "on" : "off",
			 (uint8_t)atomic_get(&stream_rate_hz),
			 (uint32_t)atomic_get(&rx_command_count),
			 (uint32_t)atomic_get(&rx_overflow_count));
		gnss_uart_bridge_publish_status("STATUS", "UART bridge status", details);
		return;
	}

	gnss_uart_bridge_publish_status("UNKNOWN_COMMAND", "Unsupported UART command", cmd);
}

static void uart_cmd_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	struct uart_cmd cmd;

	while (k_msgq_get(&uart_cmd_queue, &cmd, K_NO_WAIT) == 0) {
		handle_command(cmd.data);
	}
}

static void uart_irq_callback(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	while (uart_irq_update(dev) && uart_irq_rx_ready(dev)) {
		uint8_t c;

		while (uart_fifo_read(dev, &c, 1) == 1) {
			if (c == '\r' || c == '\n') {
				if (rx_line_len > 0U) {
					struct uart_cmd cmd = {0};

					memcpy(cmd.data, rx_line, rx_line_len);
					cmd.data[rx_line_len] = '\0';
					if (k_msgq_put(&uart_cmd_queue, &cmd, K_NO_WAIT) != 0) {
						atomic_inc(&rx_overflow_count);
					} else {
						k_work_submit(&uart_cmd_work);
					}
					rx_line_len = 0U;
				}
				continue;
			}

			if (rx_line_len < sizeof(rx_line) - 1U) {
				rx_line[rx_line_len++] = (char)c;
			} else {
				rx_line_len = 0U;
				atomic_inc(&rx_overflow_count);
			}
		}
	}
}

int gnss_uart_bridge_init(void)
{
	if (!device_is_ready(bridge_uart)) {
		LOG_ERR("GNSS UART bridge device is not ready");
		return -ENODEV;
	}

	k_work_init(&uart_cmd_work, uart_cmd_work_fn);

	uart_irq_callback_user_data_set(bridge_uart, uart_irq_callback, NULL);
	uart_irq_rx_enable(bridge_uart);

	gnss_uart_bridge_publish_status("BOOT", "GNSS UART bridge ready",
					  "pins=nRF9151 P0.29 TX/P0.28 RX baud=9600");
	return 0;
}

void gnss_uart_bridge_publish_pvt(const struct nrf_modem_gnss_pvt_data_frame *pvt,
					  uint8_t tracked,
					  uint8_t used,
					  uint8_t unhealthy,
					  const struct gnss_uart_bridge_metrics *metrics)
{
	if (!atomic_get(&stream_enabled)) {
		return;
	}

	int64_t now = k_uptime_get();
	uint8_t rate_hz = (uint8_t)atomic_get(&stream_rate_hz);
	int64_t min_interval_ms = 1000 / MAX(rate_hz, 1U);

	if (last_publish_ms != 0 && (now - last_publish_ms) < min_interval_ms) {
		return;
	}
	last_publish_ms = now;

	bool fix_valid = (pvt->flags & NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID) != 0;
	char json[JSON_BUF_LEN];
	int len;

	if (fix_valid) {
		len = snprintk(json, sizeof(json),
			"{\"ts_ms\":%lld,\"fix_valid\":true,"
			"\"lat\":%.06f,\"lon\":%.06f,\"alt_m\":%.01f,"
			"\"speed_mps\":%.01f,\"heading_deg\":%.01f,\"accuracy_m\":%.01f,\"hdop\":%.01f,"
			"\"tracked\":%u,\"used\":%u,\"unhealthy\":%u,"
			"\"counters\":{\"accepted\":%u,\"rejected\":%u,\"nmea_dropped\":%u,"
			"\"uart_rx_commands\":%u,\"uart_rx_overflows\":%u}",
			now, pvt->latitude, pvt->longitude, (double)pvt->altitude,
			(double)pvt->speed, (double)pvt->heading, (double)pvt->accuracy,
			(double)pvt->hdop, tracked, used, unhealthy,
			metrics ? metrics->fixes_accepted : 0U,
			metrics ? metrics->fixes_rejected : 0U,
			metrics ? metrics->nmea_dropped : 0U,
			(uint32_t)atomic_get(&rx_command_count),
			(uint32_t)atomic_get(&rx_overflow_count));
	} else {
		len = snprintk(json, sizeof(json),
			"{\"ts_ms\":%lld,\"fix_valid\":false,"
			"\"lat\":null,\"lon\":null,\"alt_m\":null,"
			"\"speed_mps\":null,\"heading_deg\":null,\"accuracy_m\":%.01f,\"hdop\":%.01f,"
			"\"tracked\":%u,\"used\":%u,\"unhealthy\":%u,"
			"\"counters\":{\"accepted\":%u,\"rejected\":%u,\"nmea_dropped\":%u,"
			"\"uart_rx_commands\":%u,\"uart_rx_overflows\":%u}",
			now, (double)pvt->accuracy, (double)pvt->hdop, tracked, used, unhealthy,
			metrics ? metrics->fixes_accepted : 0U,
			metrics ? metrics->fixes_rejected : 0U,
			metrics ? metrics->nmea_dropped : 0U,
			(uint32_t)atomic_get(&rx_command_count),
			(uint32_t)atomic_get(&rx_overflow_count));
	}

	if (len < 0 || len >= sizeof(json)) {
		return;
	}

	append_optional_nmea(json, sizeof(json));
	strncat(json, "}\n", sizeof(json) - strlen(json) - 1U);
	uart_write_str(json);
}

bool gnss_uart_bridge_stream_enabled(void)
{
	return atomic_get(&stream_enabled) != 0;
}

uint8_t gnss_uart_bridge_stream_rate_hz(void)
{
	return (uint8_t)atomic_get(&stream_rate_hz);
}

uint32_t gnss_uart_bridge_rx_command_count(void)
{
	return (uint32_t)atomic_get(&rx_command_count);
}

uint32_t gnss_uart_bridge_rx_overflow_count(void)
{
	return (uint32_t)atomic_get(&rx_overflow_count);
}
