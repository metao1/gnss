/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "location_service.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <nrf_modem_at.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

LOG_MODULE_DECLARE(gnss_sample, CONFIG_GNSS_SAMPLE_LOG_LEVEL);

#define LOCATION_CELL_RAW_BUF_LEN 512
#define LOCATION_XMONITOR_BUF_LEN 256
#define LOCATION_WORKQ_PRIORITY 8
#define LOCATION_WORKQ_STARTUP_DELAY_MS 20

static struct k_mutex state_lock;
static struct Location latest_gnss;
static struct Location last_known;
static struct cellular_identity latest_cell;
static atomic_t location_initialized;
static atomic_t cellular_request_in_flight;
static atomic_t cellular_requests_started;
static atomic_t cellular_requests_success;
static atomic_t cellular_requests_failed;
static int64_t last_cellular_request_ms;
static struct k_work_q location_work_q;
static struct k_work_delayable cellular_request_work;
K_THREAD_STACK_DEFINE(location_workq_stack,
		      CONFIG_GNSS_SAMPLE_CELLULAR_LOCATION_WORKQ_STACK_SIZE);

struct cellular_context {
	char ncellmeas[LOCATION_CELL_RAW_BUF_LEN];
	char xmonitor[LOCATION_XMONITOR_BUF_LEN];
};

static bool coordinates_valid(double lat, double lon)
{
	return isfinite(lat) && isfinite(lon) &&
	       lat >= -90.0 && lat <= 90.0 &&
	       lon >= -180.0 && lon <= 180.0;
}

static void trim_ascii(char *str)
{
	size_t len;

	if (!str || str[0] == 0) {
		return;
	}

	len = strlen(str);
	while (len > 0U && isspace((unsigned char)str[len - 1U])) {
		str[len - 1U] = 0;
		len--;
	}

	while (*str && isspace((unsigned char)*str)) {
		memmove(str, str + 1, strlen(str));
	}
}

static bool is_all_digits(const char *s, size_t len)
{
	if (!s || len == 0U) {
		return false;
	}

	for (size_t i = 0U; i < len; i++) {
		if (!isdigit((unsigned char)s[i])) {
			return false;
		}
	}

	return true;
}

static bool parse_hex_u32(const char *hex, uint32_t *out)
{
	char *end = NULL;
	unsigned long v;

	if (!hex || !out || hex[0] == 0) {
		return false;
	}

	v = strtoul(hex, &end, 16);
	if (!end || *end != 0 || v > UINT32_MAX) {
		return false;
	}

	*out = (uint32_t)v;
	return true;
}

static bool parse_plmn(const char *plmn, int *mcc, int *mnc)
{
	char mcc_s[4] = {0};
	char mnc_s[4] = {0};
	size_t len;

	if (!plmn || !mcc || !mnc) {
		return false;
	}

	len = strlen(plmn);
	if (len != 5U && len != 6U) {
		return false;
	}
	if (!is_all_digits(plmn, len)) {
		return false;
	}

	memcpy(mcc_s, plmn, 3U);
	memcpy(mnc_s, plmn + 3U, len - 3U);
	*mcc = (int)strtol(mcc_s, NULL, 10);
	*mnc = (int)strtol(mnc_s, NULL, 10);
	return true;
}

static bool get_quoted_token(const char *str, int index, char *out, size_t out_len)
{
	const char *p = str;
	int token_idx = 0;

	if (!str || !out || out_len < 2U || index < 0) {
		return false;
	}

	while ((p = strchr(p, '\"')) != NULL) {
		const char *start = ++p;
		const char *end = strchr(start, '\"');
		size_t len;

		if (!end) {
			break;
		}

		if (token_idx == index) {
			len = MIN((size_t)(end - start), out_len - 1U);
			memcpy(out, start, len);
			out[len] = 0;
			return true;
		}

		token_idx++;
		p = end + 1;
	}

	return false;
}

