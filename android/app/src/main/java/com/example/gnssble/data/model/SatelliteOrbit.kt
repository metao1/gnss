package com.example.gnssble.data.model

data class SatelliteOrbit(
    val name: String,
    val noradId: Int,
    val epochMs: Long,
    val inclinationDeg: Double,
    val raanDeg: Double,
    val eccentricity: Double,
    val argumentOfPerigeeDeg: Double,
    val meanAnomalyDeg: Double,
    val meanMotionRevsPerDay: Double,
)

data class SatellitePosition(
    val latitudeDeg: Double,
    val longitudeDeg: Double,
    val altitudeKm: Double,
    val timestampMs: Long,
)
