package com.audio.loopstation.ui

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.audio.loopstation.AudioEngine
import com.audio.loopstation.StemExporter
import com.audio.loopstation.data.LoopStationDatabase
import com.audio.loopstation.data.SessionEntity
import com.audio.loopstation.data.SessionRepository
import com.audio.loopstation.data.TrackEntity
import java.io.File
import java.util.Locale
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

/**
 * Single source of truth for the UI. The screen renders exclusively from
 * [transport], [tracks], and [waveform]; every user gesture funnels through
 * an intent method here, which forwards to the native engine's lock-free
 * control surface and updates the flows.
 *
 * OWNERSHIP: the engine belongs to [com.audio.loopstation.MediaRecordingService],
 * not to this ViewModel. MainActivity binds to the service and calls
 * [attachEngine]/[detachEngine] as the connection comes and goes, so the UI
 * (and this ViewModel) can be destroyed and recreated freely without ever
 * touching the C++ audio thread. The service also owns the meter polling —
 * this class only collects the resulting StateFlow.
 *
 * Data flows IN from the engine on two paths:
 *  - the 60 Hz meter poll (transport position, engine state, beat flash,
 *    rolling input waveform, per-track content mask), and
 *  - [engineEvents], pushed from Oboe's error thread on device disconnects.
 *
 * Solo is implemented here, not in C++: the engine only knows mute, so this
 * ViewModel computes the effective mute matrix (muted || (anySolo &&
 * !soloed)) and pushes it down whenever mute/solo changes.
 */
class MainViewModel(application: Application) : AndroidViewModel(application) {

    data class TrackUiState(
        val index: Int,
        val name: String,
        val hasContent: Boolean = false,
        val isClearing: Boolean = false,
        val isSelected: Boolean = false,
        val muted: Boolean = false,
        val soloed: Boolean = false,
        val volume: Float = 1.0f,  // 0..1, mapped 1:1 onto engine gain
        val pan: Float = 0.0f,     // -1..1 balance
    )

    // NOTE: the playhead position deliberately does NOT live here. It
    // changes every meter tick (~60 Hz); as a field it would defeat this
    // data class's structural-equality dedup and recompose every collector
    // each frame. It flows separately through [position] and is only read
    // in the draw phase (see PlayheadProgressLine).
    data class TransportUiState(
        val engineState: AudioEngine.State = AudioEngine.State.IDLE,
        val isRecording: Boolean = false,
        val isPlaying: Boolean = false,
        val isCountingIn: Boolean = false,
        val hasLoop: Boolean = false,
        val loopLengthFrames: Int = 0,
        val metronomeOn: Boolean = false,
        val countIn: Boolean = true,
        val quantize: Boolean = true,
        val bpm: Int = 120,
        val beatsPerMeasure: Int = 4,
        val beatInBar: Int = 0,
        val exporting: Boolean = false,
        val saving: Boolean = false,
        val engineReady: Boolean = false,  // streams running (mic permission granted)
    ) {
        /** Label for the on-screen smart loop button (pedal-free workflow). */
        val loopButtonLabel: String = when {
            isCountingIn -> "Count-in…"
            engineState == AudioEngine.State.RECORDING_MASTER -> "Recording — tap to close"
            engineState == AudioEngine.State.OVERDUBBING -> "Overdubbing — tap to end"
            !hasLoop -> "Record loop"
            else -> "Overdub"
        }
    }

    private val exporter = StemExporter(application)
    private val repository = SessionRepository(LoopStationDatabase.get(application))

    private var engine: AudioEngine? = null
    private var metersJob: Job? = null
    private var eventsJob: Job? = null

    /** Room id of the session being edited; null until first save/restore. */
    private var currentSessionId: Long? = null
    private var restoreAttempted = false

    // Pedal/undo bookkeeping: the tracks recorded this session in order
    // (most recent last), the overdub target when recording started, and
    // the previous engine state for edge detection. UI-session state only —
    // it survives configuration changes with this ViewModel but not process
    // death (a restored session starts with an empty undo history).
    private val undoStack = ArrayDeque<Int>()
    private var armedTrack = 0
    private var suppressNextUndoPush = false
    private var lastEngineState = AudioEngine.State.IDLE

    private val _transport = MutableStateFlow(TransportUiState())
    val transport: StateFlow<TransportUiState> = _transport.asStateFlow()

