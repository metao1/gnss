package com.example.gnssble.data.model

import android.bluetooth.BluetoothDevice

data class ClassicBluetoothDeviceUi(
    val address: String,
    val name: String,
    val bonded: Boolean,
    val device: BluetoothDevice,
    val transport: Transport = Transport.CLASSIC,
    val supportsGnssUart: Boolean = false,
)

enum class Transport { CLASSIC, BLE }
