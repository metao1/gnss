package com.example.gnssble.ui.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import com.example.gnssble.ble.GnssForegroundService
import com.example.gnssble.ui.viewmodel.GnssViewModel

@Composable
fun DashboardScreen(viewModel: GnssViewModel, padding: PaddingValues) {
    val frame by viewModel.latestFrame.collectAsState()
    val state by viewModel.connectionState.collectAsState()
    val context = LocalContext.current

    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(padding)
            .padding(20.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        Text("Live GNSS", style = MaterialTheme.typography.headlineMedium)
        Text("Bluetooth serial state: $state")

        Card(modifier = Modifier.fillMaxWidth()) {
            Column(modifier = Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("Fix: ${if (frame?.fix_valid == true) "valid" else "searching"}")
                Text("Lat/Lon: ${frame?.lat ?: "-"}, ${frame?.lon ?: "-"}")
                Text("Accuracy: ${frame?.accuracy_m ?: "-"} m")
                Text("Altitude: ${frame?.alt_m ?: "-"} m")
                Text("Speed: ${frame?.speed_mps ?: "-"} m/s")
                Text("Heading: ${frame?.heading_deg ?: "-"} deg")
                Text("Tracked/Used/Unhealthy: ${frame?.tracked ?: 0}/${frame?.used ?: 0}/${frame?.unhealthy ?: 0}")
                Text("Counters: ${frame?.counters ?: "-"}")
                if (frame?.type == "status") {
                    Text("Status: ${frame?.code} ${frame?.message.orEmpty()}")
                }
            }
        }

        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Button(onClick = { viewModel.ping() }) { Text("Ping") }
            Button(onClick = { viewModel.requestStatus() }) { Text("Status") }
        }
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            OutlinedButton(onClick = { viewModel.startStream() }) { Text("Start") }
            OutlinedButton(onClick = { viewModel.stopStream() }) { Text("Stop") }
            OutlinedButton(onClick = { viewModel.setRate(1) }) { Text("1 Hz") }
            OutlinedButton(onClick = { viewModel.setRate(5) }) { Text("5 Hz") }
        }
        Button(onClick = {
            viewModel.disconnect()
            GnssForegroundService.stop(context)
        }) { Text("Disconnect") }
    }
}
