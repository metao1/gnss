/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef GNSS_BLE_H_
#define GNSS_BLE_H_

#include <stdint.h>
#include <stdbool.h>
#include <nrf_modem_gnss.h>

#ifdef __cplusplus
extern "C" {
#endif

struct gnss_ble_metrics {
	uint32_t fixes_accepted;
	uint32_t fixes_rejected;
	uint32_t nmea_dropped;
};

int gnss_ble_init(void);

void gnss_ble_set_last_nmea(const struct nrf_modem_gnss_nmea_data_frame *nmea);

void gnss_ble_notify_pvt(const struct nrf_modem_gnss_pvt_data_frame *pvt,
			 uint8_t tracked,
			 uint8_t used,
			 uint8_t unhealthy,
			 const struct gnss_ble_metrics *metrics);

void gnss_ble_notify_status(const char *code, const char *message, const char *details);

bool gnss_ble_stream_enabled(void);
uint8_t gnss_ble_stream_rate_hz(void);
uint32_t gnss_ble_dropped_count(void);

#ifdef __cplusplus
}
#endif

#endif /* GNSS_BLE_H_ */
