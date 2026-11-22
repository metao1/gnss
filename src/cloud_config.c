/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "cloud_config.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>

#if defined(CONFIG_SHELL)
#include <zephyr/shell/shell.h>
#include <zephyr/sys/reboot.h>
#endif

LOG_MODULE_DECLARE(gnss_sample, CONFIG_GNSS_SAMPLE_LOG_LEVEL);

enum config_key_id {
	KEY_DEVICE_ID,
	KEY_SERVER_HOST,
	KEY_SERVER_PORT,
	KEY_PROTOCOL,
	KEY_TELEMETRY_PROTOCOL,
	KEY_PUBLISH_INTERVAL_SEC,
	KEY_GNSS_TIMEOUT_SEC,
	KEY_DATA_MODE,
	KEY_TN_ENABLED,
	KEY_NTN_ENABLED,
	KEY_TN_UNAVAILABLE_TIMEOUT_SEC,
	KEY_NTN_MIN_SEND_INTERVAL_SEC,
	KEY_NTN_MAX_PAYLOAD_BYTES,
	KEY_MONTHLY_NTN_BYTE_LIMIT,
	KEY_CONFIG_VERSION,
	KEY_AUTH_TOKEN,
	KEY_MAX_PAYLOAD_BYTES,
	KEY_ENABLE_DEBUG_LOGS,
	KEY_MQTT_HOST,
	KEY_MQTT_PORT,
	KEY_MQTT_USERNAME,
	KEY_MQTT_PASSWORD,
	KEY_MQTT_TLS_SEC_TAG,
	KEY_MQTT_SYNC_INTERVAL_SEC,
	KEY_MQTT_SYNC_ENABLED,
	KEY_MQTT_SYNC_FORCE_IN_NTN,
	KEY_APN,
	KEY_LTE_MODE,
	KEY_INVALID,
};

struct key_map_entry {
	enum config_key_id id;
	const char *name;
};

static const struct key_map_entry runtime_key_map[] = {
	{ KEY_DEVICE_ID, "device.id" },
	{ KEY_DEVICE_ID, "device_id" },
	{ KEY_SERVER_HOST, "server.host" },
	{ KEY_SERVER_HOST, "server_host" },
	{ KEY_SERVER_PORT, "server.port" },
	{ KEY_SERVER_PORT, "server_port" },
	{ KEY_PROTOCOL, "protocol" },
	{ KEY_TELEMETRY_PROTOCOL, "telemetry_protocol" },
	{ KEY_PUBLISH_INTERVAL_SEC, "publish.interval_seconds" },
	{ KEY_PUBLISH_INTERVAL_SEC, "publish_interval_sec" },
	{ KEY_GNSS_TIMEOUT_SEC, "gnss_timeout_sec" },
	{ KEY_DATA_MODE, "data_mode" },
	{ KEY_TN_ENABLED, "tn_enabled" },
	{ KEY_NTN_ENABLED, "ntn_enabled" },
	{ KEY_TN_UNAVAILABLE_TIMEOUT_SEC, "tn_unavailable_timeout_sec" },
	{ KEY_NTN_MIN_SEND_INTERVAL_SEC, "ntn_min_send_interval_sec" },
	{ KEY_NTN_MAX_PAYLOAD_BYTES, "ntn_max_payload_bytes" },
	{ KEY_MONTHLY_NTN_BYTE_LIMIT, "monthly_ntn_byte_limit" },
	{ KEY_CONFIG_VERSION, "config_version" },
	{ KEY_AUTH_TOKEN, "auth_token" },
	{ KEY_MAX_PAYLOAD_BYTES, "max_payload_bytes" },
	{ KEY_ENABLE_DEBUG_LOGS, "enable_debug_logs" },
	{ KEY_MQTT_HOST, "mqtt.host" },
	{ KEY_MQTT_PORT, "mqtt.port" },
	{ KEY_MQTT_USERNAME, "mqtt.username" },
	{ KEY_MQTT_PASSWORD, "mqtt.password" },
	{ KEY_MQTT_TLS_SEC_TAG, "mqtt.tls_sec_tag" },
	{ KEY_MQTT_SYNC_INTERVAL_SEC, "mqtt.sync_interval_sec" },
	{ KEY_MQTT_SYNC_ENABLED, "mqtt.sync_enabled" },
	{ KEY_MQTT_SYNC_FORCE_IN_NTN, "mqtt.sync_force_in_ntn" },
	{ KEY_APN, "apn" },
	{ KEY_LTE_MODE, "lte.mode" },
};

