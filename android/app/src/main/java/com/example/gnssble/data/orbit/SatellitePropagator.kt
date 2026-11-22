package com.example.gnssble.data.orbit

import com.example.gnssble.data.model.SatelliteOrbit
import com.example.gnssble.data.model.SatellitePosition
import kotlin.math.PI
import kotlin.math.atan2
import kotlin.math.cos
import kotlin.math.sin
import kotlin.math.sqrt

/** Approximate two-body propagator for visualization only, never for navigation. */
object SatellitePropagator {
    private const val EarthRadiusKm = 6378.137
    private const val EarthMuKm3PerSec2 = 398600.4418
    private const val SecondsPerDay = 86400.0

    fun propagate(orbit: SatelliteOrbit, timestampMs: Long): SatellitePosition {
        val meanMotion = orbit.meanMotionRevsPerDay.coerceAtLeast(0.01) * 2.0 * PI / SecondsPerDay
        val semiMajorAxis = (EarthMuKm3PerSec2 / (meanMotion * meanMotion)).let { it.powOneThird() }
        val eccentricity = orbit.eccentricity.coerceIn(0.0, 0.99)
        val elapsedSeconds = (timestampMs - orbit.epochMs) / 1000.0
        val meanAnomaly = normalizeRadians(degreesToRadians(orbit.meanAnomalyDeg) + meanMotion * elapsedSeconds)
        val eccentricAnomaly = solveKepler(meanAnomaly, eccentricity)
        val trueAnomaly = atan2(
            sqrt(1.0 - eccentricity * eccentricity) * sin(eccentricAnomaly),
            cos(eccentricAnomaly) - eccentricity,
        )
        val radius = semiMajorAxis * (1.0 - eccentricity * cos(eccentricAnomaly))
        val argumentOfLatitude = degreesToRadians(orbit.argumentOfPerigeeDeg) + trueAnomaly
        val xOrbital = radius * cos(argumentOfLatitude)
        val yOrbital = radius * sin(argumentOfLatitude)
        val inclination = degreesToRadians(orbit.inclinationDeg)
        val raan = degreesToRadians(orbit.raanDeg)

        val xEci = xOrbital * cos(raan) - yOrbital * sin(raan) * cos(inclination)
        val yEci = xOrbital * sin(raan) + yOrbital * cos(raan) * cos(inclination)
        val zEci = yOrbital * sin(inclination)
        val earthRotation = greenwichMeanSiderealTime(timestampMs)
        val xEcef = xEci * cos(earthRotation) + yEci * sin(earthRotation)
        val yEcef = -xEci * sin(earthRotation) + yEci * cos(earthRotation)
        val longitude = normalizeDegrees(Math.toDegrees(atan2(yEcef, xEcef)))
        val latitude = Math.toDegrees(atan2(zEci, sqrt(xEcef * xEcef + yEcef * yEcef)))

        return SatellitePosition(
            latitudeDeg = latitude.coerceIn(-90.0, 90.0),
            longitudeDeg = longitude,
            altitudeKm = (radius - EarthRadiusKm).coerceAtLeast(0.0),
            timestampMs = timestampMs,
        )
    }

    fun normalizeDegrees(value: Double): Double = ((value + 180.0) % 360.0 + 360.0) % 360.0 - 180.0

    private fun solveKepler(meanAnomaly: Double, eccentricity: Double): Double {
        var estimate = meanAnomaly
        repeat(10) {
            estimate -= (estimate - eccentricity * sin(estimate) - meanAnomaly) /
                (1.0 - eccentricity * cos(estimate))
        }
        return estimate
    }

    private fun degreesToRadians(value: Double): Double = Math.toRadians(value)

    private fun normalizeRadians(value: Double): Double {
        val twoPi = 2.0 * PI
        return (value % twoPi + twoPi) % twoPi
    }

    private fun greenwichMeanSiderealTime(timestampMs: Long): Double {
        val julianDate = timestampMs / 86400000.0 + 2440587.5
        val centuries = (julianDate - 2451545.0) / 36525.0
        val degrees = 280.46061837 + 360.98564736629 * (julianDate - 2451545.0) +
            0.000387933 * centuries * centuries - centuries * centuries * centuries / 38710000.0
        return Math.toRadians(normalizeDegrees(degrees))
    }

    private fun Double.powOneThird(): Double = Math.cbrt(this)
}
