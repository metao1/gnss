package com.example.gnssble.ui.screens

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
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Button
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.example.gnssble.data.model.GnssSatellite
import com.example.gnssble.data.model.displayName
import com.example.gnssble.ui.viewmodel.GnssViewModel
import com.example.gnssble.ui.components.EarthGlobeView
import androidx.compose.ui.viewinterop.AndroidView
import com.example.gnssble.data.model.SatelliteOrbit
import kotlinx.coroutines.delay

@Composable
fun EarthGlobeScreen(viewModel: GnssViewModel, padding: PaddingValues) {
    val orbitState by viewModel.orbitState.collectAsState()
    val frame by viewModel.latestFrame.collectAsState()
    val lastValidFrame by viewModel.lastValidFrame.collectAsState()
    val satellites by viewModel.satellites.collectAsState()
    var timestampMs by remember { mutableLongStateOf(System.currentTimeMillis()) }
    var lastTickMs by remember { mutableLongStateOf(System.currentTimeMillis()) }
    var expanded by remember { mutableStateOf(false) }
    var selectedKey by remember { mutableStateOf<String?>(null) }
    val receiver = (frame?.takeIf { it.fix_valid } ?: lastValidFrame)?.let { current ->
        current.lat?.let { lat -> current.lon?.let { lon -> lat to lon } }
    }

    LaunchedEffect(Unit) {
        lastTickMs = System.currentTimeMillis()
        while (true) {
            val now = System.currentTimeMillis()
            timestampMs += ((now - lastTickMs) * ORBIT_SIMULATION_SPEED).toLong()
            lastTickMs = now
            delay(50L)
        }
    }

    Column(
        modifier = Modifier.fillMaxSize().padding(padding).padding(horizontal = 12.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        if (!expanded) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = androidx.compose.ui.Alignment.CenterVertically,
            ) {
                Column {
                    Text("Orbital Earth", style = MaterialTheme.typography.headlineSmall)
                    Text("Computed satellite positions, not GNSS measurements.", style = MaterialTheme.typography.bodySmall)
                }
                Button(onClick = { expanded = true }) { Text("Full") }
            }
        }

        AndroidView(
            modifier = if (expanded) Modifier.fillMaxWidth().weight(1f) else Modifier.fillMaxWidth().height(350.dp),
            factory = { EarthGlobeView(it) },
            update = { globe ->
                globe.update(
                    orbits = orbitState.orbits,
                    receiver = receiver,
                    timestampMs = timestampMs,
                )
            },
        )

        if (expanded) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.End,
            ) {
                Button(onClick = { expanded = false }) { Text("Exit") }
            }
        }

        if (!expanded) {
            if (satellites.isEmpty()) {
                Text("Waiting for measured satellite data...", style = MaterialTheme.typography.titleMedium)
            } else {
                Text("${satellites.size} satellites received", style = MaterialTheme.typography.titleMedium)
                val selected = satellites.firstOrNull { "${it.system}:${it.prn}" == selectedKey }
                selected?.let {
                    Card(modifier = Modifier.fillMaxWidth()) {
                        Column(modifier = Modifier.padding(12.dp)) {
                            Text(it.displayName(orbitState.orbits), style = MaterialTheme.typography.titleMedium)
                            Text(
                                "Signal ${it.cn0DbHz ?: "-"} dB-Hz  |  Elevation ${it.elevationDeg ?: "-"}°  |  Azimuth ${it.azimuthDeg ?: "-"}°",
                                style = MaterialTheme.typography.bodySmall,
                            )
                        }
                    }
                }
                LazyColumn(
                    modifier = Modifier.fillMaxWidth().weight(1f),
                    verticalArrangement = Arrangement.spacedBy(6.dp),
                ) {
                    items(satellites, key = { "${it.system}:${it.prn}" }) { satellite ->
                        EarthSatelliteRow(
                            satellite,
                            orbitState.orbits,
                            selected = selectedKey == "${satellite.system}:${satellite.prn}",
                            onClick = { selectedKey = "${satellite.system}:${satellite.prn}" },
                        )
                    }
                }
            }
        }
    }
}

private const val ORBIT_SIMULATION_SPEED = 60L

@Composable
private fun EarthSatelliteRow(
    satellite: GnssSatellite,
    orbits: List<SatelliteOrbit>,
    selected: Boolean,
    onClick: () -> Unit,
) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        onClick = onClick,
        colors = if (selected) {
            CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.secondaryContainer)
        } else {
            CardDefaults.cardColors()
        },
    ) {
        Row(
            modifier = Modifier.fillMaxWidth().padding(10.dp),
            horizontalArrangement = Arrangement.SpaceBetween,
        ) {
            Text(satellite.displayName(orbits), style = MaterialTheme.typography.titleSmall)
            Text(
                "C/N0 ${satellite.cn0DbHz ?: "-"}  El ${satellite.elevationDeg ?: "-"}°  " +
                    "Az ${satellite.azimuthDeg ?: "-"}°  ${if (satellite.used) "USED" else "TRACKED"}",
                style = MaterialTheme.typography.bodySmall,
            )
        }
    }
}

