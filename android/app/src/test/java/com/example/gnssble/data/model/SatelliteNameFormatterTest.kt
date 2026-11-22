package com.example.gnssble.data.model

import org.junit.Assert.assertEquals
import org.junit.Test

class SatelliteNameFormatterTest {
    @Test
    fun usesTleNameForMatchingGpsPrn() {
        val satellite = GnssSatellite("GPS-003", "GPS", 3, 40, 180, 35, true)
        val orbit = SatelliteOrbit("GPS BIIR-2 (PRN 03)", 1, 0L, 55.0, 0.0, 0.01, 0.0, 0.0, 2.0)

        assertEquals("GPS PRN 03 - GPS BIIR-2 (PRN 03)", satellite.displayName(listOf(orbit)))
    }

    @Test
    fun keepsPrnWhenNoMatchingTleExists() {
        val satellite = GnssSatellite("GPS-003", "GPS", 3, null, null, null, false)

        assertEquals("GPS PRN 03", satellite.displayName(emptyList()))
    }
}
