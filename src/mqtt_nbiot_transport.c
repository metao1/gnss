/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "mqtt_nbiot_transport.h"

#include "cloud_config.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <modem/lte_lc.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#if defined(CONFIG_SHELL)
#include <zephyr/shell/shell.h>
#endif

LOG_MODULE_DECLARE(gnss_sample, CONFIG_GNSS_SAMPLE_LOG_LEVEL);

#define MQTT_RX_BUF_LEN 512
#define MQTT_TX_BUF_LEN 512
#define MQTT_CFG_PAYLOAD_MAX 512
#define MQTT_SUB_TOPIC_MAX 160
#define TELEMETRY_PAYLOAD_MAX 512
#define ACK_ID_MAX 40

#define TN_RETRY_WHILE_ON_NTN_SEC 300
#define NTN_MONTH_MS (30LL * 24LL * 60LL * 60LL * 1000LL)

enum radio_path {
	RADIO_PATH_TN = 0,
	RADIO_PATH_NTN = 1,
};

enum transport_state {
	STATE_BOOT = 0,
	STATE_TN_CONNECTING,
	STATE_TN_AVAILABLE,
	STATE_TN_UNAVAILABLE,
	STATE_NTN_FALLBACK_ARMED,
	STATE_NTN_SENDING,
	STATE_NTN_SENT,
	STATE_NTN_FAILED,
	STATE_RETURN_TO_TN,
	STATE_MQTT_SYNC,
	STATE_SLEEP_BACKOFF,
};

static struct mqtt_client mqtt_client_ctx;
static struct sockaddr_storage mqtt_broker;
static struct zsock_pollfd mqtt_fds[1];
static uint8_t mqtt_rx_buf[MQTT_RX_BUF_LEN];
static uint8_t mqtt_tx_buf[MQTT_TX_BUF_LEN];

static atomic_t lte_registered;
static atomic_t lte_connect_started;
static atomic_t mqtt_sync_connected;
static atomic_t last_error;
static atomic_t last_lte_reg_status;

static atomic_t cfg_sync_ok;
static atomic_t cfg_sync_fail;
static atomic_t telemetry_ok;
static atomic_t telemetry_fail;
static atomic_t backend_rsp_ok;
static atomic_t backend_rsp_fail;
static atomic_t skip_lte_not_ready;
static atomic_t skip_interval;
static atomic_t skip_no_fix;
static atomic_t dropped_samples;
static atomic_t force_sync_requested;

static atomic_t ntn_sent_ok;
static atomic_t ntn_sent_fail;
static atomic_t ntn_skipped_interval;
static atomic_t ntn_skipped_budget;

static atomic_t seq_no;
static atomic_t transport_state;
static atomic_t active_radio_path;

static bool mqtt_payload_ready;
static char mqtt_payload[MQTT_CFG_PAYLOAD_MAX];

static struct nrf_modem_gnss_pvt_data_frame latest_pvt;
static struct nrf_modem_gnss_pvt_data_frame last_good_fix;
static bool latest_has_pvt;
static bool latest_fix_accepted;
static bool have_last_good_fix;

static struct k_mutex pvt_lock;
static struct k_mutex ack_lock;
static char pending_ack_id[ACK_ID_MAX];

static int64_t last_telemetry_ms;
static int64_t last_sync_ms;
static int64_t next_retry_backoff_ms;

static int64_t tn_unavailable_since_ms;
static int64_t ntn_last_send_ms;
static int64_t ntn_month_start_ms;
static uint32_t ntn_monthly_bytes;
static bool ntn_attempted_in_cycle;
static int64_t next_tn_recheck_ms;

static K_SEM_DEFINE(lte_registered_sem, 0, 1);

static void transport_thread_fn(void);
K_THREAD_STACK_DEFINE(transport_stack, CONFIG_GNSS_SAMPLE_MQTT_STACK_SIZE);
static struct k_thread transport_thread;
static k_tid_t transport_tid;

static const char *state_to_str(enum transport_state state)
{
	switch (state) {
	case STATE_BOOT:
		return "BOOT";
	case STATE_TN_CONNECTING:
		return "TN_CONNECTING";
	case STATE_TN_AVAILABLE:
		return "TN_AVAILABLE";
	case STATE_TN_UNAVAILABLE:
		return "TN_UNAVAILABLE";
	case STATE_NTN_FALLBACK_ARMED:
		return "NTN_FALLBACK_ARMED";
	case STATE_NTN_SENDING:
		return "NTN_SENDING";
	case STATE_NTN_SENT:
		return "NTN_SENT";
	case STATE_NTN_FAILED:
		return "NTN_FAILED";
	case STATE_RETURN_TO_TN:
		return "RETURN_TO_TN";
	case STATE_MQTT_SYNC:
		return "MQTT_SYNC";
	case STATE_SLEEP_BACKOFF:
		return "SLEEP_BACKOFF";
	default:
		return "UNKNOWN";
	}
}

static void set_state(enum transport_state state)
{
	enum transport_state prev = (enum transport_state)atomic_set(&transport_state, state);

	if (prev != state) {
		LOG_INF("%s", state_to_str(state));
	}
}

static bool data_mode_is_auto(const struct cloud_runtime_config *cfg)
{
	return strcmp(cfg->data_mode, "auto") == 0;
}

static bool data_mode_is_tn_only(const struct cloud_runtime_config *cfg)
{
	return strcmp(cfg->data_mode, "tn_only") == 0 || strcmp(cfg->data_mode, "tn") == 0;
}

