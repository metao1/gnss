package com.example.gnssble.ui.screens

import android.Manifest
import android.app.Activity
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.provider.Settings
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.AssistChip
import androidx.compose.material3.AssistChipDefaults
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.Alignment
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import com.example.gnssble.ble.GnssForegroundService
import com.example.gnssble.ui.viewmodel.GnssViewModel

@Composable
fun ScannerScreen(viewModel: GnssViewModel, padding: PaddingValues) {
    val devices by viewModel.devices.collectAsState()
    val isScanningBle by viewModel.isScanningBle.collectAsState()
    val state by viewModel.connectionState.collectAsState()
    val context = LocalContext.current
    var pendingAction by remember { mutableStateOf<(() -> Unit)?>(null) }
    var permissionMessage by remember { mutableStateOf<String?>(null) }
    var showSettingsButton by remember { mutableStateOf(false) }

    val permissionLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.RequestMultiplePermissions()
    ) { result ->
        val denied = result.filterValues { granted -> !granted }.keys.toList()
        if (denied.isEmpty()) {
            permissionMessage = null
            showSettingsButton = false
            val action = pendingAction
            pendingAction = null
            action?.invoke()
        } else {
            pendingAction = null
            showSettingsButton = denied.any { permission ->
                !context.shouldShowPermissionRationale(permission)
            }
            permissionMessage = if (showSettingsButton) {
                "Android did not grant: ${denied.joinToString { it.permissionLabel() }}. Open App Settings and enable permissions manually."
            } else {
                "Missing permission: ${denied.joinToString { it.permissionLabel() }}"
            }
        }
    }

    fun missingPermissions(required: List<String>): List<String> {
        return required.filter { permission ->
            ContextCompat.checkSelfPermission(context, permission) != PackageManager.PERMISSION_GRANTED
        }
    }

    fun runWithPermissions(required: List<String>, action: () -> Unit) {
        val missing = missingPermissions(required)
        if (missing.isEmpty()) {
            permissionMessage = null
            showSettingsButton = false
            action()
        } else {
            pendingAction = action
            permissionMessage = "Requesting: ${missing.joinToString { it.permissionLabel() }}"
            permissionLauncher.launch(missing.toTypedArray())
        }
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(padding)
            .padding(20.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        Text("Bluetooth Serial", style = MaterialTheme.typography.headlineMedium)
        val missingNow = missingPermissions(requiredBluetoothRuntimePermissions())

        LaunchedEffect(missingNow.isEmpty()) {
        if (missingNow.isEmpty()) {
            GnssForegroundService.start(context)
            viewModel.autoConnectStoredBle()
        }
    }

        if (missingNow.isNotEmpty() || permissionMessage != null) {
            Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant)) {
                Column(modifier = Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text("Required Android permissions", style = MaterialTheme.typography.titleMedium)
                    Text(
                        "Tap Grant Permissions before scanning: ${missingNow.joinToString { it.permissionLabel() }}",
                        style = MaterialTheme.typography.bodyMedium,
                    )
                    permissionMessage?.let {
                        Text(it, color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodyMedium)
                    }
                    OutlinedButton(onClick = {
                        runWithPermissions(requiredBluetoothRuntimePermissions()) {
                            viewModel.scanBle()
                        }
                    }) {
                        Text("Grant Permissions and Scan")
                    }
                    if (showSettingsButton) {
                        OutlinedButton(onClick = { context.openAppSettings() }) {
                            Text("Open App Settings")
                        }
                    }
                }
            }
        }

        Row(
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Button(
                onClick = {
                    runWithPermissions(requiredBluetoothRuntimePermissions()) { viewModel.scanBle() }
                },
                enabled = !isScanningBle,
                modifier = Modifier.size(72.dp),
                shape = CircleShape,
                colors = ButtonDefaults.buttonColors(
                    containerColor = Color(0xFFE3F2FD),
                    contentColor = Color(0xFF1976D2),
                    disabledContainerColor = Color(0xFFE3F2FD),
                    disabledContentColor = Color(0xFF1976D2),
                ),
                contentPadding = PaddingValues(0.dp),
            ) {
                if (isScanningBle) {
                    CircularProgressIndicator(
                        modifier = Modifier.size(30.dp),
                        color = Color(0xFF1976D2),
                        trackColor = Color(0xFFBBDEFB),
                        strokeWidth = 3.dp,
                    )
                } else {
                    Text("SCAN", style = MaterialTheme.typography.labelLarge)
                }
            }
            val status = connectionStatus(state)
            Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
                AssistChip(
                    onClick = {},
                    colors = AssistChipDefaults.assistChipColors(
                        containerColor = status.color.copy(alpha = 0.14f),
                        labelColor = status.color,
                    ),
                    label = { Text(status.label) },
                )
                status.errorReason?.let { reason ->
                    Text(
                        reason,
                        color = MaterialTheme.colorScheme.error,
                        style = MaterialTheme.typography.labelSmall,
                    )
                }
            }
        }

        LazyColumn(verticalArrangement = Arrangement.spacedBy(12.dp)) {
            items(devices, key = { it.address }) { device ->
                Card(
                    modifier = Modifier.fillMaxWidth(),
                    colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant),
                ) {
                    Column(modifier = Modifier.padding(16.dp)) {
                        Text(device.name, style = MaterialTheme.typography.titleLarge)
                        Text(device.address, style = MaterialTheme.typography.bodySmall)
                        Spacer(Modifier.height(12.dp))
                        if (device.supportsGnssUart) {
                            Button(onClick = {
                                runWithPermissions(requiredBluetoothRuntimePermissions()) {
                                    GnssForegroundService.start(context)
                                    viewModel.connect(device)
                                }
                            }) {
                                Text("Connect")
                            }
                        }
                    }
                }
            }
        }
    }
}

