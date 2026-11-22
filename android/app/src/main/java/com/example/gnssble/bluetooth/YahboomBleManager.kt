package com.example.gnssble.bluetooth

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import com.example.gnssble.data.model.ClassicBluetoothDeviceUi
import com.example.gnssble.data.model.Transport
import dagger.hilt.android.qualifiers.ApplicationContext
import java.util.UUID
import java.util.concurrent.ConcurrentHashMap
import javax.inject.Inject
import javax.inject.Singleton
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

@Singleton
class YahboomBleManager @Inject constructor(
    @ApplicationContext private val context: Context,
) {
    private val adapter: BluetoothAdapter? = context.getSystemService(BluetoothManager::class.java)?.adapter
    private val _connectionState = MutableStateFlow("DISCONNECTED")
    val connectionState: StateFlow<String> = _connectionState.asStateFlow()
    private val discovered = ConcurrentHashMap<String, ClassicBluetoothDeviceUi>()
    private var gatt: BluetoothGatt? = null
    private var tx: BluetoothGattCharacteristic? = null
    private var callback: ((ByteArray) -> Unit)? = null
    private var scanCallback: ScanCallback? = null
    private val handler = Handler(Looper.getMainLooper())
    private var targetDevice: BluetoothDevice? = null
    private var connectAttempt = 0
    private var userDisconnected = false

    @SuppressLint("MissingPermission")
    fun scan(onComplete: (List<ClassicBluetoothDeviceUi>) -> Unit) {
        scan(null, onComplete)
    }

    @SuppressLint("MissingPermission")
    fun scan(targetAddress: String?, onComplete: (List<ClassicBluetoothDeviceUi>) -> Unit) {
        if (!hasScanPermission()) return onComplete(emptyList())
        stopScan()
        discovered.clear()
        val localAdapter = adapter ?: return onComplete(emptyList())
        val scanner = object : ScanCallback() {
            override fun onScanResult(callbackType: Int, result: ScanResult) {
                val device = result.device
                val name = result.scanRecord?.deviceName
                    ?: runCatching { device.name }.getOrNull()
                    ?: "BLE device ${device.address}"
                android.util.Log.d("YahboomBle", "BLE device: $name ${device.address}")
                // Show every BLE device, but record which devices advertise the
                // GNSS UART service so the UI can distinguish compatible targets.
                val isTarget = targetAddress?.equals(device.address, ignoreCase = true) == true
                val advertisesGnssService = result.scanRecord?.serviceUuids?.any {
                    it.uuid == UART_SERVICE_UUID
                } == true
                val previous = discovered[device.address]
                discovered[device.address] = ClassicBluetoothDeviceUi(
                    address = device.address,
                    name = name,
                    bonded = device.bondState == BluetoothDevice.BOND_BONDED,
                    device = device,
                    transport = Transport.BLE,
                    supportsGnssUart = advertisesGnssService || previous?.supportsGnssUart == true || isTarget,
                )
            }

            override fun onScanFailed(errorCode: Int) {
                android.util.Log.e("YahboomBle", "BLE scan failed: $errorCode")
            }
        }
        scanCallback = scanner
        localAdapter.bluetoothLeScanner?.startScan(
            null,
            ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build(),
            scanner,
        )
        android.os.Handler(android.os.Looper.getMainLooper()).postDelayed({
            stopScan()
            onComplete(discovered.values.sortedBy { it.name })
        }, 5000)
    }

    @SuppressLint("MissingPermission")
    fun stopScan() {
        scanCallback?.let { adapter?.bluetoothLeScanner?.stopScan(it) }
        scanCallback = null
    }

    @SuppressLint("MissingPermission")
    fun connect(device: BluetoothDevice, onBytes: (ByteArray) -> Unit) {
        if (!hasConnectPermission()) {
            _connectionState.value = "BLUETOOTH_CONNECT permission required"
            return
        }
        disconnect()
        userDisconnected = false
        targetDevice = device
        connectAttempt = 0
        callback = onBytes
        connectGatt()
    }

    @SuppressLint("MissingPermission")
    private fun connectGatt() {
        val device = targetDevice ?: return
        connectAttempt++
        _connectionState.value = if (connectAttempt == 1) {
            "CONNECTING BLE"
        } else {
            "RECONNECTING BLE ($connectAttempt/$MAX_CONNECT_ATTEMPTS)"
        }
        gatt = device.connectGatt(context, false, object : BluetoothGattCallback() {
            override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
                if (g !== gatt) return
                if (status != BluetoothGatt.GATT_SUCCESS) {
                    _connectionState.value = "ERROR: GATT $status"
                    closeGatt(g)
                    scheduleReconnect()
                } else if (newState == BluetoothProfile.STATE_CONNECTED) {
                    _connectionState.value = "CONNECTED BLE, discovering services"
                    if (!g.discoverServices()) {
                        closeGatt(g)
                        scheduleReconnect()
                    }
                } else {
                    _connectionState.value = "DISCONNECTED"
                    closeGatt(g)
                    scheduleReconnect()
                }
            }

            override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
                if (g !== gatt) return
                val service = g.getService(UART_SERVICE_UUID)
                tx = service?.getCharacteristic(UART_CHARACTERISTIC_UUID)
                val characteristic = tx
                if (status != BluetoothGatt.GATT_SUCCESS || characteristic == null) {
                    _connectionState.value = "ERROR: FFE0/FFE1 not found"
                    closeGatt(g)
                    scheduleReconnect()
                    return
                }
                connectAttempt = 0
                g.setCharacteristicNotification(characteristic, true)
                val cccd = characteristic.descriptors.firstOrNull { it.uuid == CCCD_UUID }
                if (cccd != null) {
                    val it = cccd
                    it.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                    g.writeDescriptor(it)
                    _connectionState.value = "CONNECTED BLE, enabling notifications"
                } else {
                    _connectionState.value = "CONNECTED BLE"
                }
                android.util.Log.d("YahboomBle", "GATT ready: FFE0/FFE1")
            }

            override fun onDescriptorWrite(g: BluetoothGatt, descriptor: BluetoothGattDescriptor, status: Int) {
                if (g !== gatt) return
                if (descriptor.uuid == CCCD_UUID && status == BluetoothGatt.GATT_SUCCESS) {
                    _connectionState.value = "CONNECTED BLE"
                    android.util.Log.d("YahboomBle", "Notifications enabled: FFE1")
                } else if (descriptor.uuid == CCCD_UUID) {
                    _connectionState.value = "ERROR: notification setup $status"
                    android.util.Log.e("YahboomBle", "Notification setup failed: $status")
                    closeGatt(g)
                    scheduleReconnect()
                }
            }

            override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic) {
                android.util.Log.d("YahboomBle", "Notification received: ${c.uuid} ${c.value.size} bytes")
                callback?.invoke(c.value.copyOf())
            }

            @Suppress("DEPRECATION")
            override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic, value: ByteArray) {
                android.util.Log.d("YahboomBle", "Notification received: ${c.uuid} ${value.size} bytes")
                callback?.invoke(value.copyOf())
            }
        })
    }

    private fun scheduleReconnect() {
        if (userDisconnected || targetDevice == null || connectAttempt >= MAX_CONNECT_ATTEMPTS) {
            return
        }
        handler.removeCallbacksAndMessages(null)
        handler.postDelayed({ connectGatt() }, RETRY_DELAY_MS)
    }

    @SuppressLint("MissingPermission")
    private fun closeGatt(connection: BluetoothGatt) {
        if (connection === gatt) {
            gatt = null
            tx = null
        }
        connection.disconnect()
        connection.close()
    }

    @SuppressLint("MissingPermission")
    fun sendCommand(command: String) {
        val g = gatt ?: return
        val characteristic = tx ?: return
        characteristic.value = (command.trim() + "\n").toByteArray(Charsets.UTF_8)
        characteristic.writeType = BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
        g.writeCharacteristic(characteristic)
    }

    @SuppressLint("MissingPermission")
    fun disconnect() {
        userDisconnected = true
        targetDevice = null
        handler.removeCallbacksAndMessages(null)
        gatt?.let { closeGatt(it) }
        gatt = null
        tx = null
        callback = null
        _connectionState.value = "DISCONNECTED"
    }

    private fun hasConnectPermission() = Build.VERSION.SDK_INT < 31 ||
        context.checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED

    private fun hasScanPermission() = Build.VERSION.SDK_INT < 31 ||
        context.checkSelfPermission(Manifest.permission.BLUETOOTH_SCAN) == PackageManager.PERMISSION_GRANTED

    companion object {
        private const val MAX_CONNECT_ATTEMPTS = 3
        private const val RETRY_DELAY_MS = 1200L
        val UART_SERVICE_UUID: UUID = UUID.fromString("0000FFE0-0000-1000-8000-00805F9B34FB")
        val UART_CHARACTERISTIC_UUID: UUID = UUID.fromString("0000FFE1-0000-1000-8000-00805F9B34FB")
        val CCCD_UUID: UUID = UUID.fromString("00002902-0000-1000-8000-00805F9B34FB")
    }
}