static bool data_mode_is_ntn_only(const struct cloud_runtime_config *cfg)
{
	return strcmp(cfg->data_mode, "ntn_only") == 0 || strcmp(cfg->data_mode, "ntn") == 0;
}

static bool registration_is_ready(enum lte_lc_nw_reg_status status)
{
	return status == LTE_LC_NW_REG_REGISTERED_HOME ||
	       status == LTE_LC_NW_REG_REGISTERED_ROAMING;
}

static bool is_network_unavailable_err(int err)
{
	switch (err) {
	case -ETIMEDOUT:
	case -EHOSTUNREACH:
	case -ENETDOWN:
	case -ENETUNREACH:
	case -EPIPE:
	case -ECONNRESET:
		return true;
	default:
		return false;
	}
}

const char *mqtt_nbiot_reg_status_to_str(int32_t reg_status)
{
	switch (reg_status) {
	case LTE_LC_NW_REG_NOT_REGISTERED:
		return "NOT_REGISTERED";
	case LTE_LC_NW_REG_REGISTERED_HOME:
		return "REGISTERED_HOME";
	case LTE_LC_NW_REG_SEARCHING:
		return "SEARCHING";
	case LTE_LC_NW_REG_REGISTRATION_DENIED:
		return "REGISTRATION_DENIED";
	case LTE_LC_NW_REG_UNKNOWN:
		return "UNKNOWN";
	case LTE_LC_NW_REG_REGISTERED_ROAMING:
		return "REGISTERED_ROAMING";
	case LTE_LC_NW_REG_NO_SUITABLE_CELL:
		return "NO_SUITABLE_CELL";
	default:
		return "UNMAPPED";
	}
}

const char *mqtt_nbiot_err_to_str(int32_t err)
{
	switch (err) {
	case 0:
		return "OK";
	case -ETIMEDOUT:
		return "ETIMEDOUT";
	case -EHOSTUNREACH:
		return "EHOSTUNREACH";
	case -ECONNREFUSED:
		return "ECONNREFUSED";
	case -ENETDOWN:
		return "ENETDOWN";
	case -ENETUNREACH:
		return "ENETUNREACH";
	case -EAGAIN:
		return "EAGAIN";
	default:
		return "ERR";
	}
}

const char *mqtt_nbiot_state_to_str(int32_t state)
{
	return state_to_str((enum transport_state)state);
}

const char *mqtt_nbiot_active_broker_host(void)
{
	return cloud_config_get()->mqtt_host;
}

static void set_last_error(int err)
{
	atomic_set(&last_error, err);
}

static void reset_tn_unavailable_timer(void)
{
	tn_unavailable_since_ms = 0;
}

static void mark_tn_unavailable_now(void)
{
	if (tn_unavailable_since_ms == 0) {
		tn_unavailable_since_ms = k_uptime_get();
	}
}

static uint32_t tn_unavailable_elapsed_sec(void)
{
	if (tn_unavailable_since_ms == 0) {
		return 0;
	}

	return (uint32_t)((k_uptime_get() - tn_unavailable_since_ms) / 1000LL);
}

static void ntn_monthly_rollover_if_needed(void)
{
	int64_t now_ms = k_uptime_get();

	if (ntn_month_start_ms == 0 || (now_ms - ntn_month_start_ms) >= NTN_MONTH_MS) {
		ntn_month_start_ms = now_ms;
		ntn_monthly_bytes = 0;
	}
}

static bool ntn_budget_allows(uint32_t bytes, const struct cloud_runtime_config *cfg)
{
	ntn_monthly_rollover_if_needed();
	return (ntn_monthly_bytes + bytes) <= cfg->monthly_ntn_byte_limit;
}

static void ntn_account_bytes(uint32_t bytes)
{
	ntn_monthly_bytes += bytes;
}

static void lte_evt_handler(const struct lte_lc_evt *const evt)
{
	if (evt->type != LTE_LC_EVT_NW_REG_STATUS) {
		return;
	}

	atomic_set(&last_lte_reg_status, evt->nw_reg_status);

	if (registration_is_ready(evt->nw_reg_status)) {
		atomic_set(&lte_registered, 1);
		k_sem_give(&lte_registered_sem);
	} else {
		atomic_set(&lte_registered, 0);
	}
}

static int apply_lte_profile(enum radio_path path)
{
	enum lte_lc_system_mode mode;
	enum lte_lc_system_mode_preference pref;
	int err;
	const struct cloud_runtime_config *cfg = cloud_config_get();

	if (path == RADIO_PATH_TN) {
		err = cloud_config_get_lte_mode(&mode, &pref);
		if (err) {
			LOG_ERR("Invalid LTE mode: %s", cfg->lte_mode);
			return err;
		}
	} else {
		mode = LTE_LC_SYSTEM_MODE_NTN_NBIOT;
		pref = LTE_LC_SYSTEM_MODE_PREFER_AUTO;
	}

	err = lte_lc_offline();
	if (err) {
		LOG_WRN("lte_lc_offline failed before mode switch: %d", err);
	}

	err = lte_lc_system_mode_set(mode, pref);
	if (err) {
		LOG_ERR("lte_lc_system_mode_set failed: %d", err);
		return err;
	}

	k_sem_reset(&lte_registered_sem);
	atomic_set(&lte_registered, 0);
	atomic_set(&last_lte_reg_status, LTE_LC_NW_REG_UNKNOWN);

	err = lte_lc_connect_async(NULL);
	if (err && err != -EINPROGRESS) {
		LOG_ERR("lte_lc_connect_async failed after mode switch: %d", err);
		return err;
	}

	atomic_set(&active_radio_path, path);
	atomic_set(&lte_connect_started, 1);

	if (path == RADIO_PATH_TN) {
		LOG_INF("Applied TN profile: apn=%s mode=%s", cfg->apn, cfg->lte_mode);
	} else {
		LOG_INF("Applied NTN profile: mode=ntn_nbiot");
	}

	return 0;
}