static const struct key_map_entry settings_key_map[] = {
	{ KEY_DEVICE_ID, "device_id" },
	{ KEY_SERVER_HOST, "server_host" },
	{ KEY_SERVER_PORT, "server_port" },
	{ KEY_PROTOCOL, "protocol" },
	{ KEY_TELEMETRY_PROTOCOL, "telemetry_protocol" },
	{ KEY_PUBLISH_INTERVAL_SEC, "publish_interval_sec" },
	{ KEY_GNSS_TIMEOUT_SEC, "gnss_timeout_sec" },
	{ KEY_DATA_MODE, "data_mode" },
	{ KEY_TN_ENABLED, "tn_enabled" },
	{ KEY_NTN_ENABLED, "ntn_enabled" },
	{ KEY_TN_UNAVAILABLE_TIMEOUT_SEC, "tn_unavailable_timeout_sec" },
	{ KEY_NTN_MIN_SEND_INTERVAL_SEC, "ntn_min_send_interval_sec" },
	{ KEY_NTN_MAX_PAYLOAD_BYTES, "ntn_max_payload_bytes" },
	{ KEY_MONTHLY_NTN_BYTE_LIMIT, "monthly_ntn_byte_limit" },
	{ KEY_CONFIG_VERSION, "config_version" },
	{ KEY_AUTH_TOKEN, "auth_token" },
	{ KEY_MAX_PAYLOAD_BYTES, "max_payload_bytes" },
	{ KEY_ENABLE_DEBUG_LOGS, "enable_debug_logs" },
	{ KEY_MQTT_HOST, "mqtt_host" },
	{ KEY_MQTT_PORT, "mqtt_port" },
	{ KEY_MQTT_USERNAME, "mqtt_username" },
	{ KEY_MQTT_PASSWORD, "mqtt_password" },
	{ KEY_MQTT_TLS_SEC_TAG, "mqtt_tls_sec_tag" },
	{ KEY_MQTT_SYNC_INTERVAL_SEC, "mqtt_sync_interval_sec" },
	{ KEY_MQTT_SYNC_ENABLED, "mqtt_sync_enabled" },
	{ KEY_MQTT_SYNC_FORCE_IN_NTN, "mqtt_sync_force_in_ntn" },
	{ KEY_APN, "apn" },
	{ KEY_LTE_MODE, "lte_mode" },
};

static struct cloud_runtime_config runtime_cfg;
static bool cfg_initialized;
static bool cfg_dirty;
static bool cfg_reboot_required;
K_MUTEX_DEFINE(cfg_lock);

static int set_string(char *dst, size_t dst_len, const char *src)
{
	size_t src_len;

	if (!dst || !src || dst_len == 0U) {
		return -EINVAL;
	}

	src_len = strlen(src);
	if (src_len >= dst_len) {
		return -E2BIG;
	}

	memcpy(dst, src, src_len + 1U);
	return 0;
}

static bool parse_bool(const char *value, bool *out)
{
	if (!value || !out) {
		return false;
	}

	if ((strcmp(value, "1") == 0) || (strcasecmp(value, "true") == 0) ||
	    (strcasecmp(value, "yes") == 0) || (strcasecmp(value, "on") == 0)) {
		*out = true;
		return true;
	}

	if ((strcmp(value, "0") == 0) || (strcasecmp(value, "false") == 0) ||
	    (strcasecmp(value, "no") == 0) || (strcasecmp(value, "off") == 0)) {
		*out = false;
		return true;
	}

	return false;
}

static bool lte_mode_valid(const char *value)
{
	return (strcmp(value, "nbiot_gps") == 0) ||
	       (strcmp(value, "lte_m_gps") == 0) ||
	       (strcmp(value, "lte_m_nbiot_gps") == 0) ||
	       (strcmp(value, "ntn_nbiot") == 0);
}

static bool protocol_valid(const char *value)
{
	return (strcmp(value, "udp") == 0) || (strcmp(value, "coap") == 0);
}

static bool data_mode_valid(const char *value)
{
	return (strcmp(value, "tn") == 0) || (strcmp(value, "ntn") == 0) ||
	       (strcmp(value, "auto") == 0) || (strcmp(value, "tn_only") == 0) ||
	       (strcmp(value, "ntn_only") == 0);
}

static const char *normalize_data_mode(const char *value)
{
	if (strcmp(value, "tn") == 0) {
		return "tn_only";
	}
	if (strcmp(value, "ntn") == 0) {
		return "ntn_only";
	}

	return value;
}

static enum config_key_id lookup_key_id(const char *key,
					const struct key_map_entry *map,
					size_t map_len)
{
	for (size_t i = 0; i < map_len; i++) {
		if (strcmp(key, map[i].name) == 0) {
			return map[i].id;
		}
	}

	return KEY_INVALID;
}

