package com.example.gnssble.ui.screens

import android.content.Context
import android.view.MotionEvent
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Text
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.FilledTonalButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.MutableState
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import com.example.gnssble.ui.viewmodel.GnssViewModel
import com.example.gnssble.BuildConfig
import org.osmdroid.config.Configuration
import org.osmdroid.tileprovider.tilesource.OnlineTileSourceBase
import org.osmdroid.util.MapTileIndex
import org.osmdroid.util.GeoPoint
import org.osmdroid.views.MapView
import org.osmdroid.events.MapListener
import org.osmdroid.events.ScrollEvent
import org.osmdroid.events.ZoomEvent
import org.osmdroid.views.overlay.Marker
import org.osmdroid.views.overlay.Polyline

private enum class MapStyle(val label: String) {
    STREETS("Streets"),
    SATELLITE("Satellite"),
    OUTDOOR("Outdoor / Topographic"),
}

@Composable
fun MapScreen(viewModel: GnssViewModel, padding: PaddingValues) {
    val frame by viewModel.latestFrame.collectAsState()
    val lastValidFrame by viewModel.lastValidFrame.collectAsState()
    val trackSegments by viewModel.trackSegments.collectAsState()
    val displayFrame = frame?.takeIf {
        it.fix_valid && it.lat != null && it.lon != null
    } ?: lastValidFrame
    val point = displayFrame?.let { current ->
        current.lat?.let { lat -> current.lon?.let { lon -> GeoPoint(lat, lon) } }
    }
    val context = LocalContext.current
    var mapStyle by remember { mutableStateOf(MapStyle.STREETS) }

    if (point == null) {
        Box(
            modifier = Modifier.fillMaxSize().padding(padding),
            contentAlignment = Alignment.Center,
        ) {
            Text("Waiting for valid fix")
        }
        return
    }

    val mapRef = remember { mutableStateOf<MapView?>(null) }
    val markerRef = remember { mutableStateOf<Marker?>(null) }
    val pathRef = remember { mutableStateOf<List<Polyline>>(emptyList()) }
    val followLocationRef = remember { mutableStateOf(true) }
    val suppressCameraEventsUntilRef = remember { mutableStateOf(0L) }

    DisposableEffect(Unit) {
        onDispose {
            mapRef.value?.onPause()
            mapRef.value?.onDetach()
            mapRef.value = null
            markerRef.value = null
            pathRef.value = emptyList()
        }
    }

    Box(modifier = Modifier.fillMaxSize().padding(padding)) {
        AndroidView(
            modifier = Modifier.fillMaxSize(),
            factory = {
                createMap(
                    context,
                    point,
                    trackSegments,
                    mapRef,
                    markerRef,
                    pathRef,
                    followLocationRef,
                    suppressCameraEventsUntilRef,
                )
            },
            update = { map ->
                if (map.tag != mapStyle) {
                    map.setTileSource(MapTilerTileSource(mapStyle))
                    map.tag = mapStyle
                }
                val marker = markerRef.value
                marker?.position = point
                marker?.setAnchor(Marker.ANCHOR_CENTER, Marker.ANCHOR_BOTTOM)
                if (pathRef.value.size != trackSegments.size) {
                    pathRef.value.forEach { map.overlays.remove(it) }
                    val paths = trackSegments.map { segment -> createPath(map, segment) }
                    paths.forEach { map.overlays.add(it) }
                    pathRef.value = paths
                } else {
                    pathRef.value.zip(trackSegments).forEach { (path, segment) ->
                        path.setPoints(segment.map { (lat, lon) -> GeoPoint(lat, lon) })
                    }
                }
                if (followLocationRef.value) {
                    suppressCameraEventsUntilRef.value = android.os.SystemClock.uptimeMillis() + 800L
                    map.controller.animateTo(point)
                }
                map.invalidate()
            },
        )

        Box(modifier = Modifier.align(Alignment.TopStart).padding(12.dp)) {
            var menuOpen by remember { mutableStateOf(false) }
            androidx.compose.material3.OutlinedButton(onClick = { menuOpen = true }) {
                Text(mapStyle.label)
            }
            DropdownMenu(expanded = menuOpen, onDismissRequest = { menuOpen = false }) {
                MapStyle.values().forEach { style ->
                    DropdownMenuItem(
                        text = { Text(style.label) },
                        onClick = {
                            mapStyle = style
                            menuOpen = false
                        },
                    )
                }
            }
        }

        FilledTonalButton(
            modifier = Modifier.align(Alignment.BottomStart).padding(12.dp),
            onClick = {
                followLocationRef.value = true
                mapRef.value?.let { map ->
                    suppressCameraEventsUntilRef.value = android.os.SystemClock.uptimeMillis() + 800L
                    map.controller.animateTo(point)
                }
            },
        ) {
            Text("Re-center")
        }

        Text(
            text = "© OpenStreetMap contributors",
            modifier = Modifier
                .align(Alignment.BottomEnd)
                .padding(8.dp)
                .background(Color.White.copy(alpha = 0.82f))
                .padding(horizontal = 6.dp, vertical = 3.dp),
            color = Color.DarkGray,
            fontSize = 10.sp,
        )
    }
}

