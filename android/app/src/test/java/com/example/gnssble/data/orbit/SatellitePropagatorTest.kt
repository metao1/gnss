package com.example.gnssble.data.orbit

import com.example.gnssble.data.model.SatelliteOrbit
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class SatellitePropagatorTest {
    private val orbit = SatelliteOrbit(
        name = "GPS TEST",
        noradId = 1,
        epochMs = 1_700_000_000_000L,
        inclinationDeg = 55.0,
        raanDeg = 120.0,
        eccentricity = 0.01,
        argumentOfPerigeeDeg = 40.0,
        meanAnomalyDeg = 15.0,
        meanMotionRevsPerDay = 2.0,
    )

    @Test
    fun propagationStaysWithinEarthCoordinateBounds() {
        val position = SatellitePropagator.propagate(orbit, orbit.epochMs + 60_000L)

        assertTrue(position.latitudeDeg in -90.0..90.0)
        assertTrue(position.longitudeDeg >= -180.0)
        assertTrue(position.longitudeDeg < 180.0)
        assertTrue(position.altitudeKm > 0.0)
    }

    @Test
    fun longitudeWrapsToAntimeridianRange() {
        assertEquals(-180.0, SatellitePropagator.normalizeDegrees(180.0), 0.000001)
        assertEquals(10.0, SatellitePropagator.normalizeDegrees(370.0), 0.000001)
        assertEquals(-10.0, SatellitePropagator.normalizeDegrees(-370.0), 0.000001)
    }
}
