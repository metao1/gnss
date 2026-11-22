package com.example.gnssble.data.repo

import android.content.Context
import androidx.room.Room
import java.io.File
import java.io.FileInputStream
import java.time.Instant
import java.time.ZoneId
import com.example.gnssble.bluetooth.ClassicBluetoothSerialManager
import com.example.gnssble.bluetooth.YahboomBleManager
import com.example.gnssble.data.db.AppDatabase
import com.example.gnssble.data.db.DebugEventEntity
import com.example.gnssble.data.db.SessionEntity
import com.example.gnssble.data.db.TrackPointEntity
import com.example.gnssble.data.model.ClassicBluetoothDeviceUi
import com.example.gnssble.data.model.Transport
import com.example.gnssble.data.model.GnssFrame
import com.example.gnssble.data.model.GnssSatellite
import dagger.hilt.android.qualifiers.ApplicationContext
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
import kotlinx.serialization.json.Json
import kotlin.math.max

@Singleton
class GnssRepository @Inject constructor(
    @ApplicationContext context: Context,
    private val bluetooth: ClassicBluetoothSerialManager,
    private val ble: YahboomBleManager,
) {
    private val connectionPrefs = context.getSharedPreferences("ble_connection", Context.MODE_PRIVATE)
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val json = Json { ignoreUnknownKeys = true }
    private val db = Room.databaseBuilder(context, AppDatabase::class.java, "gnss_serial.db").build()
    private val positionLogDirectory = File(context.filesDir, "positions")
    private val debugLogFile = File(context.filesDir, "debug.log")
    private val logFileLock = Any()

    fun storedPositionFiles(): List<StoredPositionFile> {
        return positionLogDirectory.listFiles { file ->
            file.isFile && file.name.startsWith("positions-") && file.name.endsWith(".jsonl")
        }?.sortedByDescending { it.name }?.map {
            StoredPositionFile(it.name, it.length())
        } ?: emptyList()
    }

    fun positionFile(name: String): File? {
        val file = File(positionLogDirectory, name)
        return if (file.parentFile?.canonicalFile == positionLogDirectory.canonicalFile && file.isFile) file else null
    }

    suspend fun loadStoredPositionFile(name: String): List<List<Pair<Double, Double>>> = withContext(Dispatchers.IO) {
        val file = positionFile(name) ?: return@withContext emptyList()
        val segments = mutableListOf<MutableList<Pair<Double, Double>>>()
        var current: MutableList<Pair<Double, Double>>? = null
        file.forEachLine { line ->
            if (line.contains("\"type\":\"gap\"")) {
                current = null
                return@forEachLine
            }
            val frame = runCatching { json.decodeFromString(GnssFrame.serializer(), line) }.getOrNull()
            val lat = frame?.lat
            val lon = frame?.lon
            if (frame?.fix_valid == true && lat != null && lon != null && lat.isFinite() && lon.isFinite()) {
                if (current == null) {
                    current = mutableListOf()
                    segments += current!!
                }
                current!!.add(lat to lon)
            }
        }
        segments.takeLast(MAX_TRACK_SEGMENTS).map { it.takeLast(MAX_POINTS_PER_SEGMENT) }
    }

    fun showStoredTrack(segments: List<List<Pair<Double, Double>>>) {
        _trackSegments.value = segments
        lastAcceptedPoint = null
        trackGapPending = true
    }

    private val _latestFrame = MutableStateFlow<GnssFrame?>(null)
    val latestFrame: StateFlow<GnssFrame?> = _latestFrame.asStateFlow()

    private val _lastValidFrame = MutableStateFlow<GnssFrame?>(null)
    val lastValidFrame: StateFlow<GnssFrame?> = _lastValidFrame.asStateFlow()

    private val _trackSegments = MutableStateFlow<List<List<Pair<Double, Double>>>>(emptyList())
    val trackSegments: StateFlow<List<List<Pair<Double, Double>>>> = _trackSegments.asStateFlow()

    private val _debugLines = MutableStateFlow<List<String>>(emptyList())
    val debugLines: StateFlow<List<String>> = _debugLines.asStateFlow()

    private val _satellites = MutableStateFlow<List<GnssSatellite>>(emptyList())
    val satellites: StateFlow<List<GnssSatellite>> = _satellites.asStateFlow()

    private val _liveDisplay = MutableStateFlow("")
    val liveDisplay: StateFlow<String> = _liveDisplay.asStateFlow()
    private val liveDisplayBuffer = StringBuilder()

    private val trackingPattern = Regex("Tracking:\\s*(\\d+)\\s+Using:\\s*(\\d+)\\s+Unhealthy:\\s*(\\d+)")
    private val latitudePattern = Regex("Latitude:\\s*([-+]?\\d+(?:\\.\\d+)?)")
    private val longitudePattern = Regex("Longitude:\\s*([-+]?\\d+(?:\\.\\d+)?)")
    private val accuracyPattern = Regex("Accuracy:\\s*([-+]?\\d+(?:\\.\\d+)?)\\s*m")
    private val altitudePattern = Regex("Altitude:\\s*([-+]?\\d+(?:\\.\\d+)?)\\s*m")
    private val speedPattern = Regex("Speed:\\s*([-+]?\\d+(?:\\.\\d+)?)\\s*m/s")
    private val headingPattern = Regex("Heading:\\s*([-+]?\\d+(?:\\.\\d+)?)\\s*deg")
    private val hdopPattern = Regex("HDOP:\\s*([-+]?\\d+(?:\\.\\d+)?)")

    private val _connectionState = MutableStateFlow("DISCONNECTED")
    val connectionState: StateFlow<String> = _connectionState.asStateFlow()

    private var currentSessionId: Long? = null
    private var autoConnectInProgress = false
    private var lastAcceptedPoint: AcceptedPoint? = null
    private var trackGapPending = false

    fun pairedDevices(): List<ClassicBluetoothDeviceUi> = bluetooth.pairedDevices()

    fun scanBle(onComplete: (List<ClassicBluetoothDeviceUi>) -> Unit) = ble.scan(onComplete)

    fun autoConnectStoredBle(onScanComplete: (List<ClassicBluetoothDeviceUi>) -> Unit = {}) {
        val address = connectionPrefs.getString(KEY_BLE_ADDRESS, null)
        if (address == null) {
            android.util.Log.d("GnssRepository", "Auto BLE reconnect skipped: no saved device")
            return
        }
        val state = _connectionState.value
        val connectionActive = state.startsWith("CONNECTED BLE") ||
            state.startsWith("CONNECTING BLE") ||
            state.startsWith("RECONNECTING BLE")
        if (connectionActive) {
            android.util.Log.d("GnssRepository", "Auto BLE reconnect skipped: state=$state")
            return
        }
        if (autoConnectInProgress) {
            android.util.Log.d("GnssRepository", "Auto BLE reconnect skipped: scan already running")
            return
        }

        autoConnectInProgress = true
        appendDebug("Looking for saved BLE device ${connectionPrefs.getString(KEY_BLE_NAME, address)}")
        android.util.Log.d("GnssRepository", "Auto BLE scan started for $address")
        ble.scan(address) { devices ->
            try {
                onScanComplete(devices)
                val saved = devices.firstOrNull { it.transport == Transport.BLE && it.address.equals(address, true) }
                if (saved != null) {
                    appendDebug("Saved BLE device found, reconnecting")
                    android.util.Log.d("GnssRepository", "Auto BLE device found: ${saved.address}")
                    connect(saved)
                } else {
                    appendDebug("Saved BLE device not found")
                    android.util.Log.d("GnssRepository", "Auto BLE device not found; scan results=${devices.size}")
                }
            } finally {
                autoConnectInProgress = false
            }
        }
    }

    fun connect(device: ClassicBluetoothDeviceUi) {
        scope.launch {
            currentSessionId = db.gnssDao().insertSession(
                SessionEntity(
                    startedAt = System.currentTimeMillis(),
                    deviceAddress = device.address,
                )
            )
        }
        appendDebug("Connecting to ${device.name} (${device.address}) over ${device.transport}")
        if (device.transport == Transport.BLE) {
            connectionPrefs.edit()
                .putString(KEY_BLE_ADDRESS, device.address)
                .putString(KEY_BLE_NAME, device.name)
                .apply()
            ble.connect(device.device) { bytes -> handleBleBytes(bytes) }
            scope.launch {
                ble.connectionState.collect {
                    val deviceLabel = "${device.name} (${device.address})"
                    // Keep the target device attached to every BLE state,
                    // including errors, so the UI never leaves ambiguity
                    // about which peripheral is affected.
                    val displayState = "$it [$deviceLabel]"
                    _connectionState.value = displayState
                    appendDebug("BLE: $displayState")
                }
            }
        } else {
            bluetooth.connect(device.device) { line -> handleLine(line) }
            scope.launch { bluetooth.connectionState.collect { _connectionState.value = it } }
        }
    }

    fun disconnect() {
        bluetooth.disconnect()
        ble.disconnect()
        val sessionId = currentSessionId ?: return
        scope.launch {
            db.gnssDao().endSession(sessionId, System.currentTimeMillis())
        }
        currentSessionId = null
    }

    fun sendCommand(command: String) {
        appendDebug("> $command")
        if (connectionState.value.contains("BLE")) ble.sendCommand(command) else bluetooth.sendCommand(command)
    }

    private fun handleLine(line: String) {
        val compact = line.trim()
        if (compact.isEmpty()) return
        appendDebug(compact)

        val sessionId = currentSessionId
        if (sessionId != null) {
            scope.launch {
                db.gnssDao().insertDebugEvent(
                    DebugEventEntity(
                        sessionId = sessionId,
                        tsMs = System.currentTimeMillis(),
                        payload = compact,
                    )
                )
            }
        }

        runCatching { json.decodeFromString(GnssFrame.serializer(), compact) }
            .onSuccess { frame ->
                _latestFrame.value = frame
                val accepted = if (frame.fix_valid && frame.lat != null && frame.lon != null) {
                    rememberValidFrame(frame)
                } else {
                    trackGapPending = true
                    false
                }
                if (sessionId != null && accepted && frame.lat != null && frame.lon != null) {
                    scope.launch {
                        db.gnssDao().insertTrackPoint(
                            TrackPointEntity(
                                sessionId = sessionId,
                                tsMs = frame.ts_ms,
                                lat = frame.lat,
                                lon = frame.lon,
                                altM = frame.alt_m,
                                accuracyM = frame.accuracy_m,
                            )
                        )
                    }
                }
            }
    }

    private fun handleBleBytes(bytes: ByteArray) {
        // A BLE read with no payload is not a GNSS frame. Ignore it before
        // updating the display or invoking any GNSS parsers.
        if (bytes.isEmpty()) return
        val isText = bytes.all { it == '\n'.code.toByte() || it == '\r'.code.toByte() ||
            it == '\t'.code.toByte() || (it.toInt() in 0x20..0x7e) }
        if (isText) {
            val text = bytes.toString(Charsets.UTF_8)
            if (text.isBlank()) return
            updateLiveDisplay(text)
            parseRawGnssDisplay(_liveDisplay.value)
            parseSatellites(_liveDisplay.value)
            handleLine(text)
        } else {
            val text = bytes.map { byte ->
                val value = byte.toInt() and 0xFF
                if (value == '\n'.code || value == '\r'.code || value == '\t'.code || value in 0x20..0x7E) {
                    value.toChar()
                } else {
                    '\uFFFD'
                }
            }.joinToString("")
            val hex = bytes.joinToString(" ") { "%02X".format(it.toInt() and 0xFF) }
            updateLiveDisplay("BLE RX TEXT: $text\nBLE RX HEX: $hex")
            appendDebug("BLE RX TEXT: $text")
            appendDebug("BLE RX HEX: $hex")
            val sessionId = currentSessionId
            if (sessionId != null) {
                scope.launch {
                    db.gnssDao().insertDebugEvent(
                        DebugEventEntity(
                            sessionId = sessionId,
                            tsMs = System.currentTimeMillis(),
                            payload = "BLE RX TEXT: $text\nBLE RX HEX: $hex",
                        )
                    )
                }
            }
        }
    }

    private fun parseRawGnssDisplay(text: String) {
        val tracking = trackingPattern.find(text) ?: return
        val lat = latitudePattern.find(text)?.groupValues?.get(1)?.toDoubleOrNull()
        val lon = longitudePattern.find(text)?.groupValues?.get(1)?.toDoubleOrNull()
        val frame = GnssFrame(
            ts_ms = System.currentTimeMillis(),
            fix_valid = lat != null && lon != null,
            lat = lat,
            lon = lon,
            alt_m = altitudePattern.find(text)?.groupValues?.get(1)?.toDoubleOrNull(),
            speed_mps = speedPattern.find(text)?.groupValues?.get(1)?.toDoubleOrNull(),
            heading_deg = headingPattern.find(text)?.groupValues?.get(1)?.toDoubleOrNull(),
            accuracy_m = accuracyPattern.find(text)?.groupValues?.get(1)?.toDoubleOrNull(),
            hdop = hdopPattern.find(text)?.groupValues?.get(1)?.toDoubleOrNull(),
            tracked = tracking.groupValues[1].toInt(),
            used = tracking.groupValues[2].toInt(),
            unhealthy = tracking.groupValues[3].toInt(),
        )
        _latestFrame.value = frame
        if (frame.fix_valid && frame.lat != null && frame.lon != null) {
            rememberValidFrame(frame)
        } else {
            trackGapPending = true
        }
    }

    private fun parseSatellites(text: String) {
        val usedPrns = text.lineSequence()
            .filter { it.startsWith("\$GPGSA,") || it.startsWith("\$GNGSA,") }
            .flatMap { line ->
                line.substringBefore('*').split(',').drop(3).take(12).asSequence()
            }
            .mapNotNull { it.toIntOrNull() }
            .toSet()

        val parsed = mutableListOf<GnssSatellite>()
        text.lineSequence()
            .filter { it.startsWith("\$GPGSV,") || it.startsWith("\$GLGSV,") ||
                it.startsWith("\$GAGSV,") || it.startsWith("\$GBGSV,") || it.startsWith("\$BDGSV,") }
            .forEach { line ->
                val fields = line.substringBefore('*').split(',')
                val talker = fields.firstOrNull()?.removePrefix("$") ?: return@forEach
                val system = when (talker.take(2)) {
                    "GP", "GN" -> "GPS"
                    "GL" -> "GLONASS"
                    "GA" -> "Galileo"
                    "GB", "BD" -> "BeiDou"
                    else -> talker.take(2)
                }
                var index = 4
                while (index + 3 < fields.size) {
                    val prn = fields[index].toIntOrNull()
                    if (prn != null) {
                        val name = "$system-${prn.toString().padStart(3, '0')}"
                        parsed += GnssSatellite(
                            name = name,
                            system = system,
                            prn = prn,
                            elevationDeg = fields[index + 1].toIntOrNull(),
                            azimuthDeg = fields[index + 2].toIntOrNull(),
                            cn0DbHz = fields[index + 3].toIntOrNull(),
                            used = prn in usedPrns,
                        )
                    }
                    index += 4
                }
            }
        if (parsed.isNotEmpty()) {
            _satellites.value = parsed.distinctBy { "${it.system}:${it.prn}" }
                .sortedWith(compareByDescending<GnssSatellite> { it.used }.thenBy { it.name })
        }
    }

    private fun rememberValidFrame(frame: GnssFrame): Boolean {
        val lat = frame.lat ?: return false
        val lon = frame.lon ?: return false
        if (!lat.isFinite() || !lon.isFinite() || lat !in -90.0..90.0 || lon !in -180.0..180.0) {
            trackGapPending = true
            return false
        }
        if (frame.accuracy_m != null &&
            (!frame.accuracy_m.isFinite() || frame.accuracy_m < 0.0 || frame.accuracy_m > MAX_ACCEPTED_ACCURACY_M)
        ) {
            trackGapPending = true
            return false
        }

        val timestampMs = if (frame.ts_ms > 0L) frame.ts_ms else System.currentTimeMillis()
        val previous = lastAcceptedPoint
        if (previous != null) {
            val elapsedMs = timestampMs - previous.timestampMs
            val distanceM = distanceMeters(previous.lat, previous.lon, lat, lon)
            val elapsedSeconds = elapsedMs / 1000.0
            val allowedDistanceM = if (elapsedMs <= 0L || elapsedMs > MAX_TRACK_GAP_MS) {
                0.0
            } else {
                max(MIN_TRACK_JUMP_M, MAX_TRACK_SPEED_MPS * elapsedSeconds)
            }
            if (elapsedMs <= 0L || elapsedMs > MAX_TRACK_GAP_MS || distanceM > allowedDistanceM) {
                // Keep the fix for the marker, but do not connect it to the
                // previous segment. A later good fix starts a fresh segment.
                trackGapPending = true
                lastAcceptedPoint = null
                return false
            }
        }

        val hadGap = trackGapPending
        _lastValidFrame.value = frame
        val point = lat to lon
        val segments = _trackSegments.value.toMutableList()
        if (trackGapPending || segments.isEmpty()) {
            segments += listOf(point)
        } else {
            val current = segments.last().toMutableList()
            if (current.lastOrNull() != point) {
                current += point
                segments[segments.lastIndex] = current.takeLast(MAX_POINTS_PER_SEGMENT)
            }
        }
        _trackSegments.value = segments.takeLast(MAX_TRACK_SEGMENTS)
        lastAcceptedPoint = AcceptedPoint(timestampMs, lat, lon)
        trackGapPending = false
        scope.launch {
            val date = Instant.ofEpochMilli(System.currentTimeMillis())
                .atZone(ZoneId.systemDefault()).toLocalDate()
            val file = File(positionLogDirectory, "positions-$date.jsonl")
            if (hadGap) appendCapped(file, "{\"type\":\"gap\"}\n")
            appendCapped(file, json.encodeToString(GnssFrame.serializer(), frame) + "\n")
            prunePositionFiles()
        }
        return true
    }

    private data class AcceptedPoint(val timestampMs: Long, val lat: Double, val lon: Double)

    private fun distanceMeters(lat1: Double, lon1: Double, lat2: Double, lon2: Double): Double {
        val earthRadiusM = 6_371_000.0
        val dLat = Math.toRadians(lat2 - lat1)
        val dLon = Math.toRadians(lon2 - lon1)
        val a = kotlin.math.sin(dLat / 2) * kotlin.math.sin(dLat / 2) +
            kotlin.math.cos(Math.toRadians(lat1)) * kotlin.math.cos(Math.toRadians(lat2)) *
            kotlin.math.sin(dLon / 2) * kotlin.math.sin(dLon / 2)
        return 2 * earthRadiusM * kotlin.math.asin(kotlin.math.sqrt(a.coerceIn(0.0, 1.0)))
    }

    private fun updateLiveDisplay(incoming: String) {
        synchronized(liveDisplayBuffer) {
            // Firmware status output starts a new screen at Tracking:. Keep the
            // current page until the next refresh, then replace it in place.
            val hasCurrentPage = liveDisplayBuffer.contains("Tracking:")
            val startsNewPage = incoming.contains("Tracking:") && hasCurrentPage
            if (startsNewPage || (!hasCurrentPage && !incoming.contains("Tracking:"))) {
                liveDisplayBuffer.clear()
            }

            liveDisplayBuffer.append(incoming)

            if (liveDisplayBuffer.length > 12000) {
                liveDisplayBuffer.delete(0, liveDisplayBuffer.length - 12000)
            }
            _liveDisplay.value = liveDisplayBuffer.toString().replace("\r", "")
        }
    }

    private fun appendDebug(line: String) {
        _debugLines.value = (_debugLines.value + line).takeLast(300)
        scope.launch {
            appendCapped(debugLogFile, "${System.currentTimeMillis()} $line\n")
        }
    }

    private fun appendCapped(file: File, text: String) {
        val bytes = text.toByteArray(Charsets.UTF_8)
        synchronized(logFileLock) {
            file.parentFile?.mkdirs()
            if (bytes.size >= MAX_LOG_FILE_BYTES) {
                file.outputStream().use { output ->
                    output.write(bytes, bytes.size - MAX_LOG_FILE_BYTES, MAX_LOG_FILE_BYTES)
                }
                return
            }

            val existingLength = file.length()
            val overflow = existingLength + bytes.size - MAX_LOG_FILE_BYTES
            if (overflow > 0 && file.exists()) {
                val temp = File(file.parentFile, "${file.name}.tmp")
                FileInputStream(file).use { input ->
                    var remaining = overflow
                    while (remaining > 0) {
                        val skipped = input.skip(remaining)
                        if (skipped <= 0) break
                        remaining -= skipped
                    }
                    temp.outputStream().use { output -> input.copyTo(output) }
                }
                file.delete()
                temp.renameTo(file)
            }
            file.appendBytes(bytes)
        }
    }

    private fun prunePositionFiles() {
        val files = positionLogDirectory.listFiles { file ->
            file.isFile && file.name.startsWith("positions-") && file.name.endsWith(".jsonl")
        }?.sortedByDescending { it.name } ?: return
        files.drop(MAX_POSITION_FILES).forEach { it.delete() }
    }

    fun sessions() = db.gnssDao().observeSessions()

    suspend fun exportSession(sessionId: Long): Pair<String, String> {
        val points = db.gnssDao().getTrackPoints(sessionId)
        val events = db.gnssDao().getDebugEvents(sessionId)

        val jsonPayload = buildString {
            append("{\"sessionId\":$sessionId,\"trackPoints\":[")
            append(points.joinToString(",") { "{\"tsMs\":${it.tsMs},\"lat\":${it.lat},\"lon\":${it.lon}}" })
            append("],\"debugEvents\":[")
            append(events.joinToString(",") { "{\"tsMs\":${it.tsMs},\"payload\":\"${it.payload.replace("\"", "'")}\"}" })
            append("]}")
        }

        val csv = buildString {
            appendLine("ts_ms,lat,lon,alt_m,accuracy_m")
            points.forEach { appendLine("${it.tsMs},${it.lat},${it.lon},${it.altM ?: ""},${it.accuracyM ?: ""}") }
        }

        return jsonPayload to csv
    }

    private companion object {
        const val KEY_BLE_ADDRESS = "saved_ble_address"
        const val KEY_BLE_NAME = "saved_ble_name"
        const val MAX_ACCEPTED_ACCURACY_M = 250.0
        const val MAX_TRACK_SPEED_MPS = 80.0
        const val MIN_TRACK_JUMP_M = 1_000.0
        const val MAX_TRACK_GAP_MS = 120_000L
        const val MAX_POINTS_PER_SEGMENT = 1000
        const val MAX_TRACK_SEGMENTS = 20
        const val MAX_LOG_FILE_BYTES = 20 * 1024 * 1024
        const val MAX_POSITION_FILES = 10
    }
}

data class StoredPositionFile(val name: String, val sizeBytes: Long)
