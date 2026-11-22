package com.example.gnssble.data.model

import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable

@Serializable
data class GnssFrame(
    val ts_ms: Long = 0,
    val fix_valid: Boolean = false,
    val lat: Double? = null,
    val lon: Double? = null,
    val alt_m: Double? = null,
    val speed_mps: Double? = null,
    val heading_deg: Double? = null,
    val accuracy_m: Double? = null,
    val hdop: Double? = null,
    val tracked: Int = 0,
    val used: Int = 0,
    val unhealthy: Int = 0,
    val nmea_gga: String? = null,
    val nmea_rmc: String? = null,
    val counters: Counters = Counters(),
    val type: String? = null,
    val code: String? = null,
    val message: String? = null,
    val details: String? = null,
)

@Serializable
data class Counters(
    val accepted: Int = 0,
    val rejected: Int = 0,
    val nmea_dropped: Int = 0,
    @SerialName("ble_dropped") val bleDropped: Int = 0,
    @SerialName("uart_rx_commands") val uartRxCommands: Int = 0,
    @SerialName("uart_rx_overflows") val uartRxOverflows: Int = 0,
)
