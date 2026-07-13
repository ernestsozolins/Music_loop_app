package com.audio.loopstation.ui

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.GridItemSpan
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.audio.loopstation.AudioEngine
import kotlinx.coroutines.launch

/**
 * Root screen: the track grid scrolls while the transport bar stays fixed
 * in the Scaffold's bottomBar. Everything renders from the ViewModel's
 * StateFlows; every gesture calls back into it.
 *
 * Adaptive layout: [twoPane] (driven by the window width size class from
 * MainActivity) switches the track list from a single column (phones,
 * split-screen) to a two-column grid that puts a Tab S9 Ultra-class canvas
 * to work; the monitor-FX panel spans the full width in both.
 */
@Composable
fun MainScreen(viewModel: MainViewModel, twoPane: Boolean = false) {
    val transport by viewModel.transport.collectAsStateWithLifecycle()
    val tracks by viewModel.tracks.collectAsStateWithLifecycle()
    val reverb by viewModel.reverb.collectAsStateWithLifecycle()
    val filterState by viewModel.filter.collectAsStateWithLifecycle()
    val droneState by viewModel.drone.collectAsStateWithLifecycle()
    val autoRecordArmed by viewModel.autoRecordArmed.collectAsStateWithLifecycle()
    val manualLatencyMs by viewModel.manualLatencyMs.collectAsStateWithLifecycle()
    val calibration by viewModel.calibration.collectAsStateWithLifecycle()
    val trackWaveforms by viewModel.trackWaveforms.collectAsStateWithLifecycle()
    // The live waveform and playhead flows are deliberately NOT collected
    // here: they change ~60 times a second, and collecting them in
    // composition would recompose the whole screen every frame. They are
    // handed down as flows and only read in the draw phase
    // (WaveformVisualizer / PlayheadProgressLine).

    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val snackbar = remember { SnackbarHostState() }
    var showSessions by remember { mutableStateOf(false) }
    var showOutputs by remember { mutableStateOf(false) }
    var showInputs by remember { mutableStateOf(false) }
    var showTuner by remember { mutableStateOf(false) }
    var trimTarget by remember { mutableStateOf<Int?>(null) }

    trimTarget?.let { index ->
        val track = tracks.firstOrNull { it.index == index }
        if (track != null) {
            TrimEditorDialog(
                trackName = track.name,
                loopMs = viewModel.loopLengthMs(),
                initialStartMs = track.trimStartMs,
                initialEndMs = track.trimEndMs,
                bins = trackWaveforms[index],
                onChange = { s, e -> viewModel.onSetTrackTrim(index, s, e) },
                onAutoTrim = { viewModel.autoTrimAndGet(index) },
                onDismiss = { trimTarget = null },
            )
        } else {
            trimTarget = null
        }
    }

    if (showSessions) {
        val sessions by viewModel.sessions.collectAsStateWithLifecycle()
        SessionBrowserSheet(
            sessions = sessions,
            onLoad = { id ->
                showSessions = false
                scope.launch {
                    if (!viewModel.loadSession(id)) {
                        snackbar.showSnackbar("Couldn't load — stop recording first")
                    }
                }
            },
            onDelete = viewModel::deleteSession,
            onDismiss = { showSessions = false },
        )
    }

    if (showOutputs) {
        val devices by viewModel.outputDevices.collectAsStateWithLifecycle()
        val selectedId by viewModel.selectedOutputId.collectAsStateWithLifecycle()
        DevicePickerSheet(
            title = "Playback device",
            subtitle = "Bluetooth output adds latency — run calibration after switching.",
            devices = devices,
            selectedId = selectedId,
            onSelect = { id ->
                viewModel.onSelectOutputDevice(id)
                showOutputs = false
            },
            onDismiss = { showOutputs = false },
        )
    }

    if (showInputs) {
        val devices by viewModel.inputDevices.collectAsStateWithLifecycle()
        val selectedId by viewModel.selectedInputId.collectAsStateWithLifecycle()
        DevicePickerSheet(
            title = "Recording device",
            subtitle = "Pick your USB interface (e.g. Zoom H2n) or a mic to record from.",
            devices = devices,
            selectedId = selectedId,
            onSelect = { id ->
                viewModel.onSelectInputDevice(id)
                showInputs = false
            },
            onDismiss = { showInputs = false },
        )
    }

    if (showTuner) {
        val tunerState by viewModel.tuner.collectAsStateWithLifecycle()
        TunerSheet(
            tuner = tunerState,
            onDismiss = {
                viewModel.onTunerClose()
                showTuner = false
            },
        )
    }

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

    val onExportTap: () -> Unit = {
        scope.launch {
            val zip = viewModel.exportSession()
            if (zip != null) {
                shareSessionZip(context, zip)
            } else {
                snackbar.showSnackbar("Export failed — stop recording and try again")
            }
        }
    }

    // "Save to…" writes the stems zip to a location the user picks (SAF),
    // rather than only through the share sheet. The picker returns the target
    // document Uri; the export + copy then runs off the main thread.
    val saveToFile = rememberLauncherForActivityResult(
        ActivityResultContracts.CreateDocument("application/zip"),
    ) { uri ->
        if (uri != null) {
            scope.launch {
                val ok = viewModel.exportSessionToUri(uri)
                snackbar.showSnackbar(
                    if (ok) "Saved to your chosen location"
                    else "Save failed — stop recording and try again",
                )
            }
        }
    }
    val onExportToFileTap: () -> Unit = { saveToFile.launch(viewModel.suggestedExportName()) }

    Scaffold(
        // targetSdk 35 forces edge-to-edge, so inset the whole app into the
        // safe area — otherwise the transport bar hides behind the system
        // back/home navigation bar and the top hides under the status bar.
        modifier = Modifier.safeDrawingPadding(),
        contentWindowInsets = WindowInsets(0, 0, 0, 0),  // handled by safeDrawingPadding above
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
                onTapTempo = viewModel::onTapTempo,
                onRhythmStyleToggle = viewModel::onToggleRhythmStyle,
                onExportTap = onExportTap,
                onExportToFileTap = onExportToFileTap,
                onCountInToggle = viewModel::onCountInToggle,
                onQuantizeToggle = viewModel::onQuantizeToggle,
                onSyncToggle = viewModel::onSyncToLoopToggle,
                onSingleModeToggle = viewModel::onToggleSingleMode,
                autoRecordArmed = autoRecordArmed,
                onAutoRecordToggle = viewModel::onToggleAutoRecord,
                onTunerTap = {
                    viewModel.onTunerOpen()
                    showTuner = true
                },
                onTimeSignatureTap = viewModel::onTimeSignatureChange,
                onUndoTap = viewModel::onUndo,
                onClearAllTap = viewModel::onClearAll,
                onSaveTap = {
                    scope.launch {
                        val ok = viewModel.saveSession()
                        snackbar.showSnackbar(if (ok) "Session saved" else "Save failed")
                    }
                },
                onSessionsTap = { showSessions = true },
                onInputTap = {
                    viewModel.refreshInputDevices()
                    showInputs = true
                },
                onOutputTap = {
                    viewModel.refreshOutputDevices()
                    showOutputs = true
                },
            )
        },
    ) { innerPadding ->
        val backingTracks by viewModel.backingTracks.collectAsStateWithLifecycle()

        val trackRow: @Composable (MainViewModel.TrackUiState) -> Unit = { track ->
            TrackRow(
                track = track,
                // Live rolling input waveform rides on the selected row; the
                // others show their recorded loop audio (offline peaks).
                waveform = if (track.isSelected) viewModel.waveform else null,
                offlineWaveform = if (track.isSelected) null else trackWaveforms[track.index],
                onSelect = { viewModel.onTrackSelect(track.index) },
                onRecord = { viewModel.onTrackRecord(track.index) },
                onPlay = { viewModel.onTrackPlay(track.index) },
                onStop = { viewModel.onTrackStop(track.index) },
                onToggleOneShot = { viewModel.onToggleOneShot(track.index) },
                onMuteToggle = { viewModel.onMuteToggle(track.index) },
                onSoloToggle = { viewModel.onSoloToggle(track.index) },
                onVolumeChange = { viewModel.onVolumeChange(track.index, it) },
                onPanChange = { viewModel.onPanChange(track.index, it) },
                onClear = { viewModel.onClearTrack(track.index) },
                onTrim = { trimTarget = track.index },
                onNudge = { delta -> viewModel.onNudgeTrack(track.index, delta) },
                onToggleFade = { viewModel.onToggleTrackFade(track.index) },
            )
        }
        val backingPanel: @Composable () -> Unit = {
            BackingTrackPanel(
                slots = backingTracks,
                onLoad = viewModel::onLoadBackingTrack,
                onTogglePlay = viewModel::onToggleBackingPlay,
                onToggleLoop = viewModel::onToggleBackingLoop,
                onGainChange = viewModel::onBackingGainChange,
                onRemove = viewModel::onRemoveBackingTrack,
            )
        }
        val fxPanel: @Composable () -> Unit = {
            MonitorFxPanel(
                reverb = reverb,
                filter = filterState,
                drone = droneState,
                droneNotes = viewModel.droneNotes,
                calibration = calibration,
                inputLevel = viewModel.inputLevel,
                manualLatencyMs = manualLatencyMs,
                onManualLatencyChange = viewModel::onManualLatencyChange,
                monitorLevel = transport.monitorLevel,
                onMonitorLevelChange = viewModel::onMonitorLevelChange,
                onMixChange = viewModel::onReverbMixChange,
                onRoomSizeChange = viewModel::onReverbRoomSizeChange,
                onFilterToggle = viewModel::onFilterToggle,
                onFilterCutoff = viewModel::onFilterCutoffChange,
                onFilterResonance = viewModel::onFilterResonanceChange,
                onFilterMode = viewModel::onFilterModeChange,
                onDroneToggle = viewModel::onDroneToggle,
                onDroneNote = viewModel::onDroneNote,
                onDroneGain = viewModel::onDroneGain,
                onCalibrate = viewModel::onCalibrateLatency,
            )
        }

        // The loop/overdub button is PINNED above the scrolling list so the
        // one control you hit mid-take never scrolls off screen (the record/
        // play/stop transport is likewise pinned in the Scaffold's bottomBar).
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(innerPadding),
        ) {
            SmartLoopButton(
                transport = transport,
                onTap = viewModel::onSmartLoopButton,
                modifier = Modifier.padding(top = 12.dp),
            )
            if (twoPane) {
                LazyVerticalGrid(
                    columns = GridCells.Fixed(2),
                    modifier = Modifier
                        .weight(1f)
                        .fillMaxWidth(),
                    contentPadding = PaddingValues(12.dp),
                    verticalArrangement = Arrangement.spacedBy(10.dp),
                    horizontalArrangement = Arrangement.spacedBy(10.dp),
                ) {
                    items(tracks, key = { it.index }) { trackRow(it) }
                    item(key = "backing", span = { GridItemSpan(maxLineSpan) }) { backingPanel() }
                    item(key = "fx", span = { GridItemSpan(maxLineSpan) }) { fxPanel() }
                }
            } else {
                LazyColumn(
                    modifier = Modifier
                        .weight(1f)
                        .fillMaxWidth(),
                    contentPadding = PaddingValues(12.dp),
                    verticalArrangement = Arrangement.spacedBy(10.dp),
                ) {
                    items(tracks, key = { it.index }) { trackRow(it) }
                    item(key = "backing") { backingPanel() }
                    item(key = "fx") { fxPanel() }
                }
            }
        }
    }
}