    private val _tracks = MutableStateFlow(emptyList<TrackUiState>())
    val tracks: StateFlow<List<TrackUiState>> = _tracks.asStateFlow()

    private val _waveform = MutableStateFlow(FloatArray(0))
    val waveform: StateFlow<FloatArray> = _waveform.asStateFlow()

    /** Playhead within the loop, 0..1, ~60 Hz. Draw-phase consumers only. */
    private val _position = MutableStateFlow(0f)
    val position: StateFlow<Float> = _position.asStateFlow()

    /** Monitoring reverb parameters (output mix only; recordings stay dry). */
    data class ReverbUiState(val mix: Float = 0f, val roomSize: Float = 0.5f)

    private val _reverb = MutableStateFlow(ReverbUiState())
    val reverb: StateFlow<ReverbUiState> = _reverb.asStateFlow()

    /**
     * Offline waveform (peak bins) per recorded track, refreshed when a
     * track's content appears/changes — not per frame, so collecting this
     * in composition is cheap.
     */
    private val _trackWaveforms = MutableStateFlow<Map<Int, FloatArray>>(emptyMap())
    val trackWaveforms: StateFlow<Map<Int, FloatArray>> = _trackWaveforms.asStateFlow()
    private var prevContentBits = 0

    /** Device disconnect / restart notifications for snackbars. */
    private val _engineEvents = MutableSharedFlow<AudioEngine.EngineEvent>(extraBufferCapacity = 16)
    val engineEvents: SharedFlow<AudioEngine.EngineEvent> = _engineEvents.asSharedFlow()

    // ------------------------------------------------------------------
    // Service connection (MainActivity's ServiceConnection calls these)
    // ------------------------------------------------------------------

    /** Start observing the service-owned engine. Idempotent per instance. */
    fun attachEngine(engine: AudioEngine) {
        if (this.engine === engine) return
        detachEngine()
        this.engine = engine
        // This ViewModel survives configuration changes, so an existing
        // track list (the user's volume/pan/mute UI state) is kept; it is
        // only rebuilt when we meet a different engine (fresh process).
        if (_tracks.value.size != engine.trackCount) {
            _tracks.value = List(engine.trackCount) { i ->
                TrackUiState(index = i, name = "Track ${i + 1}", isSelected = i == 0)
            }
        }
        metersJob = viewModelScope.launch { engine.meters.collect(::onMeters) }
        eventsJob = viewModelScope.launch { engine.events.collect { _engineEvents.emit(it) } }
        maybeRestoreLastSession()
    }

    /** Stop observing. NEVER releases the engine — the service owns it. */
    fun detachEngine() {
        metersJob?.cancel()
        metersJob = null
        eventsJob?.cancel()
        eventsJob = null
        engine = null
    }

    /** Result of MediaRecordingService.ensureEngineStarted(), from the Activity. */
    fun onEngineReady(ready: Boolean) {
        _transport.update { it.copy(engineReady = ready) }
        // The restore commit lands on the audio thread, so it can only
        // complete once the streams run — try again if attach came first.
        if (ready) maybeRestoreLastSession()
    }

    // ------------------------------------------------------------------
    // Session persistence
    // ------------------------------------------------------------------

    /**
     * Loads the last active session once per process: mix parameters flow
     * to the engine through the same JNI setters the sliders use, then the
     * stem files stream back into the C++ loop tracks on the engine's
     * restore worker. Runs only against a fresh engine (the native side
     * refuses to clobber a live loop).
     */
    private fun maybeRestoreLastSession() {
        val engine = this.engine ?: return
        if (restoreAttempted || !_transport.value.engineReady) return
        restoreAttempted = true
        viewModelScope.launch {
            val saved = repository.latestSessionWithTracks() ?: return@launch
            currentSessionId = saved.session.id

            // 1. Session-level state: tempo + meter (metronome stays off
            //    until the user arms it; the tempo is preloaded lock-free).
            _transport.update {
                it.copy(bpm = saved.session.bpm, beatsPerMeasure = saved.session.timeSignature)
            }
            engine.setMetronomeState(
                false, saved.session.bpm.toFloat(), saved.session.timeSignature,
            )

            // 2. Per-track mix state -> UI flows + engine (gain/pan/mutes).
            applySavedTrackStates(engine, saved.tracks)

            // 3. Audio: stems back into the loop tracks (native worker). On
            //    failure the mix parameters above still stand and the UI
            //    simply shows empty tracks — the stems on disk are untouched.
            val paths = saved.tracks
                .filter { File(it.filePath).exists() }
                .associate { it.trackIndex to it.filePath }
            if (paths.isNotEmpty()) engine.restoreSession(paths)
        }
    }

