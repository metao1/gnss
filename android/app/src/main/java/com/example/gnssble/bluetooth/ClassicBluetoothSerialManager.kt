package com.example.gnssble.bluetooth

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothSocket
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import com.example.gnssble.data.model.ClassicBluetoothDeviceUi
import dagger.hilt.android.qualifiers.ApplicationContext
import java.io.BufferedReader
import java.io.IOException
import java.io.InputStreamReader
import java.util.UUID
import javax.inject.Inject
import javax.inject.Singleton
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

@Singleton
class ClassicBluetoothSerialManager @Inject constructor(
    @ApplicationContext private val context: Context,
) {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val btManager = context.getSystemService(BluetoothManager::class.java)
    private val adapter: BluetoothAdapter? get() = btManager?.adapter

    private val _connectionState = MutableStateFlow("DISCONNECTED")
    val connectionState: StateFlow<String> = _connectionState.asStateFlow()

    private var socket: BluetoothSocket? = null
    private var lineCallback: ((String) -> Unit)? = null

    @SuppressLint("MissingPermission")
    fun pairedDevices(): List<ClassicBluetoothDeviceUi> {
        val localAdapter = adapter ?: return emptyList()
        if (!hasBluetoothConnectPermission()) {
            _connectionState.value = "BLUETOOTH_CONNECT permission required"
            return emptyList()
        }
        return localAdapter.bondedDevices
            .map { device ->
                ClassicBluetoothDeviceUi(
                    address = device.address,
                    name = device.name ?: "Unnamed Bluetooth device",
                    bonded = device.bondState == BluetoothDevice.BOND_BONDED,
                    device = device,
                )
            }
            .sortedWith(compareByDescending<ClassicBluetoothDeviceUi> { it.name.contains("HC-06", ignoreCase = true) }
                .thenBy { it.name })
    }

    @SuppressLint("MissingPermission")
    fun connect(device: BluetoothDevice, onLine: (String) -> Unit) {
        if (!hasBluetoothConnectPermission()) {
            _connectionState.value = "BLUETOOTH_CONNECT permission required"
            return
        }
        disconnect()
        lineCallback = onLine
        _connectionState.value = "CONNECTING"

        scope.launch {
            runCatching {
                if (hasBluetoothScanPermission()) {
                    adapter?.cancelDiscovery()
                }
                val newSocket = device.createRfcommSocketToServiceRecord(SPP_UUID)
                newSocket.connect()
                socket = newSocket
                _connectionState.value = "CONNECTED"
                sendCommand("REQUEST_STATUS")
                readLoop(newSocket)
            }.onFailure { error ->
                _connectionState.value = "ERROR: ${error.message ?: error.javaClass.simpleName}"
                closeSocket()
            }
        }
    }

    private suspend fun readLoop(activeSocket: BluetoothSocket) = withContext(Dispatchers.IO) {
        val reader = BufferedReader(InputStreamReader(activeSocket.inputStream, Charsets.UTF_8))
        while (activeSocket.isConnected) {
            val line = reader.readLine() ?: break
            lineCallback?.invoke(line)
        }
        _connectionState.value = "DISCONNECTED"
        closeSocket()
    }

    fun sendCommand(command: String) {
        scope.launch {
            runCatching {
                val output = socket?.outputStream ?: throw IOException("Bluetooth serial is not connected")
                output.write((command.trim() + "\n").toByteArray(Charsets.UTF_8))
                output.flush()
            }.onFailure { error ->
                _connectionState.value = "ERROR: ${error.message ?: error.javaClass.simpleName}"
            }
        }
    }

    fun disconnect() {
        _connectionState.value = "DISCONNECTED"
        closeSocket()
    }

    private fun closeSocket() {
        runCatching { socket?.close() }
        socket = null
    }

    private fun hasBluetoothConnectPermission(): Boolean {
        return Build.VERSION.SDK_INT < Build.VERSION_CODES.S ||
            context.checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) ==
            PackageManager.PERMISSION_GRANTED
    }

    private fun hasBluetoothScanPermission(): Boolean {
        return Build.VERSION.SDK_INT < Build.VERSION_CODES.S ||
            context.checkSelfPermission(Manifest.permission.BLUETOOTH_SCAN) ==
            PackageManager.PERMISSION_GRANTED
    }

    companion object {
        val SPP_UUID: UUID = UUID.fromString("00001101-0000-1000-8000-00805F9B34FB")
    }
}