static int bootstrap_modem_config(void)
{
	int err;
	const struct cloud_runtime_config *cfg = cloud_config_get();

	err = lte_lc_pdn_ctx_configure(0, cfg->apn, LTE_LC_PDN_FAM_IPV4V6, NULL);
	if (err) {
		LOG_ERR("lte_lc_pdn_ctx_configure failed: %d", err);
		return err;
	}

	err = lte_lc_connect_async(lte_evt_handler);
	if (err && err != -EINPROGRESS) {
		LOG_ERR("lte_lc_connect_async failed: %d", err);
		set_last_error(err);
		return err;
	}
	atomic_set(&lte_connect_started, 1);

	atomic_set(&active_radio_path, RADIO_PATH_TN);
	return apply_lte_profile(RADIO_PATH_TN);
}

static bool wait_for_registration_ms(int32_t wait_ms)
{
	enum lte_lc_nw_reg_status status;

	if (atomic_get(&lte_registered)) {
		return true;
	}

	if (lte_lc_nw_reg_status_get(&status) == 0) {
		atomic_set(&last_lte_reg_status, status);
		if (registration_is_ready(status)) {
			atomic_set(&lte_registered, 1);
			return true;
		}
	}

	if (k_sem_take(&lte_registered_sem, K_MSEC(wait_ms)) == 0) {
		return atomic_get(&lte_registered) != 0;
	}

	atomic_inc(&skip_lte_not_ready);
	return false;
}

static int mqtt_resolve_broker(struct sockaddr_storage *out)
{
	struct zsock_addrinfo hints = {
		.ai_family = AF_INET,
		.ai_socktype = SOCK_STREAM,
	};
	struct zsock_addrinfo *res;
	char port[8];
	int err;
	const struct cloud_runtime_config *cfg = cloud_config_get();

	snprintk(port, sizeof(port), "%u", cfg->mqtt_port);
	err = zsock_getaddrinfo(cfg->mqtt_host, port, &hints, &res);
	if (err) {
		return -EHOSTUNREACH;
	}

	memcpy(out, res->ai_addr, res->ai_addrlen);
	zsock_freeaddrinfo(res);
	return 0;
}

static int mqtt_sync_read_publish(const struct mqtt_publish_param *pub)
{
	size_t received = 0U;
	size_t total;
	int ret;

	total = pub->message.payload.len;
	if (total >= sizeof(mqtt_payload)) {
		total = sizeof(mqtt_payload) - 1U;
	}

	while (received < total) {
		ret = mqtt_read_publish_payload_blocking(&mqtt_client_ctx,
			mqtt_payload + received,
			total - received);
		if (ret < 0) {
			return ret;
		}
		received += (size_t)ret;
	}

	mqtt_payload[received] = '\0';
	mqtt_payload_ready = true;
	return 0;
}

static int mqtt_sync_subscribe(void)
{
	char topic_buf[MQTT_SUB_TOPIC_MAX];
	static struct mqtt_topic topic;
	struct mqtt_subscription_list list;
	int len;
	int err;

	len = snprintk(topic_buf, sizeof(topic_buf), "devices/%s/config", cloud_config_device_id_get());
	if (len <= 0 || len >= sizeof(topic_buf)) {
		return -EINVAL;
	}

	topic.topic.utf8 = (uint8_t *)topic_buf;
	topic.topic.size = len;
	topic.qos = MQTT_QOS_0_AT_MOST_ONCE;

	list.list = &topic;
	list.list_count = 1U;
	list.message_id = 1U;

	err = mqtt_subscribe(&mqtt_client_ctx, &list);
	if (!err) {
		LOG_INF("MQTT subscribed to retained config topic: %s", topic_buf);
	}
	return err;
}

static void mqtt_evt_handler(struct mqtt_client *const c, const struct mqtt_evt *evt)
{
	ARG_UNUSED(c);

	switch (evt->type) {
	case MQTT_EVT_CONNACK:
		if (evt->result == 0) {
			atomic_set(&mqtt_sync_connected, 1);
			(void)mqtt_sync_subscribe();
		} else {
			set_last_error(evt->result);
		}
		break;
	case MQTT_EVT_PUBLISH:
		(void)mqtt_sync_read_publish(&evt->param.publish);
		break;
	case MQTT_EVT_DISCONNECT:
		atomic_set(&mqtt_sync_connected, 0);
		break;
	default:
		break;
	}
}

