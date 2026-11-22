/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef MQTT_NBIOT_TRANSPORT_H_
#define MQTT_NBIOT_TRANSPORT_H_

#include <stdbool.h>
#include <stdint.h>
#include <nrf_modem_gnss.h>

#ifdef __cplusplus
extern "C" {
#endif

int mqtt_nbiot_transport_init(void);
void mqtt_nbiot_transport_handle_pvt(const struct nrf_modem_gnss_pvt_data_frame *pvt,
				     bool accepted_fix);

struct mqtt_nbiot_status {
	bool lte_registered;
	bool mqtt_sync_connected;
	bool last_fix_valid;
	uint32_t config_sync_ok;
	uint32_t config_sync_fail;
	uint32_t telemetry_sent_ok;
	uint32_t telemetry_sent_fail;
	uint32_t backend_rsp_ok;
	uint32_t backend_rsp_fail;
	uint32_t queued_samples;
	uint32_t dropped_samples;
	uint32_t skipped_lte_not_ready;
	uint32_t skipped_interval;
	uint32_t skipped_no_fix;
	uint32_t ntn_sent_ok;
	uint32_t ntn_sent_fail;
	uint32_t ntn_skipped_interval;
	uint32_t ntn_skipped_budget;
	uint32_t ntn_bytes_this_month;
	uint32_t tn_unavailable_for_sec;
	uint32_t seq;
	int32_t state;
	int32_t last_error;
	int32_t last_lte_reg_status;
	uint32_t config_version;
	uint16_t server_port;
	char server_host[40];
	char protocol[8];
	char data_mode[16];
	char active_path[8];
};

void mqtt_nbiot_transport_get_status(struct mqtt_nbiot_status *status);
const char *mqtt_nbiot_state_to_str(int32_t state);
const char *mqtt_nbiot_active_broker_host(void);
const char *mqtt_nbiot_reg_status_to_str(int32_t reg_status);
const char *mqtt_nbiot_err_to_str(int32_t err);

#ifdef __cplusplus
}
#endif

#endif /* MQTT_NBIOT_TRANSPORT_H_ */
