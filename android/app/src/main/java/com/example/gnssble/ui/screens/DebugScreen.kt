package com.example.gnssble.ui.screens

import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.animation.Crossfade
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.example.gnssble.ui.viewmodel.GnssViewModel
import kotlinx.coroutines.delay

private val MatrixBlack = Color(0xFF020805)
private val MatrixPanel = Color(0xFF06130B)
private val MatrixGreen = Color(0xFF55FF88)
private val MatrixDimGreen = Color(0xFF2A9E57)

@Composable
fun DebugScreen(viewModel: GnssViewModel, padding: PaddingValues) {
    val liveDisplay by viewModel.liveDisplay.collectAsState()
    var renderedDisplay by remember { mutableStateOf(liveDisplay) }

    LaunchedEffect(Unit) {
        // BLE notifications may arrive continuously, so a debounce could be
        // cancelled forever. Refresh the instrument display at a steady rate.
        while (true) {
            delay(1000)
            renderedDisplay = viewModel.liveDisplay.value
        }
    }
    val transition = rememberInfiniteTransition(label = "matrix-cursor")
    val cursorAlpha by transition.animateFloat(
        initialValue = 0.15f,
        targetValue = 1f,
        animationSpec = infiniteRepeatable(
            animation = tween(650, easing = LinearEasing),
            repeatMode = RepeatMode.Reverse,
        ),
        label = "cursor-alpha",
    )

    Box(
        modifier = Modifier
            .fillMaxSize()
            .padding(padding)
            .background(
                Brush.verticalGradient(
                    listOf(Color(0xFF020A05), MatrixBlack, Color(0xFF001A0A)),
                ),
            )
            .padding(horizontal = 14.dp, vertical = 12.dp),
    ) {
        Canvas(modifier = Modifier.matchParentSize()) {
            val lineColor = Color(0x143CFF70)
            var y = 0f
            while (y < size.height) {
                drawLine(
                    lineColor,
                    androidx.compose.ui.geometry.Offset(0f, y),
                    androidx.compose.ui.geometry.Offset(size.width, y),
                    1f,
                )
                y += 5.dp.toPx()
            }
        }

        Column(modifier = Modifier.fillMaxSize()) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
            ) {
                Column {
                    Text(
                        text = "// GNSS_SERIAL_TERMINAL",
                        color = MatrixGreen,
                        fontFamily = FontFamily.Monospace,
                        fontSize = 16.sp,
                        fontWeight = FontWeight.Bold,
                        letterSpacing = 1.sp,
                    )
                    Text(
                        text = "LIVE DISPLAY  ::  RAW BLE STREAM",
                        color = MatrixDimGreen,
                        fontFamily = FontFamily.Monospace,
                        fontSize = 9.sp,
                        letterSpacing = 1.2.sp,
                    )
                }
                Text(
                    text = "● ONLINE",
                    color = MatrixGreen.copy(alpha = cursorAlpha),
                    fontFamily = FontFamily.Monospace,
                    fontSize = 10.sp,
                    fontWeight = FontWeight.Bold,
                )
            }

            Spacer(modifier = Modifier.height(12.dp))

            Box(
                modifier = Modifier
                    .fillMaxSize()
                    .clip(RoundedCornerShape(4.dp))
                    .background(MatrixPanel.copy(alpha = 0.94f))
                    .border(1.dp, MatrixDimGreen.copy(alpha = 0.7f), RoundedCornerShape(4.dp))
                    .padding(12.dp),
            ) {
                Crossfade(
                    targetState = renderedDisplay.ifEmpty { "WAITING FOR BLE SERIAL DATA_" },
                    animationSpec = tween(durationMillis = 220),
                    label = "display-refresh",
                ) { displayText ->
                    Text(
                        text = displayText,
                        modifier = Modifier.fillMaxSize(),
                        color = MatrixGreen,
                        fontFamily = FontFamily.Monospace,
                        fontSize = 10.sp,
                        lineHeight = 12.sp,
                        softWrap = true,
                        overflow = TextOverflow.Clip,
                    )
                }
                Text(
                    text = "_",
                    modifier = Modifier.align(Alignment.BottomStart),
                    color = MatrixGreen.copy(alpha = cursorAlpha),
                    fontFamily = FontFamily.Monospace,
                    fontSize = 12.sp,
                    fontWeight = FontWeight.Bold,
                )
            }
        }
    }
}
