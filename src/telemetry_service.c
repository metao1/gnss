/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "telemetry_service.h"
#include "cloud_config.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>

static bool coordinates_valid(double lat, double lon)
{
	return isfinite(lat) && isfinite(lon) &&
	       lat >= -90.0 && lat <= 90.0 &&
	       lon >= -180.0 && lon <= 180.0;
}

static bool pvt_coordinates_valid(const struct nrf_modem_gnss_pvt_data_frame *pvt)
{
	return pvt &&
	       (pvt->flags & NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID) &&
	       coordinates_valid(pvt->latitude, pvt->longitude);
}

static bool pvt_datetime_valid(const struct nrf_modem_gnss_datetime *datetime)
{
	return datetime->year >= 2024 &&
	       datetime->month >= 1 && datetime->month <= 12 &&
	       datetime->day >= 1 && datetime->day <= 31 &&
	       datetime->hour <= 23 &&
	       datetime->minute <= 59 &&
	       datetime->seconds <= 60;
}

static const char *telemetry_source_to_str(enum LocationSource source)
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

static void set_iso_timestamp_from_pvt(struct telemetry_sample *sample,
				       const struct nrf_modem_gnss_datetime *datetime)
{
	if (!sample || !datetime || !pvt_datetime_valid(datetime)) {
		return;
	}

	snprintk(sample->timestamp, sizeof(sample->timestamp),
		 "%04u-%02u-%02uT%02u:%02u:%02uZ",
		 datetime->year,
		 datetime->month,
		 datetime->day,
		 datetime->hour,
		 datetime->minute,
		 datetime->seconds);
	sample->timestamp_valid = true;
}

bool telemetry_sample_from_pvt(const struct nrf_modem_gnss_pvt_data_frame *pvt,
			       struct telemetry_sample *sample)
{
	if (!sample || !pvt_coordinates_valid(pvt)) {
		return false;
	}

	*sample = (struct telemetry_sample){
		.location_source = LOCATION_SOURCE_GNSS,
		.fix_valid = true,
		.latitude = pvt->latitude,
		.longitude = pvt->longitude,
		.accuracy = pvt->accuracy,
		.altitude = pvt->altitude,
		.altitude_accuracy = pvt->altitude_accuracy,
		.speed = pvt->speed,
		.speed_accuracy = pvt->speed_accuracy,
		.vertical_speed = pvt->vertical_speed,
		.vertical_speed_accuracy = pvt->vertical_speed_accuracy,
		.heading = pvt->heading,
		.heading_accuracy = pvt->heading_accuracy,
		.pdop = pvt->pdop,
		.hdop = pvt->hdop,
		.vdop = pvt->vdop,
		.tdop = pvt->tdop,
		.ts_ms = k_uptime_get(),
	};

	set_iso_timestamp_from_pvt(sample, &pvt->datetime);
	return true;
}

bool telemetry_sample_from_location(const struct Location *location,
				    const struct nrf_modem_gnss_pvt_data_frame *pvt,
				    struct telemetry_sample *sample)
{
	if (!sample || !location || !location->valid || !coordinates_valid(location->lat, location->lon)) {
		return false;
	}

	*sample = (struct telemetry_sample){
		.location_source = location->source,
		.fix_valid = location->fix_valid,
		.latitude = location->lat,
		.longitude = location->lon,
		.accuracy = (float)location->accuracy_m,
		.altitude = NAN,
		.altitude_accuracy = NAN,
		.speed = NAN,
		.speed_accuracy = NAN,
		.vertical_speed = NAN,
		.vertical_speed_accuracy = NAN,
		.heading = NAN,
		.heading_accuracy = NAN,
		.pdop = NAN,
		.hdop = NAN,
		.vdop = NAN,
		.tdop = NAN,
		.ts_ms = (uint64_t)location->timestamp,
		.timestamp_valid = location->timestamp_valid,
	};

	if (location->timestamp_valid) {
		strncpy(sample->timestamp, location->timestamp_iso8601, sizeof(sample->timestamp) - 1U);
		sample->timestamp[sizeof(sample->timestamp) - 1U] = '\0';
	}

	if (location->source == LOCATION_SOURCE_GNSS && pvt) {
		sample->altitude = pvt->altitude;
		sample->altitude_accuracy = pvt->altitude_accuracy;
		sample->speed = pvt->speed;
		sample->speed_accuracy = pvt->speed_accuracy;
		sample->vertical_speed = pvt->vertical_speed;
		sample->vertical_speed_accuracy = pvt->vertical_speed_accuracy;
		sample->heading = pvt->heading;
		sample->heading_accuracy = pvt->heading_accuracy;
		sample->pdop = pvt->pdop;
		sample->hdop = pvt->hdop;
		sample->vdop = pvt->vdop;
		sample->tdop = pvt->tdop;
		set_iso_timestamp_from_pvt(sample, &pvt->datetime);
	}