private data class ConnectionStatus(
    val label: String,
    val color: Color,
    val errorReason: String? = null,
)

private fun connectionStatus(raw: String): ConnectionStatus {
    val baseState = raw.substringBefore(" [")
    val deviceLabel = raw.substringAfter(" [", "").removeSuffix("]").trim()
    val suffix = deviceLabel.takeIf { it.isNotEmpty() }?.let { ": $it" } ?: ""
    return when {
        // The repository appends the connected device label, so accept both
        // "CONNECTED BLE" and the labelled form used by the repository.
        baseState.startsWith("CONNECTED BLE") -> ConnectionStatus("Connected$suffix", Color(0xFF2E7D32))
        baseState.startsWith("ERROR") -> ConnectionStatus(
            label = "Error$suffix",
            color = Color(0xFFC62828),
            errorReason = baseState.substringAfter(':', "Connection failed").trim().ifBlank { "Connection failed" },
        )
        baseState == "DISCONNECTED" -> ConnectionStatus("Disconnected$suffix", Color(0xFF607D8B))
        baseState.startsWith("CONNECT") || baseState.startsWith("RECONNECT") ->
            ConnectionStatus("Connecting$suffix", Color(0xFFEF6C00))
        else -> ConnectionStatus("Disconnected", Color(0xFF607D8B))
    }
}

private fun requiredBluetoothRuntimePermissions(): List<String> {
    return buildList {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            add(Manifest.permission.BLUETOOTH_CONNECT)
            add(Manifest.permission.BLUETOOTH_SCAN)
        } else {
            // Android 11 and earlier require location permission for BLE scan.
            add(Manifest.permission.ACCESS_FINE_LOCATION)
        }
    }
}

private fun Context.shouldShowPermissionRationale(permission: String): Boolean {
    val activity = findActivity() ?: return false
    return ActivityCompat.shouldShowRequestPermissionRationale(activity, permission)
}

private tailrec fun Context.findActivity(): Activity? {
    return when (this) {
        is Activity -> this
        is android.content.ContextWrapper -> baseContext.findActivity()
        else -> null
    }
}

private fun Context.openAppSettings() {
    val intent = Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS).apply {
        data = Uri.fromParts("package", packageName, null)
    }
    startActivity(intent)
}

private fun String.permissionLabel(): String {
    return when (this) {
        Manifest.permission.BLUETOOTH_CONNECT -> "Nearby devices / Bluetooth connect"
        Manifest.permission.BLUETOOTH_SCAN -> "Nearby devices / Bluetooth scan"
        Manifest.permission.ACCESS_COARSE_LOCATION -> "Approximate location"
        Manifest.permission.ACCESS_FINE_LOCATION -> "Location"
        Manifest.permission.POST_NOTIFICATIONS -> "Notifications"
        else -> substringAfterLast('.')
    }
}
