/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef GNSS_UART_BRIDGE_H_
#define GNSS_UART_BRIDGE_H_

#include <stdbool.h>
#include <stdint.h>
#include <nrf_modem_gnss.h>

#ifdef __cplusplus
extern "C" {
#endif

struct gnss_uart_bridge_metrics {
	uint32_t fixes_accepted;
	uint32_t fixes_rejected;
	uint32_t nmea_dropped;
};

int gnss_uart_bridge_init(void);

void gnss_uart_bridge_set_last_nmea(const struct nrf_modem_gnss_nmea_data_frame *nmea);

void gnss_uart_bridge_publish_pvt(const struct nrf_modem_gnss_pvt_data_frame *pvt,
					  uint8_t tracked,
					  uint8_t used,
					  uint8_t unhealthy,
					  const struct gnss_uart_bridge_metrics *metrics);

void gnss_uart_bridge_publish_status(const char *code, const char *message, const char *details);

void gnss_uart_bridge_send_text(const char *text);

bool gnss_uart_bridge_stream_enabled(void);
uint8_t gnss_uart_bridge_stream_rate_hz(void);
uint32_t gnss_uart_bridge_rx_command_count(void);
uint32_t gnss_uart_bridge_rx_overflow_count(void);

#ifdef __cplusplus
}
#endif

#endif /* GNSS_UART_BRIDGE_H_ */