    private fun applySavedTrackStates(engine: AudioEngine, saved: List<TrackEntity>) {
        val byIndex = saved.associateBy { it.trackIndex }
        _tracks.update { list ->
            list.map { t ->
                byIndex[t.index]?.let { s ->
                    t.copy(volume = s.volume, pan = s.pan, muted = s.isMuted, soloed = s.isSoloed)
                } ?: t
            }
        }
        for (t in _tracks.value) {
            engine.setTrackGain(t.index, faderToGain(t.volume))
            engine.setTrackPan(t.index, t.pan)
        }
        updateAndApplyMutes { it }  // pushes the effective mute/solo matrix
    }

    /**
     * Persists the whole session: finalize spooled audio, re-export one
     * stem per track into this session's directory, then commit metadata +
     * track list to Room in one transaction. The Stop/onStop UI wiring
     * calls this; it is also safe to invoke from a future "Save" button.
     */
    suspend fun saveSession(name: String? = null): Boolean {
        val engine = this.engine ?: return false
        _transport.update { it.copy(saving = true) }
        try {
            return doSaveSession(engine, name)
        } finally {
            _transport.update { it.copy(saving = false) }
        }
    }

    private suspend fun doSaveSession(engine: AudioEngine, name: String?): Boolean {
        if (!engine.flushAndCloseSession()) return false

        val t = _transport.value
        val id = repository.ensureSessionId(
            existingId = currentSessionId,
            name = name ?: defaultSessionName(),
            bpm = t.bpm,
            timeSignature = t.beatsPerMeasure,
        )
        currentSessionId = id

        val dir = File(getApplication<Application>().filesDir, "sessions/$id").apply { mkdirs() }
        engine.exportStems(dir.absolutePath) ?: return false

        // Snapshot only tracks whose stem actually landed on disk.
        val snapshots = _tracks.value.mapNotNull { track ->
            val stem = File(dir, String.format(Locale.US, "track_%02d.wav", track.index + 1))
            if (track.hasContent && stem.exists()) {
                SessionRepository.TrackSnapshot(
                    trackIndex = track.index,
                    filePath = stem.absolutePath,
                    volume = track.volume,
                    pan = track.pan,
                    isMuted = track.muted,
                    isSoloed = track.soloed,
                )
            } else {
                null
            }
        }
        repository.commitSessionState(id, name, t.bpm, t.beatsPerMeasure, snapshots)
        return true
    }

    private fun defaultSessionName(): String =
        "Session ${java.text.SimpleDateFormat("yyyy-MM-dd HH:mm", Locale.US).format(java.util.Date())}"

    // ------------------------------------------------------------------
    // Engine -> UI (60 Hz)
    // ------------------------------------------------------------------

    private fun onMeters(m: AudioEngine.MeterUiState) {
        val engine = this.engine ?: return
        _waveform.value = m.waveform
        val latest = m.latest
        val state = latest?.state ?: _transport.value.engineState
        val loopLen = latest?.loopLengthFrames ?: 0
        val playhead = latest?.playheadFrames ?: 0

        // High-rate values go to their dedicated draw-phase flow…
        _position.value = if (loopLen > 0) playhead.toFloat() / loopLen.toFloat() else 0f

        // …while the transport data class (structural equality => StateFlow
        // dedup) only emits on real state changes.
        _transport.update {
            it.copy(
                engineState = state,
                isRecording = state == AudioEngine.State.RECORDING_MASTER ||
                    state == AudioEngine.State.OVERDUBBING,
                isPlaying = state == AudioEngine.State.PLAYING ||
                    state == AudioEngine.State.OVERDUBBING,
                isCountingIn = state == AudioEngine.State.COUNT_IN,
                hasLoop = loopLen > 0,
                loopLengthFrames = loopLen,
                metronomeOn = m.metronomeActive,
                beatInBar = m.beatInBar,
            )
        }
        val mask = engine.trackContentMask
        _tracks.update { list ->
            list.map { t ->
                t.copy(
                    hasContent = mask and (1 shl t.index) != 0,
                    isClearing = mask and (1 shl (t.index + 16)) != 0,
                )
            }
        }

        // Content appeared or vanished: refresh the affected offline waveforms.
        val contentBits = mask and 0xFFFF
        if (contentBits != prevContentBits) {
            val changed = contentBits xor prevContentBits
            prevContentBits = contentBits
            for (t in _tracks.value) {
                if (changed and (1 shl t.index) != 0) refreshTrackWaveform(t.index)
            }
        }

        // A recording/overdub pass just ended: remember which track it
        // landed on so the pedal's Backspace can undo it, and repaint its
        // offline waveform (an overdub changes audio without changing mask).
        val prev = lastEngineState
        if (prev != state &&
            (prev == AudioEngine.State.RECORDING_MASTER || prev == AudioEngine.State.OVERDUBBING)
        ) {
            refreshTrackWaveform(armedTrack)
            if (suppressNextUndoPush) {
                suppressNextUndoPush = false
            } else if (undoStack.lastOrNull() != armedTrack) {  // fold repeat passes
                undoStack.addLast(armedTrack)
            }
        }
        lastEngineState = state
    }

