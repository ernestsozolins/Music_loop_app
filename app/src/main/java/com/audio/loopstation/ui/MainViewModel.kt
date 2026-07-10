package com.audio.loopstation.ui

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.audio.loopstation.AudioEngine
import com.audio.loopstation.StemExporter
import java.io.File
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

/**
 * Single source of truth for the UI. The screen renders exclusively from
 * [transport], [tracks], and [waveform]; every user gesture funnels through
 * an intent method here, which forwards to the native engine's lock-free
 * control surface and updates the flows.
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

    private val engine: AudioEngine = AudioEngine.create(application)
    private val exporter = StemExporter(application)

    private val _transport = MutableStateFlow(TransportUiState())
    val transport: StateFlow<TransportUiState> = _transport.asStateFlow()

    private val _tracks = MutableStateFlow(
        List(engine.trackCount) { i -> TrackUiState(index = i, name = "Track ${i + 1}", isSelected = i == 0) }
    )
    val tracks: StateFlow<List<TrackUiState>> = _tracks.asStateFlow()

    private val _waveform = MutableStateFlow(FloatArray(0))
    val waveform: StateFlow<FloatArray> = _waveform.asStateFlow()

    /** Device disconnect / restart notifications for snackbars. */
    val engineEvents: SharedFlow<AudioEngine.EngineEvent> = engine.events

    init {
        engine.startMeterPolling(viewModelScope)
        viewModelScope.launch { engine.meters.collect(::onMeters) }
    }

    /** Called by MainActivity once RECORD_AUDIO is granted. */
    fun onAudioPermissionGranted() {
        if (!_transport.value.engineReady) {
            val started = engine.start()
            _transport.update { it.copy(engineReady = started) }
        }
    }

    // ------------------------------------------------------------------
    // Engine -> UI (60 Hz)
    // ------------------------------------------------------------------

    private fun onMeters(m: AudioEngine.MeterUiState) {
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
        if (_transport.value.isRecording) engine.stopRecording() else engine.startRecording()
    }

    fun onPlayTap() = engine.startPlayback()

    fun onStopTap() = engine.stopPlayback()

    fun onMetronomeToggle() {
        val t = _transport.value
        engine.setMetronomeState(!t.metronomeOn, t.bpm.toFloat(), t.beatsPerMeasure)
        _transport.update { it.copy(metronomeOn = !t.metronomeOn) }
    }

    fun onBpmChange(delta: Int) {
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
        engine.selectTrack(index)
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
        engine.setTrackGain(index, v)
        _tracks.update { list -> list.map { if (it.index == index) it.copy(volume = v) else it } }
    }

    fun onPanChange(index: Int, pan: Float) {
        val p = pan.coerceIn(-1f, 1f)
        engine.setTrackPan(index, p)
        _tracks.update { list -> list.map { if (it.index == index) it.copy(pan = p) else it } }
    }

    fun onClearTrack(index: Int) = engine.clearTrack(index)

    private inline fun updateAndApplyMutes(transform: (TrackUiState) -> TrackUiState) {
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
        _transport.update { it.copy(exporting = true) }
        return try {
            exporter.exportSessionZip(engine)
        } finally {
            _transport.update { it.copy(exporting = false) }
        }
    }

    override fun onCleared() {
        engine.release()
    }

    companion object {
        const val MIN_BPM = 40
        const val MAX_BPM = 240
    }
}