static int apply_key_value(enum config_key_id id, const char *value,
			   bool mark_dirty, bool mark_reboot)
{
	char *endp;
	unsigned long parsed_ul;
	long parsed_l;
	bool bval;
	int err = 0;

	if (!value) {
		return -EINVAL;
	}

	switch (id) {
	case KEY_DEVICE_ID:
		err = set_string(runtime_cfg.device_id, sizeof(runtime_cfg.device_id), value);
		break;
	case KEY_SERVER_HOST:
		err = set_string(runtime_cfg.server_host, sizeof(runtime_cfg.server_host), value);
		break;
	case KEY_SERVER_PORT:
		parsed_ul = strtoul(value, &endp, 10);
		if (*value == '\0' || *endp != '\0' || parsed_ul < 1UL || parsed_ul > 65535UL) {
			return -EINVAL;
		}
		runtime_cfg.server_port = (uint16_t)parsed_ul;
		break;
	case KEY_PROTOCOL:
		if (!protocol_valid(value)) {
			return -EINVAL;
		}
		err = set_string(runtime_cfg.protocol, sizeof(runtime_cfg.protocol), value);
		if (!err) {
			err = set_string(runtime_cfg.telemetry_protocol,
					 sizeof(runtime_cfg.telemetry_protocol), value);
		}
		break;
	case KEY_TELEMETRY_PROTOCOL:
		if (!protocol_valid(value)) {
			return -EINVAL;
		}
		err = set_string(runtime_cfg.protocol, sizeof(runtime_cfg.protocol), value);
		if (!err) {
			err = set_string(runtime_cfg.telemetry_protocol,
					 sizeof(runtime_cfg.telemetry_protocol), value);
		}
		break;
	case KEY_PUBLISH_INTERVAL_SEC:
		parsed_ul = strtoul(value, &endp, 10);
		if (*value == '\0' || *endp != '\0' || parsed_ul < 30UL || parsed_ul > 86400UL) {
			return -EINVAL;
		}
		runtime_cfg.publish_interval_sec = (uint32_t)parsed_ul;
		break;
	case KEY_GNSS_TIMEOUT_SEC:
		parsed_ul = strtoul(value, &endp, 10);
		if (*value == '\0' || *endp != '\0' || parsed_ul < 10UL || parsed_ul > 3600UL) {
			return -EINVAL;
		}
		runtime_cfg.gnss_timeout_sec = (uint32_t)parsed_ul;
		break;
	case KEY_DATA_MODE:
		if (!data_mode_valid(value)) {
			return -EINVAL;
		}
		err = set_string(runtime_cfg.data_mode, sizeof(runtime_cfg.data_mode),
				 normalize_data_mode(value));
		break;
	case KEY_TN_ENABLED:
		if (!parse_bool(value, &bval)) {
			return -EINVAL;
		}
		runtime_cfg.tn_enabled = bval;
		break;
	case KEY_NTN_ENABLED:
		if (!parse_bool(value, &bval)) {
			return -EINVAL;
		}
		runtime_cfg.ntn_enabled = bval;
		break;
	case KEY_TN_UNAVAILABLE_TIMEOUT_SEC:
		parsed_ul = strtoul(value, &endp, 10);
		if (*value == '\0' || *endp != '\0' || parsed_ul < 60UL || parsed_ul > 86400UL) {
			return -EINVAL;
		}
		runtime_cfg.tn_unavailable_timeout_sec = (uint32_t)parsed_ul;
		break;
	case KEY_NTN_MIN_SEND_INTERVAL_SEC:
		parsed_ul = strtoul(value, &endp, 10);
		if (*value == '\0' || *endp != '\0' || parsed_ul < 300UL || parsed_ul > 2592000UL) {
			return -EINVAL;
		}
		runtime_cfg.ntn_min_send_interval_sec = (uint32_t)parsed_ul;
		break;
	case KEY_NTN_MAX_PAYLOAD_BYTES:
		parsed_ul = strtoul(value, &endp, 10);
		if (*value == '\0' || *endp != '\0' || parsed_ul < 64UL || parsed_ul > 512UL) {
			return -EINVAL;
		}
		runtime_cfg.ntn_max_payload_bytes = (uint16_t)parsed_ul;
		break;
	case KEY_MONTHLY_NTN_BYTE_LIMIT:
		parsed_ul = strtoul(value, &endp, 10);
		if (*value == '\0' || *endp != '\0' || parsed_ul < 1024UL || parsed_ul > 500000UL) {
			return -EINVAL;
		}
		runtime_cfg.monthly_ntn_byte_limit = (uint32_t)parsed_ul;
		break;
	case KEY_CONFIG_VERSION:
		parsed_ul = strtoul(value, &endp, 10);
		if (*value == '\0' || *endp != '\0') {
			return -EINVAL;
		}
		runtime_cfg.config_version = (uint32_t)parsed_ul;
		break;
	case KEY_AUTH_TOKEN:
		err = set_string(runtime_cfg.auth_token, sizeof(runtime_cfg.auth_token), value);
		break;
	case KEY_MAX_PAYLOAD_BYTES:
		parsed_ul = strtoul(value, &endp, 10);
		if (*value == '\0' || *endp != '\0' || parsed_ul < 64UL || parsed_ul > 1200UL) {
			return -EINVAL;
		}
		runtime_cfg.max_payload_bytes = (uint16_t)parsed_ul;
		break;
	case KEY_ENABLE_DEBUG_LOGS:
		if (!parse_bool(value, &bval)) {
			return -EINVAL;
		}
		runtime_cfg.enable_debug_logs = bval;
		break;
	case KEY_MQTT_HOST:
		err = set_string(runtime_cfg.mqtt_host, sizeof(runtime_cfg.mqtt_host), value);
		break;
	case KEY_MQTT_PORT:
		parsed_ul = strtoul(value, &endp, 10);
		if (*value == '\0' || *endp != '\0' || parsed_ul < 1UL || parsed_ul > 65535UL) {
			return -EINVAL;
		}
		runtime_cfg.mqtt_port = (uint16_t)parsed_ul;
		break;
	case KEY_MQTT_USERNAME:
		err = set_string(runtime_cfg.mqtt_username, sizeof(runtime_cfg.mqtt_username), value);
		break;
	case KEY_MQTT_PASSWORD:
		err = set_string(runtime_cfg.mqtt_password, sizeof(runtime_cfg.mqtt_password), value);
		break;
	case KEY_MQTT_TLS_SEC_TAG:
		parsed_l = strtol(value, &endp, 10);
		if (*value == '\0' || *endp != '\0' || parsed_l < 0L || parsed_l > INT32_MAX) {
			return -EINVAL;
		}
		runtime_cfg.mqtt_tls_sec_tag = (int32_t)parsed_l;
		break;
	case KEY_MQTT_SYNC_INTERVAL_SEC:
		parsed_ul = strtoul(value, &endp, 10);
		if (*value == '\0' || *endp != '\0' || parsed_ul < 300UL || parsed_ul > 172800UL) {
			return -EINVAL;
		}
		runtime_cfg.mqtt_sync_interval_sec = (uint32_t)parsed_ul;
		break;
	case KEY_MQTT_SYNC_ENABLED:
		if (!parse_bool(value, &bval)) {
			return -EINVAL;
		}
		runtime_cfg.mqtt_sync_enabled = bval;
		break;
	case KEY_MQTT_SYNC_FORCE_IN_NTN:
		if (!parse_bool(value, &bval)) {
			return -EINVAL;
		}
		runtime_cfg.mqtt_sync_force_in_ntn = bval;
		break;
	case KEY_APN:
		err = set_string(runtime_cfg.apn, sizeof(runtime_cfg.apn), value);
		break;
	case KEY_LTE_MODE:
		if (!lte_mode_valid(value)) {
			return -EINVAL;
		}
		err = set_string(runtime_cfg.lte_mode, sizeof(runtime_cfg.lte_mode), value);
		break;
	case KEY_INVALID:
	default:
		return -ENOENT;
	}

	if (err) {
		return err;
	}

	if (mark_dirty) {
		cfg_dirty = true;
	}
	if (mark_reboot) {
		cfg_reboot_required = true;
	}

	return 0;
}