    private fun refreshTrackWaveform(index: Int) {
        val engine = this.engine ?: return
        viewModelScope.launch(Dispatchers.Default) {
            val bins = engine.trackWaveform(index)  // full-track scan: off main
            _trackWaveforms.update { map ->
                if (bins != null) map + (index to bins) else map - index
            }
        }
    }

    // ------------------------------------------------------------------
    // Transport intents
    // ------------------------------------------------------------------

    fun onRecordTap() {
        val engine = this.engine ?: return
        if (_transport.value.isRecording) {
            engine.stopRecording()
        } else {
            startRecordingWithSnapshot(engine)
        }
    }

    /** Snapshot the armed track (multi-MB memcpy, so off-main) then record. */
    private fun startRecordingWithSnapshot(engine: AudioEngine) {
        armedTrack = selectedTrackIndex()
        val track = armedTrack
        viewModelScope.launch(Dispatchers.Default) {
            engine.snapshotTrackForUndo(track)  // pre-pass audio for Backspace
            engine.startRecording()
        }
    }

    fun onPlayTap() {
        engine?.startPlayback()
    }

    fun onStopTap() {
        engine?.stopPlayback()
    }

    fun onMetronomeToggle() {
        val engine = this.engine ?: return
        val t = _transport.value
        engine.setMetronomeState(!t.metronomeOn, t.bpm.toFloat(), t.beatsPerMeasure)
        _transport.update { it.copy(metronomeOn = !t.metronomeOn) }
    }

    fun onBpmChange(delta: Int) {
        val engine = this.engine ?: return
        val t = _transport.value
        val bpm = (t.bpm + delta).coerceIn(MIN_BPM, MAX_BPM)
        // Lock-free hand-off; the engine applies it at the next beat boundary.
        engine.setMetronomeState(t.metronomeOn, bpm.toFloat(), t.beatsPerMeasure)
        _transport.update { it.copy(bpm = bpm) }
    }

    // ------------------------------------------------------------------
    // Pedal intents (see PedalController for the key mapping)
    // ------------------------------------------------------------------

    /** Space: play if stopped, stop if playing. */
    fun onPlayStopToggle() {
        if (_transport.value.isPlaying) onStopTap() else onPlayTap()
    }

    /**
     * Enter: one-switch loop workflow. No loop -> record the master; press
     * again -> close it. Loop playing -> start an overdub on the selected
     * track; press again -> end the pass and auto-advance the selection so
     * the next press layers onto the next track, hands never leaving the
     * instrument.
     */
    fun onOverdubPedal() {
        val engine = this.engine ?: return
        val t = _transport.value
        if (t.isRecording) {
            engine.stopRecording()
            onSelectNextTrack()
        } else {
            startRecordingWithSnapshot(engine)
        }
    }

    /**
     * Backspace: undo the last PASS. Because every record start snapshots
     * the armed track, undoing an overdub restores the track's previous
     * layers instead of wiping it (one snapshot deep; older history falls
     * back to whole-track clears). Mid-recording it cancels the take in
     * progress. When the last recorded track goes, the loop is dropped so
     * the next recording redefines the loop length. The undo history does
     * not survive process death.
     */
    fun onUndoPedal() {
        val engine = this.engine ?: return
        if (_transport.value.isRecording) {
            suppressNextUndoPush = true  // this pass must not become undoable
            engine.stopRecording()
            val track = armedTrack
            viewModelScope.launch(Dispatchers.Default) { undoTrack(engine, track) }
            return
        }
        val track = undoStack.removeLastOrNull() ?: return
        viewModelScope.launch(Dispatchers.Default) { undoTrack(engine, track) }
    }