private fun createMap(
    context: Context,
    point: GeoPoint,
    trackSegments: List<List<Pair<Double, Double>>>,
    mapRef: MutableState<MapView?>,
    markerRef: MutableState<Marker?>,
    pathRef: MutableState<List<Polyline>>,
    followLocationRef: MutableState<Boolean>,
    suppressCameraEventsUntilRef: MutableState<Long>,
): MapView {
    // Identify the app clearly for the public OSM tile service and keep tile
    // requests modest so normal interactive use remains cache-friendly.
    Configuration.getInstance().userAgentValue = "GNSSBLE/1.0 (Android; OpenStreetMap client)"
    Configuration.getInstance().load(
        context,
        context.getSharedPreferences("osmdroid", Context.MODE_PRIVATE),
    )
    Configuration.getInstance().tileDownloadThreads = 2
    Configuration.getInstance().tileDownloadMaxQueueSize = 8

    return MapView(context).apply {
        setTileSource(MapTilerTileSource(MapStyle.STREETS))
        tag = MapStyle.STREETS
        setMultiTouchControls(true)
        minZoomLevel = 2.0
        maxZoomLevel = 20.0
        controller.setZoom(16.0)
        controller.setCenter(point)
        // Stop auto-follow as soon as the user starts a gesture. The map
        // listener below covers camera changes, while this touch hook avoids
        // a race where the next GNSS update arrives during a drag.
        setOnTouchListener { _, event ->
            if (event.actionMasked == MotionEvent.ACTION_DOWN) {
                followLocationRef.value = false
            }
            false
        }
        setMapListener(object : MapListener {
            override fun onScroll(event: ScrollEvent?): Boolean {
                if (android.os.SystemClock.uptimeMillis() > suppressCameraEventsUntilRef.value) {
                    followLocationRef.value = false
                }
                return false
            }

            override fun onZoom(event: ZoomEvent?): Boolean {
                if (android.os.SystemClock.uptimeMillis() > suppressCameraEventsUntilRef.value) {
                    followLocationRef.value = false
                }
                return false
            }
        })

        val paths = trackSegments.map { segment -> createPath(this, segment) }
        pathRef.value = paths
        paths.forEach { overlays.add(it) }

        val marker = Marker(this).apply {
            position = point
            title = "Current GNSS position"
            setAnchor(Marker.ANCHOR_CENTER, Marker.ANCHOR_BOTTOM)
        }
        markerRef.value = marker
        overlays.add(marker)
        mapRef.value = this
        onResume()
    }
}

private fun createPath(map: MapView, segment: List<Pair<Double, Double>>): Polyline {
    return Polyline(map).apply {
        outlinePaint.color = android.graphics.Color.rgb(18, 110, 245)
        outlinePaint.strokeWidth = 7f
        setPoints(segment.map { (lat, lon) -> GeoPoint(lat, lon) })
    }
}

private class MapTilerTileSource(private val style: MapStyle) : OnlineTileSourceBase(
    "MapTiler ${style.label}",
    0,
    20,
    256,
    ".png",
    arrayOf("https://api.maptiler.com/maps/streets-v4/256/"),
) {
    override fun getTileURLString(tileIndex: Long): String {
        val z = MapTileIndex.getZoom(tileIndex)
        val x = MapTileIndex.getX(tileIndex)
        val y = MapTileIndex.getY(tileIndex)
        return when (style) {
            MapStyle.SATELLITE -> "https://api.maptiler.com/tiles/satellite-v2/$z/$x/$y.jpg?key=${BuildConfig.MAPTILER_API_KEY}"
            MapStyle.STREETS -> "https://api.maptiler.com/maps/streets-v4/256/$z/$x/$y.png?key=${BuildConfig.MAPTILER_API_KEY}"
            MapStyle.OUTDOOR -> "https://api.maptiler.com/maps/outdoor-v4/256/$z/$x/$y.png?key=${BuildConfig.MAPTILER_API_KEY}"
        }
    }
}
