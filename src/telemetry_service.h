/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef TELEMETRY_SERVICE_H_
#define TELEMETRY_SERVICE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <nrf_modem_gnss.h>

#include "location_service.h"

#ifdef __cplusplus
extern "C" {
#endif

struct telemetry_sample {
	enum LocationSource location_source;
	bool fix_valid;
	double latitude;
	double longitude;
	float accuracy;
	float altitude;
	float altitude_accuracy;
	float speed;
	float speed_accuracy;
	float vertical_speed;
	float vertical_speed_accuracy;
	float heading;
	float heading_accuracy;
	float pdop;
	float hdop;
	float vdop;
	float tdop;
	uint64_t ts_ms;
	char timestamp[25];
	bool timestamp_valid;
};

bool telemetry_sample_from_pvt(const struct nrf_modem_gnss_pvt_data_frame *pvt,
			       struct telemetry_sample *sample);
bool telemetry_sample_from_location(const struct Location *location,
				    const struct nrf_modem_gnss_pvt_data_frame *pvt,
				    struct telemetry_sample *sample);

int telemetry_sample_to_json(const struct telemetry_sample *sample, char *buf, size_t len);
int telemetry_cell_identity_to_json(const struct cellular_identity *identity,
				    char *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* TELEMETRY_SERVICE_H_ */
