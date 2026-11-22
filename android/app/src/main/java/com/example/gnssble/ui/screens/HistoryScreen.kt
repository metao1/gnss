package com.example.gnssble.ui.screens

import android.content.Intent
import androidx.core.content.FileProvider
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import com.example.gnssble.ui.viewmodel.GnssViewModel
import kotlinx.coroutines.launch

@Composable
fun HistoryScreen(viewModel: GnssViewModel, padding: PaddingValues) {
    val sessions by viewModel.sessions.collectAsState()
    val scope = rememberCoroutineScope()
    val context = LocalContext.current
    val storedFiles by viewModel.storedPositionFiles.collectAsState()

    LaunchedEffect(Unit) {
        viewModel.refreshStoredPositionFiles()
    }

    Column(modifier = Modifier.padding(padding)) {
        LazyColumn(modifier = Modifier.weight(1f)) {
            item { Text("Session History") }
            items(sessions, key = { it.id }) { session ->
                Card {
                    Column {
                        Text("Session ${session.id}")
                        Text("Start: ${session.startedAt}")
                        Text("End: ${session.endedAt ?: "active"}")
                        Button(onClick = {
                            scope.launch {
                                val (json, csv) = viewModel.export(session.id)
                                val text = "JSON:\\n$json\\n\\nCSV:\\n$csv"
                                val intent = Intent(Intent.ACTION_SEND).apply {
                                    type = "text/plain"
                                    putExtra(Intent.EXTRA_SUBJECT, "GNSS Session ${session.id}")
                                    putExtra(Intent.EXTRA_TEXT, text)
                                }
                                context.startActivity(Intent.createChooser(intent, "Export session"))
                            }
                        }) {
                            Text("Export")
                        }
                    }
                }
            }
            item {
                Spacer(Modifier.height(16.dp))
                Text("Saved Daily Routes")
                if (storedFiles.isEmpty()) {
                    Text("No saved position files yet.")
                }
            }
            items(storedFiles, key = { it.name }) { file ->
                Card(modifier = Modifier.fillMaxWidth()) {
                    Column(modifier = Modifier.padding(12.dp)) {
                        Text(file.name)
                        Text("${file.sizeBytes / 1024} KB")
                        Row {
                            Button(onClick = { viewModel.loadStoredRoute(file) }) {
                                Text("Show Route")
                            }
                            Button(onClick = {
                                viewModel.positionFile(file)?.let { localFile ->
                                    val uri = FileProvider.getUriForFile(
                                        context,
                                        "${context.packageName}.fileprovider",
                                        localFile,
                                    )
                                    val intent = Intent(Intent.ACTION_SEND).apply {
                                        type = "application/json"
                                        putExtra(Intent.EXTRA_STREAM, uri)
                                        addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                                    }
                                    context.startActivity(Intent.createChooser(intent, "Share route file"))
                                }
                            }) {
                                Text("Share")
                            }
                        }
                    }
                }
            }
        }
    }
}