static int mqtt_sync_once(void)
{
	const struct cloud_runtime_config *cfg = cloud_config_get();
	int err;
	int64_t deadline;
	char cid[64];
	size_t cid_len;

	if (!cfg->mqtt_sync_enabled) {
		return 0;
	}
	if (atomic_get(&active_radio_path) == RADIO_PATH_NTN && !cfg->mqtt_sync_force_in_ntn) {
		LOG_INF("MQTT config sync skipped on NTN path");
		return 0;
	}

	err = mqtt_resolve_broker(&mqtt_broker);
	if (err) {
		set_last_error(err);
		return err;
	}

	mqtt_client_init(&mqtt_client_ctx);
	mqtt_client_ctx.broker = &mqtt_broker;
	mqtt_client_ctx.evt_cb = mqtt_evt_handler;
	snprintk(cid, sizeof(cid), "%s-cfg", cloud_config_device_id_get());
	cid_len = strlen(cid);
	mqtt_client_ctx.client_id.utf8 = (uint8_t *)cid;
	mqtt_client_ctx.client_id.size = cid_len;
	mqtt_client_ctx.protocol_version = MQTT_VERSION_3_1_1;
	mqtt_client_ctx.keepalive = 30;
	mqtt_client_ctx.rx_buf = mqtt_rx_buf;
	mqtt_client_ctx.rx_buf_size = sizeof(mqtt_rx_buf);
	mqtt_client_ctx.tx_buf = mqtt_tx_buf;
	mqtt_client_ctx.tx_buf_size = sizeof(mqtt_tx_buf);
	mqtt_client_ctx.transport.type = MQTT_TRANSPORT_NON_SECURE;

	if (cfg->mqtt_username[0]) {
		static struct mqtt_utf8 user;
		user.utf8 = (uint8_t *)cfg->mqtt_username;
		user.size = strlen(cfg->mqtt_username);
		mqtt_client_ctx.user_name = &user;
	}
	if (cfg->mqtt_password[0]) {
		static struct mqtt_utf8 pass;
		pass.utf8 = (uint8_t *)cfg->mqtt_password;
		pass.size = strlen(cfg->mqtt_password);
		mqtt_client_ctx.password = &pass;
	}

	mqtt_payload_ready = false;
	atomic_set(&mqtt_sync_connected, 0);
	set_state(STATE_MQTT_SYNC);

	err = mqtt_connect(&mqtt_client_ctx);
	if (err) {
		set_last_error(err);
		return err;
	}

	mqtt_fds[0].fd = mqtt_client_ctx.transport.tcp.sock;
	mqtt_fds[0].events = ZSOCK_POLLIN;
	mqtt_fds[0].revents = 0;

	deadline = k_uptime_get() + 20000;
	while (k_uptime_get() < deadline) {
		int rc = zsock_poll(mqtt_fds, 1, 500);
		if (rc > 0 && (mqtt_fds[0].revents & ZSOCK_POLLIN)) {
			rc = mqtt_input(&mqtt_client_ctx);
			if (rc) {
				set_last_error(rc);
				break;
			}
		}
		(void)mqtt_live(&mqtt_client_ctx);
		if (mqtt_payload_ready) {
			break;
		}
	}

	if (mqtt_payload_ready) {
		bool applied = false;

		(void)cloud_config_apply_mqtt_retained(mqtt_payload, strlen(mqtt_payload), &applied);
		atomic_inc(&cfg_sync_ok);
		LOG_INF("MQTT config sync complete (retained=%s)", applied ? "new" : "same");
		err = 0;
	} else {
		atomic_inc(&cfg_sync_fail);
		err = -ETIMEDOUT;
	}

	(void)mqtt_abort(&mqtt_client_ctx);
	atomic_set(&mqtt_sync_connected, 0);
	last_sync_ms = k_uptime_get();
	set_last_error(err == -ETIMEDOUT ? err : 0);

	return err;
}

static int resolve_udp_server(struct sockaddr_storage *dst, socklen_t *dst_len)
{
	struct zsock_addrinfo hints = {
		.ai_family = AF_INET,
		.ai_socktype = SOCK_DGRAM,
	};
	struct zsock_addrinfo *res;
	char port[8];
	int err;
	const struct cloud_runtime_config *cfg = cloud_config_get();

	snprintk(port, sizeof(port), "%u", cfg->server_port);
	err = zsock_getaddrinfo(cfg->server_host, port, &hints, &res);
	if (err) {
		return -EHOSTUNREACH;
	}

	memcpy(dst, res->ai_addr, res->ai_addrlen);
	*dst_len = res->ai_addrlen;
	zsock_freeaddrinfo(res);
	return 0;
}

static int64_t get_unix_ts_s(void)
{
	return k_uptime_get() / 1000;
}

static bool json_find_key_value(const char *json, const char *key, const char **out)
{
	char pattern[48];
	char *p;

	snprintk(pattern, sizeof(pattern), "\"%s\"", key);
	p = strstr((char *)json, pattern);
	if (!p) {
		return false;
	}
	p = strchr(p, ':');
	if (!p) {
		return false;
	}
	p++;
	while (*p && isspace((unsigned char)*p)) {
		p++;
	}
	*out = p;
	return true;
}

static bool json_extract_int(const char *json, const char *key, int *out)
{
	const char *p;
	char *endp;
	long v;

	if (!json_find_key_value(json, key, &p)) {
		return false;
	}

	v = strtol(p, &endp, 10);
	if (p == endp) {
		return false;
	}

	*out = (int)v;
	return true;
}

static bool json_extract_string(const char *json, const char *key, char *out, size_t out_len)
{
	const char *p;
	const char *q;
	size_t len;

	if (!json_find_key_value(json, key, &p) || *p != '"') {
		return false;
	}
	p++;
	q = strchr(p, '"');
	if (!q) {
		return false;
	}
	len = MIN((size_t)(q - p), out_len - 1U);
	memcpy(out, p, len);
	out[len] = '\0';
	return true;
}