static void set_defaults(void)
{
	(void)set_string(runtime_cfg.device_id, sizeof(runtime_cfg.device_id),
			 CONFIG_GNSS_SAMPLE_MQTT_DEVICE_ID);
	(void)set_string(runtime_cfg.server_host, sizeof(runtime_cfg.server_host),
			 CONFIG_GNSS_SAMPLE_TELEMETRY_SERVER_HOST);
	runtime_cfg.server_port = (uint16_t)CONFIG_GNSS_SAMPLE_TELEMETRY_SERVER_PORT;
	(void)set_string(runtime_cfg.protocol, sizeof(runtime_cfg.protocol),
			 CONFIG_GNSS_SAMPLE_TELEMETRY_PROTOCOL);
	(void)set_string(runtime_cfg.telemetry_protocol, sizeof(runtime_cfg.telemetry_protocol),
			 CONFIG_GNSS_SAMPLE_TELEMETRY_PROTOCOL);
	runtime_cfg.publish_interval_sec = CONFIG_GNSS_SAMPLE_TELEMETRY_PUBLISH_INTERVAL_SECONDS;
	runtime_cfg.gnss_timeout_sec = CONFIG_GNSS_SAMPLE_GNSS_TIMEOUT_SECONDS;
	(void)set_string(runtime_cfg.data_mode, sizeof(runtime_cfg.data_mode),
			 CONFIG_GNSS_SAMPLE_DATA_MODE_DEFAULT);
	runtime_cfg.tn_enabled = IS_ENABLED(CONFIG_GNSS_SAMPLE_TN_ENABLED_DEFAULT);
	runtime_cfg.ntn_enabled = IS_ENABLED(CONFIG_GNSS_SAMPLE_NTN_ENABLED_DEFAULT);
	runtime_cfg.tn_unavailable_timeout_sec =
		CONFIG_GNSS_SAMPLE_TN_UNAVAILABLE_TIMEOUT_SECONDS;
	runtime_cfg.ntn_min_send_interval_sec =
		CONFIG_GNSS_SAMPLE_NTN_MIN_SEND_INTERVAL_SECONDS;
	runtime_cfg.ntn_max_payload_bytes =
		CONFIG_GNSS_SAMPLE_NTN_MAX_PAYLOAD_BYTES_DEFAULT;
	runtime_cfg.monthly_ntn_byte_limit =
		CONFIG_GNSS_SAMPLE_MONTHLY_NTN_BYTE_LIMIT_DEFAULT;
	runtime_cfg.config_version = CONFIG_GNSS_SAMPLE_CONFIG_VERSION_DEFAULT;
	(void)set_string(runtime_cfg.auth_token, sizeof(runtime_cfg.auth_token),
			 CONFIG_GNSS_SAMPLE_AUTH_TOKEN_DEFAULT);
	runtime_cfg.max_payload_bytes = CONFIG_GNSS_SAMPLE_MAX_PAYLOAD_BYTES_DEFAULT;
	runtime_cfg.enable_debug_logs = IS_ENABLED(CONFIG_GNSS_SAMPLE_ENABLE_DEBUG_LOGS_DEFAULT);

	(void)set_string(runtime_cfg.mqtt_host, sizeof(runtime_cfg.mqtt_host),
			 CONFIG_GNSS_SAMPLE_MQTT_BROKER_HOSTNAME);
	runtime_cfg.mqtt_port = (uint16_t)CONFIG_GNSS_SAMPLE_MQTT_BROKER_PORT;
	(void)set_string(runtime_cfg.mqtt_username, sizeof(runtime_cfg.mqtt_username),
			 CONFIG_GNSS_SAMPLE_MQTT_USERNAME);
	(void)set_string(runtime_cfg.mqtt_password, sizeof(runtime_cfg.mqtt_password),
			 CONFIG_GNSS_SAMPLE_MQTT_PASSWORD);
#if defined(CONFIG_GNSS_SAMPLE_MQTT_TLS_SEC_TAG)
	runtime_cfg.mqtt_tls_sec_tag = CONFIG_GNSS_SAMPLE_MQTT_TLS_SEC_TAG;
#else
	runtime_cfg.mqtt_tls_sec_tag = 0;
#endif
	runtime_cfg.mqtt_sync_interval_sec = CONFIG_GNSS_SAMPLE_MQTT_SYNC_INTERVAL_SECONDS;
	runtime_cfg.mqtt_sync_enabled = IS_ENABLED(CONFIG_GNSS_SAMPLE_MQTT_SYNC_ENABLED);
	runtime_cfg.mqtt_sync_force_in_ntn = IS_ENABLED(CONFIG_GNSS_SAMPLE_MQTT_SYNC_FORCE_IN_NTN);

	(void)set_string(runtime_cfg.apn, sizeof(runtime_cfg.apn), CONFIG_GNSS_SAMPLE_MQTT_APN);
#if defined(CONFIG_LTE_NETWORK_MODE_NTN_NBIOT)
	(void)set_string(runtime_cfg.lte_mode, sizeof(runtime_cfg.lte_mode), "ntn_nbiot");
#elif defined(CONFIG_LTE_NETWORK_MODE_NBIOT_GPS)
	(void)set_string(runtime_cfg.lte_mode, sizeof(runtime_cfg.lte_mode), "nbiot_gps");
#elif defined(CONFIG_LTE_NETWORK_MODE_LTE_M_GPS)
	(void)set_string(runtime_cfg.lte_mode, sizeof(runtime_cfg.lte_mode), "lte_m_gps");
#else
	(void)set_string(runtime_cfg.lte_mode, sizeof(runtime_cfg.lte_mode), "lte_m_nbiot_gps");
#endif

	cfg_dirty = false;
	cfg_reboot_required = false;
}

