package com.example.gnssble.ui.viewmodel

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.example.gnssble.data.db.SessionEntity
import com.example.gnssble.data.model.ClassicBluetoothDeviceUi
import com.example.gnssble.data.model.GnssFrame
import com.example.gnssble.data.model.GnssSatellite
import com.example.gnssble.data.orbit.OrbitRepository
import com.example.gnssble.data.orbit.OrbitState
import com.example.gnssble.data.repo.GnssRepository
import com.example.gnssble.data.repo.StoredPositionFile
import dagger.hilt.android.lifecycle.HiltViewModel
import javax.inject.Inject
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch

@HiltViewModel
class GnssViewModel @Inject constructor(
    private val repo: GnssRepository,
    private val orbitRepository: OrbitRepository,
) : ViewModel() {
    private val _devices = MutableStateFlow<List<ClassicBluetoothDeviceUi>>(emptyList())
    val devices: StateFlow<List<ClassicBluetoothDeviceUi>> = _devices.asStateFlow()

    private val _isScanningBle = MutableStateFlow(false)
    val isScanningBle: StateFlow<Boolean> = _isScanningBle.asStateFlow()

    private val _storedPositionFiles = MutableStateFlow<List<StoredPositionFile>>(emptyList())
    val storedPositionFiles: StateFlow<List<StoredPositionFile>> = _storedPositionFiles.asStateFlow()

    val latestFrame: StateFlow<GnssFrame?> = repo.latestFrame
    val lastValidFrame: StateFlow<GnssFrame?> = repo.lastValidFrame
    val trackSegments: StateFlow<List<List<Pair<Double, Double>>>> = repo.trackSegments
    val debugLines = repo.debugLines
    val liveDisplay = repo.liveDisplay
    val connectionState = repo.connectionState
    val satellites: StateFlow<List<GnssSatellite>> = repo.satellites
    val orbitState: StateFlow<OrbitState> = orbitRepository.state

    private val _sessions = MutableStateFlow<List<SessionEntity>>(emptyList())
    val sessions: StateFlow<List<SessionEntity>> = _sessions.asStateFlow()

    init {
        refreshStoredPositionFiles()
        viewModelScope.launch { orbitRepository.refresh() }
        viewModelScope.launch {
            repo.sessions().collect { _sessions.value = it }
        }
    }

    fun refreshOrbits() {
        viewModelScope.launch { orbitRepository.refresh() }
    }

    fun refreshPairedDevices() {
        _devices.value = repo.pairedDevices()
    }

    fun refreshStoredPositionFiles() {
        _storedPositionFiles.value = repo.storedPositionFiles()
    }

    fun loadStoredRoute(file: StoredPositionFile) {
        viewModelScope.launch {
            repo.showStoredTrack(repo.loadStoredPositionFile(file.name))
        }
    }

    fun positionFile(file: StoredPositionFile) = repo.positionFile(file.name)

    fun scanBle() {
        if (_isScanningBle.value) return
        _isScanningBle.value = true
        repo.scanBle { found ->
            try {
                _devices.value = (_devices.value.filter { it.transport.name != "BLE" } + found)
                    .distinctBy { it.address }
                    .sortedBy { it.name }
            } finally {
                _isScanningBle.value = false
            }
        }
    }

    fun autoConnectStoredBle() = repo.autoConnectStoredBle { found ->
        _devices.value = (_devices.value.filter { it.transport.name != "BLE" } + found)
            .distinctBy { it.address }
            .sortedBy { it.name }
    }

    fun connect(device: ClassicBluetoothDeviceUi) = repo.connect(device)
    fun disconnect() = repo.disconnect()
    fun ping() = repo.sendCommand("PING")
    fun requestStatus() = repo.sendCommand("REQUEST_STATUS")
    fun startStream() = repo.sendCommand("START_STREAM")
    fun stopStream() = repo.sendCommand("STOP_STREAM")
    fun setRate(hz: Int) = repo.sendCommand("SET_RATE:$hz")

    suspend fun export(sessionId: Long) = repo.exportSession(sessionId)
}