static void set_pending_ack(const char *cmd_id)
{
	k_mutex_lock(&ack_lock, K_FOREVER);
	if (cmd_id && cmd_id[0]) {
		strncpy(pending_ack_id, cmd_id, sizeof(pending_ack_id) - 1U);
		pending_ack_id[sizeof(pending_ack_id) - 1U] = '\0';
	}
	k_mutex_unlock(&ack_lock);
}

static bool pop_pending_ack(char *out, size_t out_len)
{
	bool have = false;

	k_mutex_lock(&ack_lock, K_FOREVER);
	if (pending_ack_id[0]) {
		strncpy(out, pending_ack_id, out_len - 1U);
		out[out_len - 1U] = '\0';
		pending_ack_id[0] = '\0';
		have = true;
	}
	k_mutex_unlock(&ack_lock);
	return have;
}

static void apply_backend_response(const char *rsp)
{
	int v;
	char s[64];
	char buf[24];

	if (!rsp || rsp[0] == '\0') {
		return;
	}

	if (json_extract_int(rsp, "cfg", &v)) {
		snprintk(buf, sizeof(buf), "%d", v);
		(void)cloud_config_set("config_version", buf);
	}
	if (json_extract_int(rsp, "publishIntervalSec", &v)) {
		snprintk(buf, sizeof(buf), "%d", v);
		(void)cloud_config_set("publish_interval_sec", buf);
	}
	if (json_extract_int(rsp, "gnssTimeoutSec", &v)) {
		snprintk(buf, sizeof(buf), "%d", v);
		(void)cloud_config_set("gnss_timeout_sec", buf);
	}
	if (json_extract_string(rsp, "dataMode", s, sizeof(s))) {
		(void)cloud_config_set("data_mode", s);
	}
	if (json_extract_string(rsp, "protocol", s, sizeof(s))) {
		(void)cloud_config_set("protocol", s);
	}
	if (json_extract_string(rsp, "host", s, sizeof(s))) {
		(void)cloud_config_set("server.host", s);
	}
	if (json_extract_int(rsp, "port", &v)) {
		snprintk(buf, sizeof(buf), "%d", v);
		(void)cloud_config_set("server.port", buf);
	}
	if (json_extract_string(rsp, "id", s, sizeof(s))) {
		set_pending_ack(s);
	}

	if (cloud_config_is_dirty()) {
		(void)cloud_config_save();
	}
}

static int build_payload_common(char *buf, size_t len, bool for_ntn)
{
	struct nrf_modem_gnss_pvt_data_frame pvt;
	const struct cloud_runtime_config *cfg = cloud_config_get();
	bool fix_valid;
	bool have_pvt;
	bool include_stale = false;
	char ack[ACK_ID_MAX] = {0};
	bool have_ack = false;
	uint32_t seq;

	k_mutex_lock(&pvt_lock, K_FOREVER);
	have_pvt = latest_has_pvt;
	pvt = latest_pvt;
	fix_valid = latest_fix_accepted;
	if (fix_valid) {
		last_good_fix = pvt;
		have_last_good_fix = true;
	} else if (have_last_good_fix) {
		pvt = last_good_fix;
		include_stale = true;
	}
	k_mutex_unlock(&pvt_lock);

	if (!have_pvt) {
		atomic_inc(&skip_no_fix);
		return -ENOENT;
	}

	seq = (uint32_t)atomic_inc(&seq_no) + 1U;

	if (for_ntn) {
		if (fix_valid || include_stale) {
			return snprintk(buf, len,
				"{\"id\":\"%s\",\"seq\":%u,\"fix\":%u,%s\"lat\":%.06f,\"lon\":%.06f,\"bat\":3800,\"ts\":%lld}",
				cfg->device_id,
				seq,
				fix_valid ? 1U : 0U,
				include_stale ? "\"stale\":1," : "",
				pvt.latitude,
				pvt.longitude,
				get_unix_ts_s());
		}

		return snprintk(buf, len,
			"{\"id\":\"%s\",\"seq\":%u,\"fix\":0,\"bat\":3800,\"ts\":%lld}",
			cfg->device_id,
			seq,
			get_unix_ts_s());
	}

	have_ack = pop_pending_ack(ack, sizeof(ack));
	if (fix_valid || include_stale) {
		if (have_ack) {
			return snprintk(buf, len,
				"{\"id\":\"%s\",\"seq\":%u,\"cfg\":%u,\"ack\":[\"%s\"],\"fix\":%u,\"lat\":%.06f,\"lon\":%.06f,\"acc\":%.01f,\"bat\":3800,\"ts\":%lld}",
				cfg->device_id,
				seq,
				cfg->config_version,
				ack,
				fix_valid ? 1U : 0U,
				pvt.latitude,
				pvt.longitude,
				(double)pvt.accuracy,
				get_unix_ts_s());
		}

		return snprintk(buf, len,
			"{\"id\":\"%s\",\"seq\":%u,\"cfg\":%u,\"fix\":%u,\"lat\":%.06f,\"lon\":%.06f,\"acc\":%.01f,\"bat\":3800,\"ts\":%lld}",
			cfg->device_id,
			seq,
			cfg->config_version,
			fix_valid ? 1U : 0U,
			pvt.latitude,
			pvt.longitude,
			(double)pvt.accuracy,
			get_unix_ts_s());
	}

	if (have_ack) {
		return snprintk(buf, len,
			"{\"id\":\"%s\",\"seq\":%u,\"cfg\":%u,\"ack\":[\"%s\"],\"fix\":0,\"bat\":3800,\"ts\":%lld}",
			cfg->device_id,
			seq,
			cfg->config_version,
			ack,
			get_unix_ts_s());
	}

	return snprintk(buf, len,
		"{\"id\":\"%s\",\"seq\":%u,\"cfg\":%u,\"fix\":0,\"bat\":3800,\"ts\":%lld}",
		cfg->device_id,
		seq,
		cfg->config_version,
		get_unix_ts_s());
}

