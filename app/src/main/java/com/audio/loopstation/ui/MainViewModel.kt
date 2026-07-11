package com.audio.loopstation.ui

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.audio.loopstation.AudioEngine
import com.audio.loopstation.StemExporter
import com.audio.loopstation.data.LoopStationDatabase
import com.audio.loopstation.data.SessionRepository
import com.audio.loopstation.data.TrackEntity
import java.io.File
import java.util.Locale
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
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

    data class TransportUiState(
        val engineState: AudioEngine.State = AudioEngine.State.IDLE,
        val isRecording: Boolean = false,
        val isPlaying: Boolean = false,
        val loopLengthFrames: Int = 0,
        val playheadFrames: Int = 0,
        val positionFraction: Float = 0f,  // playhead within the loop, 0..1
        val metronomeOn: Boolean = false,
        val bpm: Int = 120,
        val beatsPerMeasure: Int = 4,
        val beatInBar: Int = 0,
        val exporting: Boolean = false,
        val engineReady: Boolean = false,  // streams running (mic permission granted)
    )

    private val exporter = StemExporter(application)
    private val repository = SessionRepository(LoopStationDatabase.get(application))

    private var engine: AudioEngine? = null
    private var metersJob: Job? = null
    private var eventsJob: Job? = null

    /** Room id of the session being edited; null until first save/restore. */
    private var currentSessionId: Long? = null
    private var restoreAttempted = false

    private val _transport = MutableStateFlow(TransportUiState())
    val transport: StateFlow<TransportUiState> = _transport.asStateFlow()

    private val _tracks = MutableStateFlow(emptyList<TrackUiState>())
    val tracks: StateFlow<List<TrackUiState>> = _tracks.asStateFlow()

    private val _waveform = MutableStateFlow(FloatArray(0))
    val waveform: StateFlow<FloatArray> = _waveform.asStateFlow()

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
            engine.setTrackGain(t.index, t.volume)
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
        _transport.update {
            it.copy(
                engineState = state,
                isRecording = state == AudioEngine.State.RECORDING_MASTER ||
                    state == AudioEngine.State.OVERDUBBING,
                isPlaying = state == AudioEngine.State.PLAYING ||
                    state == AudioEngine.State.OVERDUBBING,
                loopLengthFrames = loopLen,
                playheadFrames = playhead,
                positionFraction = if (loopLen > 0) {
                    playhead.toFloat() / loopLen.toFloat()
                } else 0f,
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
    }

    // ------------------------------------------------------------------
    // Transport intents
    // ------------------------------------------------------------------

    fun onRecordTap() {
        val engine = this.engine ?: return
        if (_transport.value.isRecording) engine.stopRecording() else engine.startRecording()
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
        engine?.setTrackGain(index, v) ?: return
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

    companion object {
        const val MIN_BPM = 40
        const val MAX_BPM = 240
    }
}