static int cloud_settings_set(const char *name, size_t len_rd,
			      settings_read_cb read_cb, void *cb_arg)
{
	char value[192];
	char key_name[48];
	const char *next;
	int key_len;
	int read_len;
	enum config_key_id key_id;

	key_len = settings_name_next(name, &next);
	if (key_len <= 0 || key_len >= sizeof(key_name)) {
		return -ENOENT;
	}
	if (next && *next != '\0') {
		return -ENOENT;
	}

	memcpy(key_name, name, key_len);
	key_name[key_len] = '\0';

	if (len_rd >= sizeof(value)) {
		return -E2BIG;
	}

	read_len = read_cb(cb_arg, value, len_rd);
	if (read_len < 0) {
		return read_len;
	}
	value[read_len] = '\0';

	key_id = lookup_key_id(key_name, settings_key_map, ARRAY_SIZE(settings_key_map));
	if (key_id == KEY_INVALID) {
		return -ENOENT;
	}

	return apply_key_value(key_id, value, false, false);
}

static struct settings_handler cloud_settings = {
	.name = "cloud",
	.h_set = cloud_settings_set,
};

int cloud_config_init(void)
{
	int err;
	bool removed_legacy_server = false;

	k_mutex_lock(&cfg_lock, K_FOREVER);
	if (cfg_initialized) {
		k_mutex_unlock(&cfg_lock);
		return 0;
	}

	set_defaults();

	err = settings_subsys_init();
	if (err && err != -EALREADY) {
		k_mutex_unlock(&cfg_lock);
		return err;
	}

	err = settings_register(&cloud_settings);
	if (err && err != -EEXIST) {
		k_mutex_unlock(&cfg_lock);
		return err;
	}

	err = settings_load_subtree("cloud");
	if (err) {
		k_mutex_unlock(&cfg_lock);
		return err;
	}

	/* Do not retain the temporary development endpoint from older builds. */
	if (strcmp(runtime_cfg.server_host, "89.168.91.219") == 0) {
		runtime_cfg.server_host[0] = '\0';
		cfg_dirty = true;
		removed_legacy_server = true;
	}

	cfg_initialized = true;
	k_mutex_unlock(&cfg_lock);
	if (removed_legacy_server) {
		(void)cloud_config_save();
		LOG_WRN("Removed legacy telemetry server from persistent config");
	}

	LOG_INF("Runtime config loaded: device=%s protocol=%s server=%s:%u mode=%s cfg=%u interval=%us",
		runtime_cfg.device_id,
		runtime_cfg.protocol,
		runtime_cfg.server_host,
		runtime_cfg.server_port,
		runtime_cfg.data_mode,
		runtime_cfg.config_version,
		runtime_cfg.publish_interval_sec);
	LOG_INF("TN/NTN policy: tn=%u ntn=%u tn_timeout=%us ntn_min=%us ntn_max_payload=%u monthly_ntn_limit=%u",
		runtime_cfg.tn_enabled ? 1U : 0U,
		runtime_cfg.ntn_enabled ? 1U : 0U,
		runtime_cfg.tn_unavailable_timeout_sec,
		runtime_cfg.ntn_min_send_interval_sec,
		runtime_cfg.ntn_max_payload_bytes,
		runtime_cfg.monthly_ntn_byte_limit);
	LOG_INF("Config sync: mqtt=%s interval=%us host=%s:%u",
		runtime_cfg.mqtt_sync_enabled ? "enabled" : "disabled",
		runtime_cfg.mqtt_sync_interval_sec,
		runtime_cfg.mqtt_host,
		runtime_cfg.mqtt_port);

	return 0;
}

