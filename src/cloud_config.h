/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef CLOUD_CONFIG_H_
#define CLOUD_CONFIG_H_

#include <stdbool.h>
#include <stdint.h>
#include <modem/lte_lc.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CLOUD_CONFIG_DEVICE_ID_MAX_LEN 64
#define CLOUD_CONFIG_HOST_MAX_LEN 96
#define CLOUD_CONFIG_PROTOCOL_MAX_LEN 8
#define CLOUD_CONFIG_TOPIC_MAX_LEN 128
#define CLOUD_CONFIG_USERNAME_MAX_LEN 64
#define CLOUD_CONFIG_PASSWORD_MAX_LEN 128
#define CLOUD_CONFIG_TOKEN_MAX_LEN 128
#define CLOUD_CONFIG_APN_MAX_LEN 64
#define CLOUD_CONFIG_LTE_MODE_MAX_LEN 32
#define CLOUD_CONFIG_DATA_MODE_MAX_LEN 16

struct cloud_runtime_config {
	char device_id[CLOUD_CONFIG_DEVICE_ID_MAX_LEN];
	char server_host[CLOUD_CONFIG_HOST_MAX_LEN];
	uint16_t server_port;
	char protocol[CLOUD_CONFIG_PROTOCOL_MAX_LEN];
	char telemetry_protocol[CLOUD_CONFIG_PROTOCOL_MAX_LEN];
	uint32_t publish_interval_sec;
	uint32_t gnss_timeout_sec;
	char data_mode[CLOUD_CONFIG_DATA_MODE_MAX_LEN];
	bool tn_enabled;
	bool ntn_enabled;
	uint32_t tn_unavailable_timeout_sec;
	uint32_t ntn_min_send_interval_sec;
	uint16_t ntn_max_payload_bytes;
	uint32_t monthly_ntn_byte_limit;
	uint32_t config_version;
	char auth_token[CLOUD_CONFIG_TOKEN_MAX_LEN];
	uint16_t max_payload_bytes;
	bool enable_debug_logs;

	char mqtt_host[CLOUD_CONFIG_HOST_MAX_LEN];
	uint16_t mqtt_port;
	char mqtt_username[CLOUD_CONFIG_USERNAME_MAX_LEN];
	char mqtt_password[CLOUD_CONFIG_PASSWORD_MAX_LEN];
	int32_t mqtt_tls_sec_tag;
	uint32_t mqtt_sync_interval_sec;
	bool mqtt_sync_enabled;
	bool mqtt_sync_force_in_ntn;

	char apn[CLOUD_CONFIG_APN_MAX_LEN];
	char lte_mode[CLOUD_CONFIG_LTE_MODE_MAX_LEN];
};

int cloud_config_init(void);
const struct cloud_runtime_config *cloud_config_get(void);
const char *cloud_config_device_id_get(void);

int cloud_config_set(const char *key, const char *value);
int cloud_config_save(void);
bool cloud_config_is_dirty(void);
bool cloud_config_reboot_required(void);

int cloud_config_get_lte_mode(enum lte_lc_system_mode *mode,
			      enum lte_lc_system_mode_preference *pref);

int cloud_config_apply_mqtt_retained(const char *json, size_t len, bool *applied_newer);

#ifdef __cplusplus
}
#endif

#endif /* CLOUD_CONFIG_H_ */
