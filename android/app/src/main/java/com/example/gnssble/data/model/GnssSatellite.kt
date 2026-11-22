package com.example.gnssble.data.model

data class GnssSatellite(
    val name: String,
    val system: String,
    val prn: Int,
    val elevationDeg: Int?,
    val azimuthDeg: Int?,
    val cn0DbHz: Int?,
    val used: Boolean,
)
