package com.example.gnssble

import android.os.Bundle
import android.os.Build
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import com.example.gnssble.ble.GnssForegroundService
import com.example.gnssble.data.repo.GnssRepository
import com.example.gnssble.ui.navigation.GnssNavHost
import dagger.hilt.android.AndroidEntryPoint
import javax.inject.Inject

@AndroidEntryPoint
class MainActivity : ComponentActivity() {
    @Inject
    lateinit var repository: GnssRepository

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent {
            MaterialTheme {
                Surface {
                    GnssNavHost()
                }
            }
        }
    }

    override fun onStart() {
        super.onStart()
        // Start the session owner on every app launch. The service performs the
        // saved-device scan even when the user opens another tab first.
        GnssForegroundService.start(this)
    }

    override fun onStop() {
        super.onStop()
        // Keep the foreground service alive while the UI is backgrounded. This
        // prevents the BLE module from being left in a stale connected state.
    }
}
