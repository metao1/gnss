package com.example.gnssble.data.model

private val PrnPattern = Regex("\\bPRN\\s*0*(\\d+)\\b", RegexOption.IGNORE_CASE)

fun GnssSatellite.displayName(orbits: List<SatelliteOrbit>): String {
    val signalName = when (system.uppercase()) {
        "GPS" -> "GPS PRN ${prn.toString().padStart(2, '0')}"
        "GLONASS" -> "GLONASS ${prn.toString().padStart(2, '0')}"
        "GALILEO" -> "Galileo E${prn.toString().padStart(2, '2')}"
        "BEIDOU" -> "BeiDou ${prn.toString().padStart(2, '2')}"
        else -> "$system ${prn.toString().padStart(2, '0')}"
    }
    if (!system.equals("GPS", ignoreCase = true)) return signalName

    val orbitName = orbits.firstOrNull { orbit ->
        PrnPattern.find(orbit.name)?.groupValues?.getOrNull(1)?.toIntOrNull() == prn
    }?.name
    return orbitName?.let { "$signalName - $it" } ?: signalName
}