static int udp_send_and_receive(bool using_ntn)
{
	struct sockaddr_storage dst;
	socklen_t dst_len;
	char payload[TELEMETRY_PAYLOAD_MAX];
	int sock;
	int err;
	int payload_len;
	int rc;
	size_t max_len;
	const struct cloud_runtime_config *cfg = cloud_config_get();

	err = resolve_udp_server(&dst, &dst_len);
	if (err) {
		set_last_error(err);
		return err;
	}

	max_len = using_ntn ? cfg->ntn_max_payload_bytes : cfg->max_payload_bytes;
	payload_len = build_payload_common(payload, MIN(sizeof(payload), max_len), using_ntn);
	if (payload_len <= 0 || payload_len >= sizeof(payload)) {
		return -EINVAL;
	}

	if (using_ntn && !ntn_budget_allows((uint32_t)payload_len, cfg)) {
		atomic_inc(&ntn_skipped_budget);
		LOG_WRN("NTN payload skipped, monthly byte budget reached (%u/%u)",
			ntn_monthly_bytes,
			cfg->monthly_ntn_byte_limit);
		return -EDQUOT;
	}

	sock = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock < 0) {
		return -errno;
	}

	rc = zsock_sendto(sock, payload, payload_len, 0, (struct sockaddr *)&dst, dst_len);
	if (rc < 0) {
		err = -errno;
		(void)zsock_close(sock);
		return err;
	}

	if (using_ntn) {
		atomic_inc(&ntn_sent_ok);
		ntn_account_bytes((uint32_t)payload_len);
		LOG_INF("NTN compact payload sent (%d bytes) to %s:%u", payload_len,
			cfg->server_host, cfg->server_port);
		(void)zsock_close(sock);
		return 0;
	}

	atomic_inc(&telemetry_ok);
	LOG_INF("Telemetry sent (%d bytes) to %s:%u proto=%s (send-only)",
		payload_len, cfg->server_host, cfg->server_port, cfg->protocol);

	(void)zsock_close(sock);
	return 0;
}

static bool should_sync_config_now(const struct cloud_runtime_config *cfg, bool force_boot)
{
	if (atomic_cas(&force_sync_requested, 1, 0)) {
		return true;
	}
	if (!cfg->mqtt_sync_enabled) {
		return false;
	}
	if ((atomic_get(&active_radio_path) == RADIO_PATH_NTN) && !cfg->mqtt_sync_force_in_ntn) {
		return false;
	}
	if (force_boot) {
		return true;
	}
	if ((k_uptime_get() - last_sync_ms) >= ((int64_t)cfg->mqtt_sync_interval_sec * 1000LL)) {
		return true;
	}
	return false;
}

static bool tn_should_fallback_to_ntn(const struct cloud_runtime_config *cfg)
{
	if (!cfg->tn_enabled || !cfg->ntn_enabled) {
		return false;
	}
	if (!data_mode_is_auto(cfg)) {
		return false;
	}
	if (tn_unavailable_since_ms == 0) {
		return false;
	}

	return (k_uptime_get() - tn_unavailable_since_ms) >=
		((int64_t)cfg->tn_unavailable_timeout_sec * 1000LL);
}

static bool try_tn_connect_window(const struct cloud_runtime_config *cfg, int32_t wait_ms)
{
	ARG_UNUSED(cfg);
	set_state(STATE_TN_CONNECTING);

	if (wait_for_registration_ms(wait_ms)) {
		set_state(STATE_TN_AVAILABLE);
		reset_tn_unavailable_timer();
		next_retry_backoff_ms = 0;
		return true;
	}

	mark_tn_unavailable_now();
	return false;
}

static void enter_ntn_fallback_cycle(void)
{
	set_state(STATE_TN_UNAVAILABLE);
	set_state(STATE_NTN_FALLBACK_ARMED);
	ntn_attempted_in_cycle = false;
}

static int maybe_send_ntn_fallback(const struct cloud_runtime_config *cfg)
{
	int err;

	if (!cfg->ntn_enabled) {
		return -EACCES;
	}
	if (ntn_attempted_in_cycle) {
		return -EALREADY;
	}
	if (ntn_last_send_ms != 0 &&
	    (k_uptime_get() - ntn_last_send_ms) < ((int64_t)cfg->ntn_min_send_interval_sec * 1000LL)) {
		atomic_inc(&ntn_skipped_interval);
		return -EAGAIN;
	}

	set_state(STATE_NTN_SENDING);
	err = udp_send_and_receive(true);
	ntn_attempted_in_cycle = true;
	if (err == 0) {
		ntn_last_send_ms = k_uptime_get();
		set_state(STATE_NTN_SENT);
		set_last_error(0);
		return 0;
	}

	atomic_inc(&ntn_sent_fail);
	set_state(STATE_NTN_FAILED);
	set_last_error(err);
	return err;
}