const struct cloud_runtime_config *cloud_config_get(void)
{
	if (!cfg_initialized && runtime_cfg.device_id[0] == '\0') {
		set_defaults();
	}

	return &runtime_cfg;
}

const char *cloud_config_device_id_get(void)
{
	if (runtime_cfg.device_id[0] != '\0') {
		return runtime_cfg.device_id;
	}

	return CONFIG_GNSS_SAMPLE_MQTT_DEVICE_ID;
}

int cloud_config_set(const char *key, const char *value)
{
	enum config_key_id key_id;
	int err;

	if (!cfg_initialized) {
		return -EAGAIN;
	}
	if (!key || !value) {
		return -EINVAL;
	}

	key_id = lookup_key_id(key, runtime_key_map, ARRAY_SIZE(runtime_key_map));
	if (key_id == KEY_INVALID) {
		return -ENOENT;
	}

	k_mutex_lock(&cfg_lock, K_FOREVER);
	err = apply_key_value(key_id, value, true, true);
	k_mutex_unlock(&cfg_lock);

	return err;
}

static int save_setting(const char *name, const char *value)
{
	char full_name[64];
	int n;

	n = snprintk(full_name, sizeof(full_name), "cloud/%s", name);
	if (n <= 0 || n >= sizeof(full_name)) {
		return -EINVAL;
	}

	return settings_save_one(full_name, value, strlen(value) + 1U);
}

static int save_u32(const char *name, uint32_t value)
{
	char buf[16];

	snprintk(buf, sizeof(buf), "%u", value);
	return save_setting(name, buf);
}

static int save_i32(const char *name, int32_t value)
{
	char buf[16];

	snprintk(buf, sizeof(buf), "%d", value);
	return save_setting(name, buf);
}

static int save_bool(const char *name, bool value)
{
	return save_setting(name, value ? "1" : "0");
}

int cloud_config_save(void)
{
	int err;

	if (!cfg_initialized) {
		return -EAGAIN;
	}

	k_mutex_lock(&cfg_lock, K_FOREVER);

	err = save_setting("device_id", runtime_cfg.device_id);
	if (!err) {
		err = save_setting("server_host", runtime_cfg.server_host);
	}
	if (!err) {
		err = save_u32("server_port", runtime_cfg.server_port);
	}
	if (!err) {
		err = save_setting("protocol", runtime_cfg.protocol);
	}
	if (!err) {
		err = save_setting("telemetry_protocol", runtime_cfg.telemetry_protocol);
	}
	if (!err) {
		err = save_u32("publish_interval_sec", runtime_cfg.publish_interval_sec);
	}
	if (!err) {
		err = save_u32("gnss_timeout_sec", runtime_cfg.gnss_timeout_sec);
	}
	if (!err) {
		err = save_setting("data_mode", runtime_cfg.data_mode);
	}
	if (!err) {
		err = save_bool("tn_enabled", runtime_cfg.tn_enabled);
	}
	if (!err) {
		err = save_bool("ntn_enabled", runtime_cfg.ntn_enabled);
	}
	if (!err) {
		err = save_u32("tn_unavailable_timeout_sec", runtime_cfg.tn_unavailable_timeout_sec);
	}
	if (!err) {
		err = save_u32("ntn_min_send_interval_sec", runtime_cfg.ntn_min_send_interval_sec);
	}
	if (!err) {
		err = save_u32("ntn_max_payload_bytes", runtime_cfg.ntn_max_payload_bytes);
	}
	if (!err) {
		err = save_u32("monthly_ntn_byte_limit", runtime_cfg.monthly_ntn_byte_limit);
	}
	if (!err) {
		err = save_u32("config_version", runtime_cfg.config_version);
	}
	if (!err) {
		err = save_setting("auth_token", runtime_cfg.auth_token);
	}
	if (!err) {
		err = save_u32("max_payload_bytes", runtime_cfg.max_payload_bytes);
	}
	if (!err) {
		err = save_bool("enable_debug_logs", runtime_cfg.enable_debug_logs);
	}
	if (!err) {
		err = save_setting("mqtt_host", runtime_cfg.mqtt_host);
	}
	if (!err) {
		err = save_u32("mqtt_port", runtime_cfg.mqtt_port);
	}
	if (!err) {
		err = save_setting("mqtt_username", runtime_cfg.mqtt_username);
	}
	if (!err) {
		err = save_setting("mqtt_password", runtime_cfg.mqtt_password);
	}
	if (!err) {
		err = save_i32("mqtt_tls_sec_tag", runtime_cfg.mqtt_tls_sec_tag);
	}
	if (!err) {
		err = save_u32("mqtt_sync_interval_sec", runtime_cfg.mqtt_sync_interval_sec);
	}
	if (!err) {
		err = save_bool("mqtt_sync_enabled", runtime_cfg.mqtt_sync_enabled);
	}
	if (!err) {
		err = save_bool("mqtt_sync_force_in_ntn", runtime_cfg.mqtt_sync_force_in_ntn);
	}
	if (!err) {
		err = save_setting("apn", runtime_cfg.apn);
	}
	if (!err) {
		err = save_setting("lte_mode", runtime_cfg.lte_mode);
	}

	if (!err) {
		cfg_dirty = false;
	}

	k_mutex_unlock(&cfg_lock);
	return err;
}