    private suspend fun undoTrack(engine: AudioEngine, track: Int) {
        // Per-pass restore when the snapshot matches; whole-track clear
        // otherwise (undoLastPass handles the started-from-empty case itself).
        val handled = engine.undoPassTrack == track && engine.undoLastPass()
        if (!handled) engine.clearTrack(track)
        delay(120)  // let the audio thread commit + amortized clears register
        refreshTrackWaveform(track)
        if (engine.trackContentMask and 0xFFFF == 0) {
            engine.clearAll()  // nothing recorded remains: drop the loop too
        }
    }

    fun onSelectNextTrack() = selectRelativeTrack(+1)
    fun onSelectPreviousTrack() = selectRelativeTrack(-1)

    private fun selectRelativeTrack(delta: Int) {
        val count = _tracks.value.size
        if (count == 0) return
        onTrackSelect((selectedTrackIndex() + delta + count) % count)
    }

    private fun selectedTrackIndex(): Int =
        _tracks.value.firstOrNull { it.isSelected }?.index ?: 0

    // ------------------------------------------------------------------
    // Track intents
    // ------------------------------------------------------------------

    fun onTrackSelect(index: Int) {
        engine?.selectTrack(index) ?: return
        _tracks.update { list -> list.map { it.copy(isSelected = it.index == index) } }
    }

    fun onMuteToggle(index: Int) = updateAndApplyMutes { t ->
        if (t.index == index) t.copy(muted = !t.muted) else t
    }

    fun onSoloToggle(index: Int) = updateAndApplyMutes { t ->
        if (t.index == index) t.copy(soloed = !t.soloed) else t
    }

    fun onVolumeChange(index: Int, volume: Float) {
        val v = volume.coerceIn(0f, 1f)
        engine?.setTrackGain(index, faderToGain(v)) ?: return  // dB taper, not linear
        _tracks.update { list -> list.map { if (it.index == index) it.copy(volume = v) else it } }
    }

    fun onPanChange(index: Int, pan: Float) {
        val p = pan.coerceIn(-1f, 1f)
        engine?.setTrackPan(index, p) ?: return
        _tracks.update { list -> list.map { if (it.index == index) it.copy(pan = p) else it } }
    }

    fun onClearTrack(index: Int) {
        engine?.clearTrack(index)
    }

    // ------------------------------------------------------------------
    // Monitoring reverb intents (output mix only; recordings stay dry)
    // ------------------------------------------------------------------

    fun onReverbMixChange(mix: Float) {
        val v = mix.coerceIn(0f, 1f)
        engine?.setReverbMix(v) ?: return
        _reverb.update { it.copy(mix = v) }
    }

    fun onReverbRoomSizeChange(size: Float) {
        val v = size.coerceIn(0f, 1f)
        engine?.setReverbRoomSize(v) ?: return
        _reverb.update { it.copy(roomSize = v) }
    }

    // ------------------------------------------------------------------
    // Metronome workflow toggles + smart loop button
    // ------------------------------------------------------------------

    fun onCountInToggle() {
        val on = !_transport.value.countIn
        engine?.setCountInEnabled(on) ?: return
        _transport.update { it.copy(countIn = on) }
    }

    fun onQuantizeToggle() {
        val on = !_transport.value.quantize
        engine?.setLoopQuantize(on) ?: return
        _transport.update { it.copy(quantize = on) }
    }

    /**
     * On-screen equivalent of the Enter pedal — the whole tablet-without-a-
     * pedal workflow on one big button: record the master, close it, start
     * an overdub, or end a pass and advance. Delegates to [onOverdubPedal].
     */
    fun onSmartLoopButton() = onOverdubPedal()

    // ------------------------------------------------------------------
    // dB-calibrated fader taper
    // ------------------------------------------------------------------

    /**
     * Maps a 0..1 fader position to linear gain on a dB curve: hard 0 at the
     * bottom, unity (0 dB) at [UNITY_FADER], up to +6 dB at the top. Far more
     * usable than a linear slider, where unity would sit at 1.0 and the whole
     * useful range bunches near the top.
     */
    private fun faderToGain(position: Float): Float {
        if (position <= 0f) return 0f
        // Piecewise dB: below unity spans MIN_DB..0, above spans 0..MAX_DB.
        val decibels = if (position >= UNITY_FADER) {
            (position - UNITY_FADER) / (1f - UNITY_FADER) * MAX_DB
        } else {
            (UNITY_FADER - position) / UNITY_FADER * MIN_DB
        }
        return Math.pow(10.0, decibels / 20.0).toFloat()
    }