static void location_timestamp_set_from_gnss(struct Location *location,
					     const struct nrf_modem_gnss_datetime *datetime)
{
	if (!location || !datetime) {
		return;
	}

	if (datetime->year < 2024 ||
	    datetime->month < 1 || datetime->month > 12 ||
	    datetime->day < 1 || datetime->day > 31 ||
	    datetime->hour > 23 ||
	    datetime->minute > 59 ||
	    datetime->seconds > 60) {
		return;
	}

	snprintk(location->timestamp_iso8601, sizeof(location->timestamp_iso8601),
		 "%04u-%02u-%02uT%02u:%02u:%02uZ",
		 datetime->year, datetime->month, datetime->day,
		 datetime->hour, datetime->minute, datetime->seconds);
	location->timestamp_valid = true;
}

static int collect_cellular_context(struct cellular_context *ctx)
{
	int err;

	if (!ctx) {
		return -EINVAL;
	}

	memset(ctx, 0, sizeof(*ctx));

	err = nrf_modem_at_cmd(ctx->ncellmeas, sizeof(ctx->ncellmeas), "AT%%NCELLMEAS=1");
	if (err) {
		LOG_WRN("%%NCELLMEAS command failed: %d", err);
		return err;
	}
	trim_ascii(ctx->ncellmeas);

	err = nrf_modem_at_cmd(ctx->xmonitor, sizeof(ctx->xmonitor), "AT%%XMONITOR");
	if (err) {
		LOG_WRN("%%XMONITOR command failed: %d", err);
		ctx->xmonitor[0] = 0;
	} else {
		trim_ascii(ctx->xmonitor);
	}

	return 0;
}

static bool parse_cell_identity(const struct cellular_context *ctx, struct cellular_identity *id)
{
	char tok0[64] = {0};
	char tok1[64] = {0};
	char tok2[64] = {0};
	char tok3[64] = {0};
	char tok4[64] = {0};
	const char *after_tac = NULL;

	if (!ctx || !id) {
		return false;
	}

	memset(id, 0, sizeof(*id));
	id->rat = -1;
	id->rsrp = -1;
	id->ts_ms = k_uptime_get();

	if (ctx->ncellmeas[0] != 0) {
		if (get_quoted_token(ctx->ncellmeas, 0, tok0, sizeof(tok0))) {
			strncpy(id->cell_id_hex, tok0, sizeof(id->cell_id_hex) - 1U);
			(void)parse_hex_u32(id->cell_id_hex, &id->cell_id);
		}
		if (get_quoted_token(ctx->ncellmeas, 1, tok1, sizeof(tok1))) {
			strncpy(id->plmn, tok1, sizeof(id->plmn) - 1U);
			(void)parse_plmn(id->plmn, &id->mcc, &id->mnc);
		}
		if (get_quoted_token(ctx->ncellmeas, 2, tok2, sizeof(tok2))) {
			strncpy(id->tac_hex, tok2, sizeof(id->tac_hex) - 1U);
			(void)parse_hex_u32(id->tac_hex, &id->tac);
		}

		/* Try to extract an RSRP-like value from the numeric section if present. */
		{
			int a, b, c, d;
			if (sscanf(ctx->ncellmeas,
				   "%%NCELLMEAS: %*d,\"%*[^\"]\",\"%*[^\"]\",\"%*[^\"]\",%d,%d,%d,%d",
				   &a, &b, &c, &d) == 4) {
				id->rsrp = c;
			}
		}
	}

	if (ctx->xmonitor[0] != 0) {
		if (get_quoted_token(ctx->xmonitor, 0, tok0, sizeof(tok0))) {
			strncpy(id->operator_name, tok0, sizeof(id->operator_name) - 1U);
		}
		if (get_quoted_token(ctx->xmonitor, 2, tok2, sizeof(tok2)) && id->plmn[0] == 0) {
			strncpy(id->plmn, tok2, sizeof(id->plmn) - 1U);
			(void)parse_plmn(id->plmn, &id->mcc, &id->mnc);
		}
		if (get_quoted_token(ctx->xmonitor, 3, tok3, sizeof(tok3)) && id->tac_hex[0] == 0) {
			strncpy(id->tac_hex, tok3, sizeof(id->tac_hex) - 1U);
			(void)parse_hex_u32(id->tac_hex, &id->tac);
		}
		if (get_quoted_token(ctx->xmonitor, 4, tok4, sizeof(tok4)) && id->cell_id_hex[0] == 0 &&
		    strchr(tok4, '.') == NULL && strchr(tok4, '-') == NULL) {
			/* Token may be cell ID or other numeric field depending on modem FW. */
			if (strlen(tok4) >= 4U && strlen(tok4) <= 8U) {
				strncpy(id->cell_id_hex, tok4, sizeof(id->cell_id_hex) - 1U);
				(void)parse_hex_u32(id->cell_id_hex, &id->cell_id);
			}
		}

		if (id->tac_hex[0] != 0) {
			after_tac = strstr(ctx->xmonitor, id->tac_hex);
			if (after_tac) {
				after_tac += strlen(id->tac_hex);
				if (sscanf(after_tac, "\",%d", &id->rat) != 1) {
					id->rat = -1;
				}
			}
		}
	}

	if (id->plmn[0] != 0) {
		(void)parse_plmn(id->plmn, &id->mcc, &id->mnc);
	}

	id->valid = (id->mcc > 0 && id->mnc >= 0 && id->tac > 0U && id->cell_id > 0U);
	return id->valid;
}

