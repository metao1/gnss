/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef LOCATION_SERVICE_H_
#define LOCATION_SERVICE_H_

#include <stdbool.h>
#include <stdint.h>
#include <nrf_modem_gnss.h>

#ifdef __cplusplus
extern "C" {
#endif

enum LocationSource {
	LOCATION_SOURCE_GNSS,
	LOCATION_SOURCE_CELLULAR,
	LOCATION_SOURCE_LAST_KNOWN,
	LOCATION_SOURCE_NONE
};

struct Location {
	double lat;
	double lon;
	double accuracy_m;
	bool valid;
	bool fix_valid;
	enum LocationSource source;
	int64_t timestamp;
	char timestamp_iso8601[25];
	bool timestamp_valid;
};

struct location_service_status {
	bool cellular_request_in_flight;
	uint32_t cellular_requests_started;
	uint32_t cellular_requests_success;
	uint32_t cellular_requests_failed;
};

struct cellular_identity {
	bool valid;
	char operator_name[32];
	char plmn[7];
	int mcc;
	int mnc;
	char tac_hex[9];
	char cell_id_hex[9];
	uint32_t tac;
	uint32_t cell_id;
	int rat;
	int rsrp;
	int64_t ts_ms;
};

int location_service_init(void);
void location_service_handle_gnss_update(const struct nrf_modem_gnss_pvt_data_frame *pvt,
					 bool accepted_fix);
bool location_service_get_location(struct Location *location);
bool location_service_get_cell_identity(struct cellular_identity *identity);
const char *location_service_source_to_str(enum LocationSource source);
void location_service_get_status(struct location_service_status *status);

#ifdef __cplusplus
}
#endif

#endif /* LOCATION_SERVICE_H_ */