bool cloud_config_is_dirty(void)
{
	return cfg_dirty;
}

bool cloud_config_reboot_required(void)
{
	return cfg_reboot_required;
}

int cloud_config_get_lte_mode(enum lte_lc_system_mode *mode,
			      enum lte_lc_system_mode_preference *pref)
{
	if (!mode || !pref) {
		return -EINVAL;
	}

	if (strcmp(runtime_cfg.lte_mode, "nbiot_gps") == 0) {
		*mode = LTE_LC_SYSTEM_MODE_NBIOT_GPS;
		*pref = LTE_LC_SYSTEM_MODE_PREFER_NBIOT_PLMN_PRIO;
		return 0;
	}
	if (strcmp(runtime_cfg.lte_mode, "lte_m_gps") == 0) {
		*mode = LTE_LC_SYSTEM_MODE_LTEM_GPS;
		*pref = LTE_LC_SYSTEM_MODE_PREFER_LTEM_PLMN_PRIO;
		return 0;
	}
	if (strcmp(runtime_cfg.lte_mode, "lte_m_nbiot_gps") == 0) {
		*mode = LTE_LC_SYSTEM_MODE_LTEM_NBIOT_GPS;
		*pref = LTE_LC_SYSTEM_MODE_PREFER_AUTO;
		return 0;
	}
	if (strcmp(runtime_cfg.lte_mode, "ntn_nbiot") == 0) {
		*mode = LTE_LC_SYSTEM_MODE_NTN_NBIOT;
		*pref = LTE_LC_SYSTEM_MODE_PREFER_AUTO;
		return 0;
	}