static void transport_thread_fn(void)
{
	const struct cloud_runtime_config *cfg;
	bool boot_sync_pending = true;
	bool warned_ntn_only = false;

	set_state(STATE_BOOT);

	for (;;) {
		int err;
		bool tn_ok;

		cfg = cloud_config_get();

		if (data_mode_is_ntn_only(cfg) && !warned_ntn_only) {
			LOG_WRN("data_mode=ntn_only active: expensive path for test use only");
			warned_ntn_only = true;
		}

		if (data_mode_is_ntn_only(cfg)) {
			if (atomic_get(&active_radio_path) != RADIO_PATH_NTN) {
				(void)apply_lte_profile(RADIO_PATH_NTN);
			}
			(void)wait_for_registration_ms(20000);
			err = maybe_send_ntn_fallback(cfg);
			if (err && err != -EALREADY && err != -EAGAIN && err != -EDQUOT) {
				next_retry_backoff_ms = MIN(next_retry_backoff_ms == 0 ?
					(int64_t)CONFIG_GNSS_SAMPLE_TELEMETRY_BACKOFF_BASE_SECONDS * 1000LL :
					next_retry_backoff_ms * 2,
					(int64_t)CONFIG_GNSS_SAMPLE_TELEMETRY_BACKOFF_MAX_SECONDS * 1000LL);
				set_state(STATE_SLEEP_BACKOFF);
				k_sleep(K_MSEC(next_retry_backoff_ms));
			} else {
				k_sleep(K_SECONDS(5));
			}
			continue;
		}

		if (atomic_get(&active_radio_path) == RADIO_PATH_NTN) {
			if (k_uptime_get() >= next_tn_recheck_ms && cfg->tn_enabled) {
				set_state(STATE_RETURN_TO_TN);
				err = apply_lte_profile(RADIO_PATH_TN);
				if (!err && try_tn_connect_window(cfg, 30000)) {
					LOG_INF("TN restored, returning from NTN fallback");
					atomic_set(&active_radio_path, RADIO_PATH_TN);
					next_tn_recheck_ms = 0;
					ntn_attempted_in_cycle = false;
					continue;
				}

				(void)apply_lte_profile(RADIO_PATH_NTN);
				next_tn_recheck_ms = k_uptime_get() + (int64_t)TN_RETRY_WHILE_ON_NTN_SEC * 1000LL;
				enter_ntn_fallback_cycle();
			}

			err = maybe_send_ntn_fallback(cfg);
			if (err && err != -EALREADY && err != -EAGAIN && err != -EDQUOT) {
				next_retry_backoff_ms = MIN(next_retry_backoff_ms == 0 ?
					(int64_t)CONFIG_GNSS_SAMPLE_TELEMETRY_BACKOFF_BASE_SECONDS * 1000LL :
					next_retry_backoff_ms * 2,
					(int64_t)CONFIG_GNSS_SAMPLE_TELEMETRY_BACKOFF_MAX_SECONDS * 1000LL);
				set_state(STATE_SLEEP_BACKOFF);
				k_sleep(K_MSEC(next_retry_backoff_ms));
			}

			k_sleep(K_SECONDS(2));
			continue;
		}

		tn_ok = try_tn_connect_window(cfg,
			CONFIG_GNSS_SAMPLE_MQTT_REGISTRATION_TIMEOUT_SECONDS * 1000);
		if (!tn_ok) {
			if (tn_should_fallback_to_ntn(cfg) && !data_mode_is_tn_only(cfg)) {
				enter_ntn_fallback_cycle();
				err = apply_lte_profile(RADIO_PATH_NTN);
				if (!err) {
					next_tn_recheck_ms = k_uptime_get() + (int64_t)TN_RETRY_WHILE_ON_NTN_SEC * 1000LL;
					continue;
				}
				set_last_error(err);
			}

			atomic_inc(&skip_lte_not_ready);
			k_sleep(K_SECONDS(2));
			continue;
		}

		if (last_telemetry_ms != 0 &&
		    (k_uptime_get() - last_telemetry_ms) < ((int64_t)cfg->publish_interval_sec * 1000LL)) {
			atomic_inc(&skip_interval);
			k_sleep(K_SECONDS(1));
			continue;
		}

		err = udp_send_and_receive(false);
		if (err == 0) {
			last_telemetry_ms = k_uptime_get();
			next_retry_backoff_ms = 0;
			set_last_error(0);
			reset_tn_unavailable_timer();
			set_state(STATE_TN_AVAILABLE);
		} else {
			atomic_inc(&telemetry_fail);
			if (is_network_unavailable_err(err)) {
				mark_tn_unavailable_now();
			}
			next_retry_backoff_ms = MIN(next_retry_backoff_ms == 0 ?
				(int64_t)CONFIG_GNSS_SAMPLE_TELEMETRY_BACKOFF_BASE_SECONDS * 1000LL :
				next_retry_backoff_ms * 2,
				(int64_t)CONFIG_GNSS_SAMPLE_TELEMETRY_BACKOFF_MAX_SECONDS * 1000LL);
			set_state(STATE_SLEEP_BACKOFF);
			k_sleep(K_MSEC(next_retry_backoff_ms));
		}

		k_sleep(K_SECONDS(1));
	}
}

