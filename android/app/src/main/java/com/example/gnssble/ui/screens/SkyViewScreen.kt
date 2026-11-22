package com.example.gnssble.ui.screens

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.Card
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.tween
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.unit.dp
import com.example.gnssble.data.model.GnssSatellite
import com.example.gnssble.data.model.displayName
import com.example.gnssble.ui.viewmodel.GnssViewModel
import kotlin.math.cos
import kotlin.math.sin

@Composable
fun SkyViewScreen(viewModel: GnssViewModel, padding: PaddingValues) {
    val satellites by viewModel.satellites.collectAsState()
    val orbitState by viewModel.orbitState.collectAsState()

    Column(
        modifier = Modifier.fillMaxSize().padding(padding).padding(horizontal = 16.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Text("GNSS Sky View", style = MaterialTheme.typography.headlineSmall)
        Text("Receiver-relative view: north is up, horizon is the outer ring.")
        if (satellites.isEmpty()) {
            Text("Waiting for GSV satellite data...")
        } else {
            SkyPlot(satellites)
            Text("${satellites.size} satellites received", style = MaterialTheme.typography.titleMedium)
            LazyColumn(verticalArrangement = Arrangement.spacedBy(6.dp)) {
                items(satellites, key = { "${it.system}:${it.prn}" }) { satellite ->
                    SatelliteRow(satellite, orbitState.orbits)
                }
            }
        }
    }
}

@Composable
private fun SkyPlot(satellites: List<GnssSatellite>) {
    val animatedSatellites = satellites.map { satellite ->
        val animatedElevation by animateFloatAsState(
            targetValue = (satellite.elevationDeg ?: 0).toFloat(),
            animationSpec = tween(durationMillis = 650),
            label = "${satellite.system}-${satellite.prn}-elevation",
        )
        val animatedAzimuth by animateFloatAsState(
            targetValue = (satellite.azimuthDeg ?: 0).toFloat(),
            animationSpec = tween(durationMillis = 650),
            label = "${satellite.system}-${satellite.prn}-azimuth",
        )
        satellite to (animatedElevation to animatedAzimuth)
    }

    Canvas(modifier = Modifier.fillMaxWidth().height(270.dp)) {
        val center = Offset(size.width / 2f, size.height / 2f)
        val radius = minOf(size.width, size.height) * 0.42f
        val ringColor = Color(0xFF90A4AE)
        drawCircle(ringColor, radius, center, style = Stroke(2f))
        drawCircle(ringColor.copy(alpha = 0.65f), radius * 0.66f, center, style = Stroke(1f))
        drawCircle(ringColor.copy(alpha = 0.45f), radius * 0.33f, center, style = Stroke(1f))
        drawLine(ringColor.copy(alpha = 0.5f), Offset(center.x - radius, center.y), Offset(center.x + radius, center.y))
        drawLine(ringColor.copy(alpha = 0.5f), Offset(center.x, center.y - radius), Offset(center.x, center.y + radius))

        animatedSatellites.forEach { (satellite, position) ->
            if (satellite.elevationDeg == null || satellite.azimuthDeg == null) return@forEach
            val elevation = position.first
            val azimuth = position.second
            val distance = radius * (90f - elevation.coerceIn(0f, 90f)) / 90f
            val angle = Math.toRadians(azimuth.toDouble())
            val point = Offset(
                center.x + (sin(angle) * distance).toFloat(),
                center.y - (cos(angle) * distance).toFloat(),
            )
            val color = when {
                satellite.used -> Color(0xFF2E7D32)
                (satellite.cn0DbHz ?: 0) >= 35 -> Color(0xFF1976D2)
                (satellite.cn0DbHz ?: 0) >= 20 -> Color(0xFFF9A825)
                else -> Color(0xFFC62828)
            }
            drawCircle(color, 8f, point)
        }
    }
}

@Composable
private fun SatelliteRow(satellite: GnssSatellite, orbits: List<com.example.gnssble.data.model.SatelliteOrbit>) {
    Card(modifier = Modifier.fillMaxWidth()) {
        Row(modifier = Modifier.fillMaxWidth().padding(10.dp), horizontalArrangement = Arrangement.SpaceBetween) {
            Text(satellite.displayName(orbits), style = MaterialTheme.typography.titleSmall)
            Text(
                "C/N0 ${satellite.cn0DbHz ?: "-"}  El ${satellite.elevationDeg ?: "-"}°  Az ${satellite.azimuthDeg ?: "-"}°  ${if (satellite.used) "USED" else "TRACKED"}",
                style = MaterialTheme.typography.bodySmall,
            )
        }
    }
}