	return -EINVAL;
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

int cloud_config_apply_mqtt_retained(const char *json, size_t len, bool *applied_newer)
{
	int cfg_version;
	char tmp[96];
	char num_buf[24];
	bool changed = false;

	if (applied_newer) {
		*applied_newer = false;
	}
	if (!json || len == 0U) {
		return -EINVAL;
	}

	if (!json_extract_int(json, "configVersion", &cfg_version)) {
		return -EINVAL;
	}

	k_mutex_lock(&cfg_lock, K_FOREVER);
	if ((uint32_t)cfg_version <= runtime_cfg.config_version) {
		k_mutex_unlock(&cfg_lock);
		return 0;
	}

	snprintk(num_buf, sizeof(num_buf), "%d", cfg_version);
	if (apply_key_value(KEY_CONFIG_VERSION, num_buf, true, false) == 0) {
		changed = true;
	}

	if (json_extract_string(json, "protocol", tmp, sizeof(tmp)) && protocol_valid(tmp)) {
		if (apply_key_value(KEY_PROTOCOL, tmp, true, false) == 0) {
			changed = true;
		}
	}
	if (json_extract_string(json, "host", tmp, sizeof(tmp))) {
		if (apply_key_value(KEY_SERVER_HOST, tmp, true, false) == 0) {
			changed = true;
		}
	}
	if (json_extract_int(json, "port", &cfg_version)) {
		snprintk(num_buf, sizeof(num_buf), "%d", cfg_version);
		if (apply_key_value(KEY_SERVER_PORT, num_buf, true, false) == 0) {
			changed = true;
		}
	}
	if (json_extract_int(json, "publishIntervalSec", &cfg_version)) {
		snprintk(num_buf, sizeof(num_buf), "%d", cfg_version);
		if (apply_key_value(KEY_PUBLISH_INTERVAL_SEC, num_buf, true, false) == 0) {
			changed = true;
		}
	}
	if (json_extract_int(json, "gnssTimeoutSec", &cfg_version)) {
		snprintk(num_buf, sizeof(num_buf), "%d", cfg_version);
		if (apply_key_value(KEY_GNSS_TIMEOUT_SEC, num_buf, true, false) == 0) {
			changed = true;
		}
	}
	if (json_extract_string(json, "dataMode", tmp, sizeof(tmp)) && data_mode_valid(tmp)) {
		if (apply_key_value(KEY_DATA_MODE, tmp, true, false) == 0) {
			changed = true;
		}
	}
	if (json_extract_int(json, "tnEnabled", &cfg_version)) {
		if (apply_key_value(KEY_TN_ENABLED, cfg_version ? "1" : "0", true, false) == 0) {
			changed = true;
		}
	}
	if (json_extract_int(json, "ntnEnabled", &cfg_version)) {
		if (apply_key_value(KEY_NTN_ENABLED, cfg_version ? "1" : "0", true, false) == 0) {
			changed = true;
		}
	}
	if (json_extract_int(json, "tnUnavailableTimeoutSec", &cfg_version)) {
		snprintk(num_buf, sizeof(num_buf), "%d", cfg_version);
		if (apply_key_value(KEY_TN_UNAVAILABLE_TIMEOUT_SEC, num_buf, true, false) == 0) {
			changed = true;
		}
	}
	if (json_extract_int(json, "ntnMinSendIntervalSec", &cfg_version)) {
		snprintk(num_buf, sizeof(num_buf), "%d", cfg_version);
		if (apply_key_value(KEY_NTN_MIN_SEND_INTERVAL_SEC, num_buf, true, false) == 0) {
			changed = true;
		}
	}
	if (json_extract_int(json, "ntnMaxPayloadBytes", &cfg_version)) {
		snprintk(num_buf, sizeof(num_buf), "%d", cfg_version);
		if (apply_key_value(KEY_NTN_MAX_PAYLOAD_BYTES, num_buf, true, false) == 0) {
			changed = true;
		}
	}
	if (json_extract_int(json, "monthlyNtnByteLimit", &cfg_version)) {
		snprintk(num_buf, sizeof(num_buf), "%d", cfg_version);
		if (apply_key_value(KEY_MONTHLY_NTN_BYTE_LIMIT, num_buf, true, false) == 0) {
			changed = true;
		}
	}
	if (json_extract_int(json, "maxPayloadBytes", &cfg_version)) {
		snprintk(num_buf, sizeof(num_buf), "%d", cfg_version);
		if (apply_key_value(KEY_MAX_PAYLOAD_BYTES, num_buf, true, false) == 0) {
			changed = true;
		}
	}

	k_mutex_unlock(&cfg_lock);

	if (changed) {
		(void)cloud_config_save();
		cfg_reboot_required = false;
		if (applied_newer) {
			*applied_newer = true;
		}
		LOG_INF("Applied retained config version %u", runtime_cfg.config_version);
	}

	return 0;
}

#if defined(CONFIG_SHELL)
static int cmd_config_show(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "device_id=%s", runtime_cfg.device_id);
	shell_print(sh, "server_host=%s", runtime_cfg.server_host);
	shell_print(sh, "server_port=%u", runtime_cfg.server_port);
	shell_print(sh, "protocol=%s", runtime_cfg.protocol);
	shell_print(sh, "telemetry_protocol=%s", runtime_cfg.telemetry_protocol);
	shell_print(sh, "publish_interval_sec=%u", runtime_cfg.publish_interval_sec);
	shell_print(sh, "gnss_timeout_sec=%u", runtime_cfg.gnss_timeout_sec);
	shell_print(sh, "data_mode=%s", runtime_cfg.data_mode);
	shell_print(sh, "tn_enabled=%u", runtime_cfg.tn_enabled ? 1U : 0U);
	shell_print(sh, "ntn_enabled=%u", runtime_cfg.ntn_enabled ? 1U : 0U);
	shell_print(sh, "tn_unavailable_timeout_sec=%u", runtime_cfg.tn_unavailable_timeout_sec);
	shell_print(sh, "ntn_min_send_interval_sec=%u", runtime_cfg.ntn_min_send_interval_sec);
	shell_print(sh, "ntn_max_payload_bytes=%u", runtime_cfg.ntn_max_payload_bytes);
	shell_print(sh, "monthly_ntn_byte_limit=%u", runtime_cfg.monthly_ntn_byte_limit);
	shell_print(sh, "config_version=%u", runtime_cfg.config_version);
	shell_print(sh, "max_payload_bytes=%u", runtime_cfg.max_payload_bytes);
	shell_print(sh, "enable_debug_logs=%u", runtime_cfg.enable_debug_logs ? 1U : 0U);
	shell_print(sh, "auth_token=%s", runtime_cfg.auth_token[0] ? "<set>" : "<none>");
	shell_print(sh, "mqtt_host=%s", runtime_cfg.mqtt_host);
	shell_print(sh, "mqtt_port=%u", runtime_cfg.mqtt_port);
	shell_print(sh, "mqtt_sync_enabled=%u", runtime_cfg.mqtt_sync_enabled ? 1U : 0U);
	shell_print(sh, "mqtt_sync_interval_sec=%u", runtime_cfg.mqtt_sync_interval_sec);
	shell_print(sh, "apn=%s", runtime_cfg.apn);
	shell_print(sh, "lte_mode=%s", runtime_cfg.lte_mode);
	shell_print(sh, "dirty=%u reboot_required=%u", cfg_dirty ? 1U : 0U,
		    cfg_reboot_required ? 1U : 0U);
	return 0;
}

static int cmd_config_set(const struct shell *sh, size_t argc, char **argv)
{
	int err;

	if (argc != 3U) {
		shell_error(sh, "Usage: config set <key> <value>");
		return -EINVAL;
	}

	err = cloud_config_set(argv[1], argv[2]);
	if (err) {
		shell_error(sh, "set failed (%d)", err);
		return err;
	}

	shell_print(sh, "Updated %s. Run 'config save' then 'config reboot'.", argv[1]);
	return 0;
}

static int cmd_config_save(const struct shell *sh, size_t argc, char **argv)
{
	int err;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	err = cloud_config_save();
	if (err) {
		shell_error(sh, "save failed (%d)", err);
		return err;
	}

	shell_print(sh, "Config saved.");
	return 0;
}

static int cmd_config_reboot(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "Rebooting...");
	sys_reboot(SYS_REBOOT_COLD);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_config,
	SHELL_CMD(show, NULL, "Show runtime config", cmd_config_show),
	SHELL_CMD_ARG(set, NULL, "Set runtime config key", cmd_config_set, 3, 0),
	SHELL_CMD(save, NULL, "Save runtime config", cmd_config_save),
	SHELL_CMD(reboot, NULL, "Reboot device", cmd_config_reboot),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(config, &sub_config, "Runtime cloud config", NULL);
#endif
