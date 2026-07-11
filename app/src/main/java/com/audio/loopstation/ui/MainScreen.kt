package com.audio.loopstation.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.audio.loopstation.AudioEngine
import kotlinx.coroutines.launch

/**
 * Root screen: the multi-track grid scrolls in a LazyColumn while the
 * transport bar stays fixed in the Scaffold's bottomBar. Everything renders
 * from the ViewModel's StateFlows; every gesture calls back into it. The
 * layout is a single adaptive column, comfortable from a Tab S9 Ultra down
 * to a small phone (rows stretch, touch targets stay fixed-size).
 */
@Composable
fun MainScreen(viewModel: MainViewModel) {
    val transport by viewModel.transport.collectAsStateWithLifecycle()
    val tracks by viewModel.tracks.collectAsStateWithLifecycle()
    // The waveform and playhead flows are deliberately NOT collected here:
    // they change ~60 times a second, and collecting them in composition
    // would recompose the whole screen every frame. They are handed down as
    // flows and only read in the draw phase (WaveformVisualizer /
    // PlayheadProgressLine).

    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val snackbar = remember { SnackbarHostState() }

    // Device disconnects arrive from the engine's error thread; warn the user.
    LaunchedEffect(viewModel) {
        viewModel.engineEvents.collect { event ->
            val message = when (event) {
                AudioEngine.EngineEvent.INPUT_DISCONNECTED ->
                    "Input device disconnected — transport paused, take saved"
                AudioEngine.EngineEvent.OUTPUT_DISCONNECTED ->
                    "Output device disconnected — transport paused"
                AudioEngine.EngineEvent.RESTART_SUCCEEDED ->
                    "Audio restarted on the current default device"
                AudioEngine.EngineEvent.RESTART_FAILED ->
                    "Could not restart audio — check your device"
                AudioEngine.EngineEvent.UNKNOWN -> return@collect
            }
            snackbar.showSnackbar(message)
        }
    }

    Scaffold(
        snackbarHost = { SnackbarHost(snackbar) },
        bottomBar = {
            TransportBar(
                transport = transport,
                position = viewModel.position,
                onRecordTap = viewModel::onRecordTap,
                onPlayTap = viewModel::onPlayTap,
                onStopTap = viewModel::onStopTap,
                onMetronomeToggle = viewModel::onMetronomeToggle,
                onBpmChange = viewModel::onBpmChange,
                onExportTap = {
                    scope.launch {
                        val zip = viewModel.exportSession()
                        if (zip != null) {
                            shareSessionZip(context, zip)
                        } else {
                            snackbar.showSnackbar("Export failed — stop recording and try again")
                        }
                    }
                },
            )
        },
    ) { innerPadding ->
        LazyColumn(
            modifier = Modifier
                .fillMaxSize()
                .padding(innerPadding),
            contentPadding = androidx.compose.foundation.layout.PaddingValues(12.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            items(tracks, key = { it.index }) { track ->
                TrackRow(
                    track = track,
                    // Live rolling input waveform rides on the selected row;
                    // the others get the inert resting line.
                    waveform = if (track.isSelected) viewModel.waveform else null,
                    onSelect = { viewModel.onTrackSelect(track.index) },
                    onMuteToggle = { viewModel.onMuteToggle(track.index) },
                    onSoloToggle = { viewModel.onSoloToggle(track.index) },
                    onVolumeChange = { viewModel.onVolumeChange(track.index, it) },
                    onPanChange = { viewModel.onPanChange(track.index, it) },
                )
            }
        }
    }
}
