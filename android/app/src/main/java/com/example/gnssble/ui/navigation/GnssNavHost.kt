package com.example.gnssble.ui.navigation

import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.ui.Modifier
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.currentBackStackEntryAsState
import androidx.navigation.compose.rememberNavController
import com.example.gnssble.ui.screens.DashboardScreen
import com.example.gnssble.ui.screens.DebugScreen
import com.example.gnssble.ui.screens.EarthGlobeScreen
import com.example.gnssble.ui.screens.HistoryScreen
import com.example.gnssble.ui.screens.MapScreen
import com.example.gnssble.ui.screens.ScannerScreen
import com.example.gnssble.ui.screens.SkyViewScreen
import com.example.gnssble.ui.viewmodel.GnssViewModel

private val tabs = listOf("scanner", "dashboard", "map", "sky", "earth", "debug", "history")

@Composable
fun GnssNavHost(viewModel: GnssViewModel = hiltViewModel()) {
    val navController = rememberNavController()
    val nav by navController.currentBackStackEntryAsState()
    val route = nav?.destination?.route ?: tabs.first()

    Scaffold(
        bottomBar = {
            NavigationBar {
                tabs.forEach { tab ->
                    NavigationBarItem(
                        selected = route == tab,
                        onClick = { navController.navigate(tab) },
                        label = { Text(tab.replaceFirstChar { it.uppercase() }) },
                        icon = {},
                    )
                }
            }
        }
    ) { padding ->
        NavHost(
            navController = navController,
            startDestination = "scanner",
            modifier = Modifier.fillMaxSize(),
        ) {
            composable("scanner") { ScannerScreen(viewModel, padding) }
            composable("dashboard") { DashboardScreen(viewModel, padding) }
            composable("map") { MapScreen(viewModel, padding) }
            composable("sky") { SkyViewScreen(viewModel, padding) }
            composable("earth") { EarthGlobeScreen(viewModel, padding) }
            composable("debug") { DebugScreen(viewModel, padding) }
            composable("history") { HistoryScreen(viewModel, padding) }
        }
    }
}