    /** dB readout for a fader position (for the UI label). */
    fun faderDb(position: Float): Float = when {
        position <= 0f -> Float.NEGATIVE_INFINITY
        position >= UNITY_FADER -> (position - UNITY_FADER) / (1f - UNITY_FADER) * MAX_DB
        else -> (UNITY_FADER - position) / UNITY_FADER * MIN_DB
    }

    private inline fun updateAndApplyMutes(transform: (TrackUiState) -> TrackUiState) {
        val engine = this.engine ?: return
        val list = _tracks.value.map(transform)
        _tracks.value = list
        val anySolo = list.any { it.soloed }
        for (t in list) {
            engine.setTrackMuted(t.index, t.muted || (anySolo && !t.soloed))
        }
    }

    // ------------------------------------------------------------------
    // Export
    // ------------------------------------------------------------------

    /** Finalizes + zips the session; the caller shares the returned file. */
    suspend fun exportSession(): File? {
        val engine = this.engine ?: return null
        _transport.update { it.copy(exporting = true) }
        return try {
            exporter.exportSessionZip(engine)
        } finally {
            _transport.update { it.copy(exporting = false) }
        }
    }

    override fun onCleared() {
        // Deliberately does NOT release the engine: MediaRecordingService
        // owns its lifecycle and may be recording long after this UI died.
        detachEngine()
    }

    // ------------------------------------------------------------------
    // Autosave + session browser
    // ------------------------------------------------------------------

    /** Sessions for the browser sheet; recomposes on every save (Room Flow). */
    val sessions: StateFlow<List<SessionEntity>> =
        repository.observeSessions()
            .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000L), emptyList())

    private var autosaving = false

    /**
     * Persist the current session if there's anything to save and we're not
     * mid-recording. Called on app background (Activity.onStop) and after
     * the transport settles, so work is never lost. Debounced by a flag so
     * overlapping saves can't stack.
     */
    fun autosaveIfNeeded() {
        val engine = this.engine ?: return
        if (autosaving) return
        if (_transport.value.isRecording || _transport.value.isCountingIn) return
        if (engine.trackContentMask and 0xFFFF == 0) return  // nothing recorded
        autosaving = true
        // Capture `engine` explicitly: this is called from Activity.onStop
        // right before detachEngine() nulls the field, and doSaveSession uses
        // the captured reference (the engine object outlives the UI in the
        // service). viewModelScope survives backgrounding (it's cancelled only
        // at onCleared), and the foreground service keeps the process alive.
        viewModelScope.launch {
            try {
                doSaveSession(engine, null)
            } finally {
                autosaving = false
            }
        }
    }

    /**
     * Loads a saved session into the (fresh) engine: clears the current
     * loop, applies tempo + mix, and streams the stems back. Refuses while
     * recording. Returns true on success.
     */
    suspend fun loadSession(sessionId: Long): Boolean {
        val engine = this.engine ?: return false
        if (_transport.value.isRecording) return false
        val saved = repository.sessionWithTracks(sessionId) ?: return false
        engine.resetLoopForRestore()  // immediate reset (no slow buffer wipe)
        delay(40)  // let the reset command drain before restoring
        currentSessionId = saved.session.id
        _transport.update {
            it.copy(bpm = saved.session.bpm, beatsPerMeasure = saved.session.timeSignature)
        }
        engine.setMetronomeState(false, saved.session.bpm.toFloat(), saved.session.timeSignature)
        applySavedTrackStates(engine, saved.tracks)
        val paths = saved.tracks
            .filter { File(it.filePath).exists() }
            .associate { it.trackIndex to it.filePath }
        return if (paths.isNotEmpty()) engine.restoreSession(paths) else true
    }

    fun deleteSession(session: SessionEntity) {
        viewModelScope.launch { repository.deleteSession(session) }
    }

    companion object {
        const val MIN_BPM = 40
        const val MAX_BPM = 240

        // dB fader taper: unity (0 dB) sits at this fader position; below maps
        // to MIN_DB..0, above to 0..MAX_DB.
        private const val UNITY_FADER = 0.8f
        private const val MIN_DB = -48f
        private const val MAX_DB = 6f
    }
}