int mqtt_nbiot_transport_init(void)
{
	int err;

	err = cloud_config_init();
	if (err) {
		LOG_ERR("cloud_config_init failed: %d", err);
		return err;
	}

	err = bootstrap_modem_config();
	if (err) {
		return err;
	}

	atomic_set(&last_lte_reg_status, LTE_LC_NW_REG_UNKNOWN);
	atomic_set(&transport_state, STATE_BOOT);
	atomic_set(&seq_no, 0);
	atomic_set(&active_radio_path, RADIO_PATH_TN);
	k_mutex_init(&pvt_lock);
	k_mutex_init(&ack_lock);
	pending_ack_id[0] = '\0';
	tn_unavailable_since_ms = 0;
	next_tn_recheck_ms = 0;
	ntn_month_start_ms = k_uptime_get();
	ntn_monthly_bytes = 0;
	ntn_attempted_in_cycle = false;

	transport_tid = k_thread_create(&transport_thread,
					transport_stack,
					K_THREAD_STACK_SIZEOF(transport_stack),
					(k_thread_entry_t)transport_thread_fn,
					NULL, NULL, NULL,
					CONFIG_GNSS_SAMPLE_MQTT_THREAD_PRIORITY,
					0, K_NO_WAIT);
	k_thread_name_set(transport_tid, "telemetry_low_data");

	LOG_INF("Hybrid TN/NTN transport started: telemetry=%s://%s:%u config-sync mqtt=%s:%u",
		cloud_config_get()->protocol,
		cloud_config_get()->server_host,
		cloud_config_get()->server_port,
		cloud_config_get()->mqtt_host,
		cloud_config_get()->mqtt_port);

	return 0;
}

void mqtt_nbiot_transport_handle_pvt(const struct nrf_modem_gnss_pvt_data_frame *pvt,
					     bool accepted_fix)
{
	if (!pvt) {
		return;
	}

	k_mutex_lock(&pvt_lock, K_FOREVER);
	latest_pvt = *pvt;
	latest_has_pvt = true;
	latest_fix_accepted = accepted_fix;
	if (accepted_fix) {
		last_good_fix = *pvt;
		have_last_good_fix = true;
	}
	k_mutex_unlock(&pvt_lock);
}

void mqtt_nbiot_transport_get_status(struct mqtt_nbiot_status *status)
{
	const struct cloud_runtime_config *cfg;

	if (!status) {
		return;
	}

	cfg = cloud_config_get();

	memset(status, 0, sizeof(*status));
	status->lte_registered = atomic_get(&lte_registered) != 0;
	status->mqtt_sync_connected = atomic_get(&mqtt_sync_connected) != 0;
	status->last_fix_valid = latest_fix_accepted;
	status->config_sync_ok = (uint32_t)atomic_get(&cfg_sync_ok);
	status->config_sync_fail = (uint32_t)atomic_get(&cfg_sync_fail);
	status->telemetry_sent_ok = (uint32_t)atomic_get(&telemetry_ok);
	status->telemetry_sent_fail = (uint32_t)atomic_get(&telemetry_fail);
	status->backend_rsp_ok = (uint32_t)atomic_get(&backend_rsp_ok);
	status->backend_rsp_fail = (uint32_t)atomic_get(&backend_rsp_fail);
	status->queued_samples = latest_has_pvt ? 1U : 0U;
	status->dropped_samples = (uint32_t)atomic_get(&dropped_samples);
	status->skipped_lte_not_ready = (uint32_t)atomic_get(&skip_lte_not_ready);
	status->skipped_interval = (uint32_t)atomic_get(&skip_interval);
	status->skipped_no_fix = (uint32_t)atomic_get(&skip_no_fix);
	status->ntn_sent_ok = (uint32_t)atomic_get(&ntn_sent_ok);
	status->ntn_sent_fail = (uint32_t)atomic_get(&ntn_sent_fail);
	status->ntn_skipped_interval = (uint32_t)atomic_get(&ntn_skipped_interval);
	status->ntn_skipped_budget = (uint32_t)atomic_get(&ntn_skipped_budget);
	status->ntn_bytes_this_month = ntn_monthly_bytes;
	status->tn_unavailable_for_sec = tn_unavailable_elapsed_sec();
	status->seq = (uint32_t)atomic_get(&seq_no);
	status->state = atomic_get(&transport_state);
	status->last_error = atomic_get(&last_error);
	status->last_lte_reg_status = atomic_get(&last_lte_reg_status);
	status->config_version = cfg->config_version;
	status->server_port = cfg->server_port;
	strncpy(status->server_host, cfg->server_host, sizeof(status->server_host) - 1U);
	strncpy(status->protocol, cfg->protocol, sizeof(status->protocol) - 1U);
	strncpy(status->data_mode, cfg->data_mode, sizeof(status->data_mode) - 1U);
	strncpy(status->active_path,
		atomic_get(&active_radio_path) == RADIO_PATH_NTN ? "ntn" : "tn",
		sizeof(status->active_path) - 1U);
}

#if defined(CONFIG_SHELL)
static int cmd_config_refresh(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	atomic_set(&force_sync_requested, 1);
	shell_print(sh, "Scheduled immediate MQTT retained config refresh.");
	return 0;
}

SHELL_CMD_REGISTER(config_refresh, NULL, "Force retained config MQTT sync now", cmd_config_refresh);
#endif
