package com.example.gnssble.data.orbit

import android.content.Context
import com.example.gnssble.data.model.SatelliteOrbit
import dagger.hilt.android.qualifiers.ApplicationContext
import java.net.HttpURLConnection
import java.net.URL
import java.time.Instant
import java.time.LocalDate
import java.time.ZoneOffset
import java.util.Locale
import javax.inject.Inject
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.withContext

data class OrbitState(
    val orbits: List<SatelliteOrbit> = emptyList(),
    val loading: Boolean = false,
    val error: String? = null,
    val source: String = "none",
    val fetchedAtMs: Long = 0L,
)

object TleParser {
    fun parse(text: String, nowMs: Long = System.currentTimeMillis()): List<SatelliteOrbit> {
        val lines = text.lineSequence().map { it.trimEnd() }.filter { it.isNotBlank() }.toList()
        val result = mutableListOf<SatelliteOrbit>()
        var index = 0
        while (index + 2 < lines.size) {
            val name = lines[index].trim()
            val line1 = lines[index + 1]
            val line2 = lines[index + 2]
            if (line1.startsWith("1 ") && line2.startsWith("2 ")) {
                parseRecord(name, line1, line2, nowMs)?.let(result::add)
                index += 3
            } else {
                index++
            }
        }
        return result
    }

    private fun parseRecord(name: String, line1: String, line2: String, nowMs: Long): SatelliteOrbit? = runCatching {
        val epochYear = line1.substring(18, 20).trim().toInt()
        val epochDay = line1.substring(20, 32).trim().toDouble()
        val fullYear = if (epochYear < 57) 2000 + epochYear else 1900 + epochYear
        val epochMs = LocalDate.of(fullYear, 1, 1)
            .atStartOfDay(ZoneOffset.UTC)
            .toInstant()
            .toEpochMilli() + ((epochDay - 1.0) * 86_400_000.0).toLong()
        val noradId = line2.substring(2, 7).trim().toInt()
        val eccentricity = "0.${line2.substring(26, 33).trim()}".toDouble()
        SatelliteOrbit(
            name = name,
            noradId = noradId,
            epochMs = epochMs,
            inclinationDeg = line2.substring(8, 16).trim().toDouble(),
            raanDeg = line2.substring(17, 25).trim().toDouble(),
            eccentricity = eccentricity,
            argumentOfPerigeeDeg = line2.substring(34, 42).trim().toDouble(),
            meanAnomalyDeg = line2.substring(43, 51).trim().toDouble(),
            meanMotionRevsPerDay = line2.substring(52, 63).trim().toDouble(),
        )
    }.getOrNull()
}

class OrbitRepository @Inject constructor(
    @ApplicationContext context: Context,
) {
    private val preferences = context.getSharedPreferences("satellite_orbits", Context.MODE_PRIVATE)
    private val _state = MutableStateFlow(loadCachedState())
    val state: StateFlow<OrbitState> = _state.asStateFlow()

    suspend fun refresh(): Result<Int> = withContext(Dispatchers.IO) {
        val cached = _state.value
        if (cached.orbits.isNotEmpty() && System.currentTimeMillis() - cached.fetchedAtMs < CACHE_MAX_AGE_MS) {
            return@withContext Result.success(cached.orbits.size)
        }
        _state.value = cached.copy(loading = true, error = null)
        runCatching {
            val text = downloadGpsTle()
            val parsed = TleParser.parse(text)
            require(parsed.isNotEmpty()) { "No valid GPS TLE records" }
            val fetchedAt = System.currentTimeMillis()
            preferences.edit().putString(KEY_TEXT, text).putLong(KEY_FETCHED_AT, fetchedAt).apply()
            _state.value = OrbitState(parsed, source = "network", fetchedAtMs = fetchedAt)
            parsed.size
        }.onFailure { error ->
            _state.value = cached.copy(loading = false, error = error.message ?: "Orbit data unavailable")
        }
    }

    private fun loadCachedState(): OrbitState {
        val text = preferences.getString(KEY_TEXT, null) ?: return OrbitState()
        val fetchedAt = preferences.getLong(KEY_FETCHED_AT, 0L)
        return OrbitState(
            orbits = TleParser.parse(text),
            source = "cache",
            fetchedAtMs = fetchedAt,
        )
    }

    private fun downloadGpsTle(): String {
        val connection = URL(TLE_URL).openConnection() as HttpURLConnection
        return try {
            connection.connectTimeout = 8_000
            connection.readTimeout = 8_000
            connection.setRequestProperty("User-Agent", "GNSSBLE/1.0 Android")
            connection.inputStream.bufferedReader().use { it.readText() }
        } finally {
            connection.disconnect()
        }
    }

    private companion object {
        const val TLE_URL = "https://celestrak.org/NORAD/elements/gp.php?GROUP=gps-ops&FORMAT=tle"
        const val CACHE_MAX_AGE_MS = 60L * 60L * 1000L
        const val KEY_TEXT = "tle_text"
        const val KEY_FETCHED_AT = "tle_fetched_at"
    }
}