	return true;
}

int telemetry_sample_to_json(const struct telemetry_sample *sample, char *buf, size_t len)
{
	const char *source;

	if (!sample || !buf || len == 0) {
		return -EINVAL;
	}

	source = telemetry_source_to_str(sample->location_source);

#if (CONFIG_GNSS_SAMPLE_MQTT_BATTERY_PERCENT >= 0)
	if (sample->location_source == LOCATION_SOURCE_GNSS && sample->fix_valid) {
		if (sample->timestamp_valid) {
			return snprintk(buf, len,
				"{\"deviceId\":\"%s\",\"locationSource\":\"%s\",\"fixValid\":true,"
				"\"lat\":%.06f,\"lon\":%.06f,\"accuracy\":%.01f,"
				"\"alt_m\":%.01f,\"alt_acc_m\":%.01f,"
				"\"speed_mps\":%.01f,\"speed_acc_mps\":%.01f,"
				"\"v_speed_mps\":%.01f,\"v_speed_acc_mps\":%.01f,"
				"\"heading_deg\":%.01f,\"heading_acc_deg\":%.01f,"
				"\"pdop\":%.01f,\"hdop\":%.01f,\"vdop\":%.01f,\"tdop\":%.01f,"
				"\"battery\":%d,\"timestamp\":\"%s\"}",
				cloud_config_device_id_get(), source,
				sample->latitude, sample->longitude, (double)sample->accuracy,
				(double)sample->altitude, (double)sample->altitude_accuracy,
				(double)sample->speed, (double)sample->speed_accuracy,
				(double)sample->vertical_speed, (double)sample->vertical_speed_accuracy,
				(double)sample->heading, (double)sample->heading_accuracy,
				(double)sample->pdop, (double)sample->hdop,
				(double)sample->vdop, (double)sample->tdop,
				CONFIG_GNSS_SAMPLE_MQTT_BATTERY_PERCENT, sample->timestamp);
		}

		return snprintk(buf, len,
			"{\"deviceId\":\"%s\",\"locationSource\":\"%s\",\"fixValid\":true,"
			"\"lat\":%.06f,\"lon\":%.06f,\"accuracy\":%.01f,"
			"\"alt_m\":%.01f,\"alt_acc_m\":%.01f,"
			"\"speed_mps\":%.01f,\"speed_acc_mps\":%.01f,"
			"\"v_speed_mps\":%.01f,\"v_speed_acc_mps\":%.01f,"
			"\"heading_deg\":%.01f,\"heading_acc_deg\":%.01f,"
			"\"pdop\":%.01f,\"hdop\":%.01f,\"vdop\":%.01f,\"tdop\":%.01f,"
			"\"battery\":%d,\"ts_ms\":%lld}",
			cloud_config_device_id_get(), source,
			sample->latitude, sample->longitude, (double)sample->accuracy,
			(double)sample->altitude, (double)sample->altitude_accuracy,
			(double)sample->speed, (double)sample->speed_accuracy,
			(double)sample->vertical_speed, (double)sample->vertical_speed_accuracy,
			(double)sample->heading, (double)sample->heading_accuracy,
			(double)sample->pdop, (double)sample->hdop,
			(double)sample->vdop, (double)sample->tdop,
			CONFIG_GNSS_SAMPLE_MQTT_BATTERY_PERCENT, sample->ts_ms);
	}

	if (sample->timestamp_valid) {
		return snprintk(buf, len,
			"{\"deviceId\":\"%s\",\"locationSource\":\"%s\",\"fixValid\":%s,"
			"\"lat\":%.06f,\"lon\":%.06f,\"accuracy\":%.01f,\"battery\":%d,\"timestamp\":\"%s\"}",
			cloud_config_device_id_get(),
			source,
			sample->fix_valid ? "true" : "false",
			sample->latitude,
			sample->longitude,
			(double)sample->accuracy,
			CONFIG_GNSS_SAMPLE_MQTT_BATTERY_PERCENT,
			sample->timestamp);
	}

	return snprintk(buf, len,
		"{\"deviceId\":\"%s\",\"locationSource\":\"%s\",\"fixValid\":%s,"
		"\"lat\":%.06f,\"lon\":%.06f,\"accuracy\":%.01f,\"battery\":%d,\"ts_ms\":%lld}",
		cloud_config_device_id_get(),
		source,
		sample->fix_valid ? "true" : "false",
		sample->latitude,
		sample->longitude,
		(double)sample->accuracy,
		CONFIG_GNSS_SAMPLE_MQTT_BATTERY_PERCENT,
		sample->ts_ms);
#else
	if (sample->location_source == LOCATION_SOURCE_GNSS && sample->fix_valid) {
		if (sample->timestamp_valid) {
			return snprintk(buf, len,
				"{\"deviceId\":\"%s\",\"locationSource\":\"%s\",\"fixValid\":true,"
				"\"lat\":%.06f,\"lon\":%.06f,\"accuracy\":%.01f,"
				"\"alt_m\":%.01f,\"alt_acc_m\":%.01f,"
				"\"speed_mps\":%.01f,\"speed_acc_mps\":%.01f,"
				"\"v_speed_mps\":%.01f,\"v_speed_acc_mps\":%.01f,"
				"\"heading_deg\":%.01f,\"heading_acc_deg\":%.01f,"
				"\"pdop\":%.01f,\"hdop\":%.01f,\"vdop\":%.01f,\"tdop\":%.01f,"
				"\"timestamp\":\"%s\"}",
				cloud_config_device_id_get(), source,
				sample->latitude, sample->longitude, (double)sample->accuracy,
				(double)sample->altitude, (double)sample->altitude_accuracy,
				(double)sample->speed, (double)sample->speed_accuracy,
				(double)sample->vertical_speed, (double)sample->vertical_speed_accuracy,
				(double)sample->heading, (double)sample->heading_accuracy,
				(double)sample->pdop, (double)sample->hdop,
				(double)sample->vdop, (double)sample->tdop,
				sample->timestamp);
		}

		return snprintk(buf, len,
			"{\"deviceId\":\"%s\",\"locationSource\":\"%s\",\"fixValid\":true,"
			"\"lat\":%.06f,\"lon\":%.06f,\"accuracy\":%.01f,"
			"\"alt_m\":%.01f,\"alt_acc_m\":%.01f,"
			"\"speed_mps\":%.01f,\"speed_acc_mps\":%.01f,"
			"\"v_speed_mps\":%.01f,\"v_speed_acc_mps\":%.01f,"
			"\"heading_deg\":%.01f,\"heading_acc_deg\":%.01f,"
			"\"pdop\":%.01f,\"hdop\":%.01f,\"vdop\":%.01f,\"tdop\":%.01f,"
			"\"ts_ms\":%lld}",
			cloud_config_device_id_get(), source,
			sample->latitude, sample->longitude, (double)sample->accuracy,
			(double)sample->altitude, (double)sample->altitude_accuracy,
			(double)sample->speed, (double)sample->speed_accuracy,
			(double)sample->vertical_speed, (double)sample->vertical_speed_accuracy,
			(double)sample->heading, (double)sample->heading_accuracy,
			(double)sample->pdop, (double)sample->hdop,
			(double)sample->vdop, (double)sample->tdop,
			sample->ts_ms);
	}

	if (sample->timestamp_valid) {
		return snprintk(buf, len,
			"{\"deviceId\":\"%s\",\"locationSource\":\"%s\",\"fixValid\":%s,"
			"\"lat\":%.06f,\"lon\":%.06f,\"accuracy\":%.01f,\"timestamp\":\"%s\"}",
			cloud_config_device_id_get(),
			source,
			sample->fix_valid ? "true" : "false",
			sample->latitude,
			sample->longitude,
			(double)sample->accuracy,
			sample->timestamp);
	}

	return snprintk(buf, len,
		"{\"deviceId\":\"%s\",\"locationSource\":\"%s\",\"fixValid\":%s,"
		"\"lat\":%.06f,\"lon\":%.06f,\"accuracy\":%.01f,\"ts_ms\":%lld}",
		cloud_config_device_id_get(),
		source,
		sample->fix_valid ? "true" : "false",
		sample->latitude,
		sample->longitude,
		(double)sample->accuracy,
		sample->ts_ms);
#endif
}

int telemetry_cell_identity_to_json(const struct cellular_identity *identity,
				    char *buf, size_t len)
{
	if (!identity || !identity->valid || !buf || len == 0U) {
		return -EINVAL;
	}

	return snprintk(buf, len,
		"{\"deviceId\":\"%s\",\"locationSource\":\"cellular\",\"fixValid\":false,"
		"\"operator\":\"%s\",\"plmn\":\"%s\",\"mcc\":%d,\"mnc\":%d,"
		"\"tac\":%u,\"tacHex\":\"%s\",\"cellId\":%u,\"cellIdHex\":\"%s\","
		"\"rat\":%d,\"rsrp\":%d,\"ts_ms\":%lld}",
		cloud_config_device_id_get(),
		identity->operator_name,
		identity->plmn,
		identity->mcc,
		identity->mnc,
		identity->tac,
		identity->tac_hex,
		identity->cell_id,
		identity->cell_id_hex,
		identity->rat,
		identity->rsrp,
		identity->ts_ms);
}