static void cellular_request_work_handler(struct k_work *work)
{
	struct cellular_context ctx;
	struct cellular_identity identity;
	int err;

	ARG_UNUSED(work);

	if (!atomic_get(&location_initialized)) {
		return;
	}

	atomic_set(&cellular_request_in_flight, 1);
	atomic_inc(&cellular_requests_started);
	LOG_INF("GNSS no fix, reading cellular identity for backend fallback");

	err = collect_cellular_context(&ctx);
	if (err) {
		atomic_inc(&cellular_requests_failed);
		atomic_set(&cellular_request_in_flight, 0);
		LOG_WRN("Cellular measurement collection failed: %d", err);
		return;
	}

	if (!parse_cell_identity(&ctx, &identity)) {
		atomic_inc(&cellular_requests_failed);
		atomic_set(&cellular_request_in_flight, 0);
		LOG_WRN("Failed to parse cellular identity from modem output");
		return;
	}

	k_mutex_lock(&state_lock, K_FOREVER);
	latest_cell = identity;
	k_mutex_unlock(&state_lock);

	atomic_inc(&cellular_requests_success);
	atomic_set(&cellular_request_in_flight, 0);
	LOG_INF("Cellular identity ready: plmn=%s tac=%u cell=%u rat=%d rsrp=%d",
		identity.plmn, identity.tac, identity.cell_id, identity.rat, identity.rsrp);
}

static int request_cellular_identity_async(void)
{
	const int64_t now = k_uptime_get();
	const int64_t req_interval_ms =
		(int64_t)CONFIG_GNSS_SAMPLE_CELLULAR_LOCATION_REQUEST_INTERVAL_SECONDS * 1000LL;

	if (!atomic_get(&location_initialized)) {
		return -EPERM;
	}

	if (atomic_get(&cellular_request_in_flight)) {
		return -EBUSY;
	}

	if ((now - last_cellular_request_ms) < req_interval_ms) {
		return -EAGAIN;
	}

	last_cellular_request_ms = now;
	return k_work_schedule_for_queue(&location_work_q, &cellular_request_work, K_NO_WAIT);
}

int location_service_init(void)
{
	if (atomic_get(&location_initialized)) {
		return 0;
	}

	k_mutex_init(&state_lock);
	memset(&latest_gnss, 0, sizeof(latest_gnss));
	memset(&last_known, 0, sizeof(last_known));
	memset(&latest_cell, 0, sizeof(latest_cell));
	latest_cell.rat = -1;
	latest_cell.rsrp = -1;

	k_work_queue_init(&location_work_q);
	k_work_queue_start(&location_work_q,
			   location_workq_stack,
			   K_THREAD_STACK_SIZEOF(location_workq_stack),
			   LOCATION_WORKQ_PRIORITY,
			   NULL);
	k_thread_name_set(&location_work_q.thread, "loc_fallback");
	k_work_init_delayable(&cellular_request_work, cellular_request_work_handler);
	k_sleep(K_MSEC(LOCATION_WORKQ_STARTUP_DELAY_MS));

	atomic_set(&location_initialized, 1);
	LOG_INF("LocationService initialized (GNSS primary, cellular identity fallback for backend)");
	return 0;
}

void location_service_handle_gnss_update(const struct nrf_modem_gnss_pvt_data_frame *pvt,
					 bool accepted_fix)
{
	struct Location loc;

	if (!pvt || !atomic_get(&location_initialized)) {
		return;
	}

	if (accepted_fix && coordinates_valid(pvt->latitude, pvt->longitude)) {
		loc = (struct Location){
			.lat = pvt->latitude,
			.lon = pvt->longitude,
			.accuracy_m = pvt->accuracy,
			.valid = true,
			.fix_valid = true,
			.source = LOCATION_SOURCE_GNSS,
			.timestamp = k_uptime_get(),
		};
		location_timestamp_set_from_gnss(&loc, &pvt->datetime);

		k_mutex_lock(&state_lock, K_FOREVER);
		latest_gnss = loc;
		last_known = loc;
		k_mutex_unlock(&state_lock);
		LOG_DBG("GNSS fix valid for LocationService");
		return;
	}

	k_mutex_lock(&state_lock, K_FOREVER);
	latest_gnss.valid = false;
	latest_gnss.fix_valid = false;
	k_mutex_unlock(&state_lock);

	LOG_DBG("GNSS no fix for LocationService");
	(void)request_cellular_identity_async();
}

bool location_service_get_location(struct Location *location)
{
	if (!location || !atomic_get(&location_initialized)) {
		return false;
	}

	k_mutex_lock(&state_lock, K_FOREVER);
	if (latest_gnss.valid && latest_gnss.fix_valid) {
		*location = latest_gnss;
		k_mutex_unlock(&state_lock);
		return true;
	}

	if (last_known.valid) {
		*location = last_known;
		location->source = LOCATION_SOURCE_LAST_KNOWN;
		location->fix_valid = false;
		k_mutex_unlock(&state_lock);
		return true;
	}
	k_mutex_unlock(&state_lock);

	*location = (struct Location){
		.valid = false,
		.source = LOCATION_SOURCE_NONE,
		.timestamp = k_uptime_get(),
	};
	return false;
}

bool location_service_get_cell_identity(struct cellular_identity *identity)
{
	if (!identity || !atomic_get(&location_initialized)) {
		return false;
	}

	k_mutex_lock(&state_lock, K_FOREVER);
	*identity = latest_cell;
	k_mutex_unlock(&state_lock);

	return identity->valid;
}

const char *location_service_source_to_str(enum LocationSource source)
{
	switch (source) {
	case LOCATION_SOURCE_GNSS:
		return "gnss";
	case LOCATION_SOURCE_CELLULAR:
		return "cellular";
	case LOCATION_SOURCE_LAST_KNOWN:
		return "last_known";
	case LOCATION_SOURCE_NONE:
	default:
		return "none";
	}
}

void location_service_get_status(struct location_service_status *status)
{
	if (!status) {
		return;
	}

	*status = (struct location_service_status){
		.cellular_request_in_flight = atomic_get(&cellular_request_in_flight) != 0,
		.cellular_requests_started = (uint32_t)atomic_get(&cellular_requests_started),
		.cellular_requests_success = (uint32_t)atomic_get(&cellular_requests_success),
		.cellular_requests_failed = (uint32_t)atomic_get(&cellular_requests_failed),
	};
}
