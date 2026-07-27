package com.audio.loopstation

import android.app.ActivityManager
import android.content.Context
import java.io.File
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull
import kotlin.math.min

/**
 * Kotlin wrapper around the native looper engine (`liblooperengine.so`).
 *
 * Threading:
 *  - Control calls (transport, metronome, parameters) may come from any
 *    thread; they resolve to lock-free hand-offs on the native side. Keep
 *    [release] confined to one owner (normally the main thread / ViewModel
 *    onCleared).
 *  - Meter polling runs in ONE coroutine started via [startMeterPolling] —
 *    the native meter queue is single-consumer.
 *
 * UI consumption (Phase 3 Compose layer):
 * ```
 * val meters by engine.meters.collectAsStateWithLifecycle()
 * Canvas(...) {
 *     val barWidth = size.width / meters.waveform.size
 *     meters.waveform.forEachIndexed { i, rms ->
 *         drawRect(
 *             topLeft = Offset(i * barWidth, size.height * (1f - rms)),
 *             size = Size(barWidth, size.height * rms),
 *         )
 *     }
 * }
 * ```
 */
class AudioEngine private constructor(private var handle: Long) {

    init {
        // Route native engine events (device disconnects, restart results)
        // into [events]. Registered before any stream can exist.
        nativeSetEventListener(handle, this)
    }

    /**
     * Engine events pushed from native (a non-main thread): the USB
     * interface or headset vanished, and how the automatic recovery went.
     * On DISCONNECTED the engine has already paused the transport and
     * finalized any capture file — the UI's job is to warn the user.
     */
    enum class EngineEvent {
        INPUT_DISCONNECTED, OUTPUT_DISCONNECTED, RESTART_SUCCEEDED, RESTART_FAILED, UNKNOWN;

        companion object {
            fun fromNative(type: Int): EngineEvent = entries.getOrElse(type) { UNKNOWN }
        }
    }

    private val _events = MutableSharedFlow<EngineEvent>(extraBufferCapacity = 16)
    val events: SharedFlow<EngineEvent> = _events.asSharedFlow()

    /**
     * Called from native on Oboe's error thread. Keep the name/signature in
     * sync with AudioEngine_JNI.cpp (and keep it from being minified:
     * `-keepclassmembers class com.audio.loopstation.AudioEngine { void onNativeEvent(int, int); }`).
     */
    @Suppress("unused")
    private fun onNativeEvent(type: Int, arg: Int) {
        _events.tryEmit(EngineEvent.fromNative(type))
    }

    /** Mirrors looper::EngineState — keep the order in sync with AudioEngine.h. */
    enum class State {
        IDLE, RECORDING_MASTER, PLAYING, OVERDUBBING, STOPPED, COUNT_IN;

        companion object {
            fun fromNative(ordinal: Int): State = entries.getOrElse(ordinal) { IDLE }
        }
    }

    /** Mirrors looper::TrackTransport — per-track play/record/stop state. */
    enum class TrackTransport {
        EMPTY, RECORDING, OVERDUBBING, PLAYING, STOPPED;

        val isPlaying: Boolean get() = this == PLAYING || this == OVERDUBBING
        val isRecording: Boolean get() = this == RECORDING || this == OVERDUBBING

        companion object {
            fun fromNative(ordinal: Int): TrackTransport = entries.getOrElse(ordinal) { EMPTY }
        }
    }

    /** Most recent per-block meter values from the audio callback. */
    class MeterPoint(
        val inputRms: Float,
        val inputPeak: Float,
        val mixRms: Float,
        val mixPeak: Float,
        val playheadFrames: Int,
        val loopLengthFrames: Int,
        val state: State,
    )

    /**
     * Snapshot published at ~60 Hz for the visualizer. [waveform] is a
     * rolling history of per-block input RMS (oldest first, newest last),
     * ready to draw as bars. A fresh array is published on every poll, so
     * StateFlow's equality check never suppresses a frame.
     */
    class MeterUiState(
        val waveform: FloatArray,
        val latest: MeterPoint?,
        val metronomeActive: Boolean,
        val beatInBar: Int,
        val beatCount: Long,
        /** Beats left until a pending count-in starts recording; 0 = none. */
        val countInBeatsRemaining: Int = 0,
    )

    // ------------------------------------------------------------------
    // Lifecycle
    // ------------------------------------------------------------------

    /** Opens and starts the Oboe streams. Returns false if the device refused. */
    fun start(): Boolean = handle != 0L && nativeStart(handle)

    /** Stops and closes the streams; loop content is kept for the next start(). */
    fun stop() {
        if (handle != 0L) nativeStop(handle)
    }

    /**
     * Routes playback to an [android.media.AudioDeviceInfo] id (0 = system
     * default). Reopens the streams if running — loops and any in-progress
     * recording survive. Blocks briefly, so runs on [Dispatchers.IO].
     */
    suspend fun setOutputDevice(deviceId: Int): Boolean = withContext(Dispatchers.IO) {
        val h = handle
        h != 0L && nativeSetOutputDevice(h, deviceId)
    }

    /** Routes capture to a device id (0 = system default). Same semantics. */
    suspend fun setInputDevice(deviceId: Int): Boolean = withContext(Dispatchers.IO) {
        val h = handle
        h != 0L && nativeSetInputDevice(h, deviceId)
    }

    /** The currently targeted output device id (0 = system default). */
    val outputDeviceId: Int
        get() = if (handle != 0L) nativeGetOutputDevice(handle) else 0

    /** Destroys the native engine. The instance must not be used afterwards. */
    fun release() {
        val h = handle
        handle = 0L
        if (h != 0L) {
            nativeSetEventListener(h, null)
            nativeStop(h)
            nativeDestroy(h)
        }
    }

    // ------------------------------------------------------------------
    // Latency calibration
    // ------------------------------------------------------------------

    /**
     * Ping-and-listen round-trip measurement. Preconditions: engine started,
     * transport stopped, capture off; monitor speakers (or a loopback cable)
     * must reach the microphone. Suspends until the run finishes and returns
     * the latency in frames — already applied by the engine as the overdub
     * record offset — or null on failure/timeout.
     */
    suspend fun calibrateLatency(timeoutMs: Long = 10_000): Int? {
        val h = handle
        if (h == 0L) return null
        nativeStartCalibration(h)
        val finalState = withTimeoutOrNull(timeoutMs) {
            var state = nativeGetCalibrationState(h)
            while (state == CAL_RUNNING) {
                delay(16)
                state = nativeGetCalibrationState(h)
            }
            state
        }
        if (finalState == null) {  // timed out: abort the run
            nativeCancelCalibration(h)
            return null
        }
        return if (finalState == CAL_SUCCEEDED) nativeGetCalibratedLatencyFrames(h) else null
    }

    /** Last successful measurement in frames (-1 if never calibrated). */
    val calibratedLatencyFrames: Int
        get() = if (handle != 0L) nativeGetCalibratedLatencyFrames(handle) else -1

    /** Same, as milliseconds for display. */
    val calibratedLatencyMillis: Float
        get() {
            val frames = calibratedLatencyFrames
            val rate = sampleRate
            return if (frames >= 0 && rate > 0) frames * 1000f / rate else -1f
        }

    // ------------------------------------------------------------------
    // Transport
    // ------------------------------------------------------------------

    /** First recording defines the master loop; later calls start an overdub pass. */
    fun startRecording() { if (handle != 0L) nativeStartRecording(handle) }

    /** Closes the master loop (and starts playback) or ends the overdub pass. */
    fun stopRecording() { if (handle != 0L) nativeStopRecording(handle) }

    /** (Re)starts playback from the top of the loop (all tracks). */
    fun startPlayback() { if (handle != 0L) nativeStartPlayback(handle) }

    /** Halts the transport; cancels a master recording in progress. */
    fun stopPlayback() { if (handle != 0L) nativeStopPlayback(handle) }

    // ------------------------------------------------------------------
    // Per-track transport (RC-505 model): each track is an independent
    // loop player with its own record/play/stop and its own playhead.
    // ------------------------------------------------------------------

    /** Toggle record/overdub on a track (fresh -> record, playing -> overdub). */
    fun recordTrack(track: Int) { if (handle != 0L) nativeRecordTrack(handle, track) }

    /** (Re)start a track from its beginning (frame 0). */
    fun playTrack(track: Int) { if (handle != 0L) nativePlayTrack(handle, track) }

    /** Stop just this track. */
    fun stopTrack(track: Int) { if (handle != 0L) nativeStopTrack(handle, track) }

    /** Start every track that holds content, from the top. */
    fun playAll() { if (handle != 0L) nativePlayAll(handle) }

    /** Stop every track (and cancel a master take in progress). */
    fun stopAll() { if (handle != 0L) nativeStopAll(handle) }

    fun trackTransport(track: Int): TrackTransport =
        TrackTransport.fromNative(if (handle != 0L) nativeGetTrackTransport(handle, track) else 0)

    /** This track's playhead / record cursor in frames (for the UI timer). */
    fun trackPositionFrames(track: Int): Int =
        if (handle != 0L) nativeGetTrackPosition(handle, track) else 0

    fun clearAll() { if (handle != 0L) nativeClearAll(handle) }
    fun clearTrack(track: Int) { if (handle != 0L) nativeClearTrack(handle, track) }

    /** Immediate loop/content reset (no buffer wipe) — precedes [restoreSession]. */
    fun resetLoopForRestore() { if (handle != 0L) nativeResetLoopForRestore(handle) }

    // ------------------------------------------------------------------
    // Metronome
    // ------------------------------------------------------------------

    /**
     * Lock-free update, applied by the audio thread at the next beat
     * boundary — safe to call continuously from a BPM slider mid-song.
     */
    fun setMetronomeState(isActive: Boolean, bpm: Float, beatsPerMeasure: Int) {
        if (handle != 0L) nativeSetMetronomeState(handle, isActive, bpm, beatsPerMeasure)
    }

    fun setMetronomeGain(gain: Float) { if (handle != 0L) nativeSetMetronomeGain(handle, gain) }

    /** Rhythm voice: CLICK or a synthesized kick/snare/hat BEAT. */
    enum class RhythmStyle { CLICK, BEAT }

    fun setMetronomeStyle(style: RhythmStyle) {
        if (handle != 0L) nativeSetMetronomeStyle(handle, style.ordinal)
    }

    /**
     * Phase-lock the click to the loop (default ON in the engine): while a
     * loop plays or is overdubbed, the loop start is bar 1 beat 1 and the
     * beat grid re-anchors at every loop wrap. Disable for a free-running
     * click that ignores the loop.
     */
    fun setMetronomeSyncToLoop(enabled: Boolean) {
        if (handle != 0L) nativeSetMetronomeSync(handle, enabled)
    }

    /** With the click on, master recording starts at the next downbeat (default ON). */
    fun setCountInEnabled(enabled: Boolean) {
        if (handle != 0L) nativeSetCountInEnabled(handle, enabled)
    }

    /** Count-in length in bars (1..8). */
    fun setCountInBars(bars: Int) {
        if (handle != 0L) nativeSetCountInBars(handle, bars)
    }

    /** With the click on, the master loop length is rounded to whole bars (default ON). */
    fun setLoopQuantize(enabled: Boolean) {
        if (handle != 0L) nativeSetLoopQuantize(handle, enabled)
    }

    /**
     * Multichannel input mapping for interfaces like a 4-channel H2n mode:
     * request [channels] capture channels (0 = engine default) and feed the
     * engine's L/R from source channels [mapLeft]/[mapRight] (same index
     * twice = mono). The channel count takes effect at the next engine
     * start; the map indices apply live.
     */
    fun setInputChannels(channels: Int, mapLeft: Int, mapRight: Int) {
        if (handle != 0L) nativeSetInputChannels(handle, channels, mapLeft, mapRight)
    }

    // ------------------------------------------------------------------
    // Output monitoring reverb (Freeverb tank in C++). Applied ONLY to
    // the headphone/speaker mix — takes, loop tracks, and exported stems
    // stay 100% dry. All setters are lock-free; drive them from sliders
    // freely mid-performance.
    // ------------------------------------------------------------------

    /** Tail length, 0..1. */
    fun setReverbRoomSize(size: Float) { if (handle != 0L) nativeSetReverbRoomSize(handle, size) }

    /** Dry/wet balance: 0 = fully dry (default), 1 = fully wet. */
    fun setReverbMix(mix: Float) { if (handle != 0L) nativeSetReverbMix(handle, mix) }

    /** High-frequency absorption of the tail, 0..1. */
    fun setReverbDamping(damping: Float) { if (handle != 0L) nativeSetReverbDamping(handle, damping) }

    /** Hard bypass (saves CPU); the tail restarts clean on re-enable. */
    fun setReverbEnabled(enabled: Boolean) { if (handle != 0L) nativeSetReverbEnabled(handle, enabled) }

    /** Multimode filter FX (monitor/output only; recordings stay dry). */
    enum class FilterMode { LOW_PASS, HIGH_PASS, BAND_PASS }

    fun setFilterEnabled(enabled: Boolean) { if (handle != 0L) nativeSetFilterEnabled(handle, enabled) }
    fun setFilterCutoff(hz: Float) { if (handle != 0L) nativeSetFilterCutoff(handle, hz) }
    fun setFilterResonance(r: Float) { if (handle != 0L) nativeSetFilterResonance(handle, r) }
    fun setFilterMode(mode: FilterMode) {
        if (handle != 0L) nativeSetFilterMode(handle, mode.ordinal)
    }

    /** Tuning / practice drone (output only; never recorded). */
    fun setDroneEnabled(enabled: Boolean) { if (handle != 0L) nativeSetDroneEnabled(handle, enabled) }
    fun setDroneFrequency(hz: Float) { if (handle != 0L) nativeSetDroneFrequency(handle, hz) }
    fun setDroneGain(gain: Float) { if (handle != 0L) nativeSetDroneGain(handle, gain) }

    // ------------------------------------------------------------------
    // Disk spooling — long-form capture and backing-track streaming.
    // All file I/O runs on the engine's DiskWriter/DiskReader threads.
    // ------------------------------------------------------------------

    enum class BackingTrackState {
        EMPTY, LOADING, READY, PLAYING, ENDED, ERROR;

        companion object {
            fun fromNative(ordinal: Int): BackingTrackState =
                entries.getOrElse(ordinal) { EMPTY }
        }
    }

    /**
     * Starts spooling to a float32 .wav at [path] (create one with
     * [newCaptureFile]). Asynchronous: poll [isCapturing]. With
     * [captureMix] the full mix (loops + monitor + backing tracks, but
     * never the metronome click) is captured instead of the raw input.
     */
    fun startCapture(path: String, captureMix: Boolean = false) {
        if (handle != 0L) nativeStartCapture(handle, path, captureMix)
    }

    /** Finalizes the .wav (header patched) on the writer thread. */
    fun stopCapture() { if (handle != 0L) nativeStopCapture(handle) }

    val isCapturing: Boolean get() = handle != 0L && nativeIsCapturing(handle)
    val capturedFrames: Long get() = if (handle != 0L) nativeGetCapturedFrames(handle) else 0L
    val captureDroppedFrames: Long
        get() = if (handle != 0L) nativeGetCaptureDroppedFrames(handle) else 0L

    /**
     * Loads a .wav into a streaming slot (0 until [BACKING_STREAM_SLOTS]).
     * Mono or stereo, 16-bit PCM or 32-bit float, any sample rate (files
     * that differ from the engine rate are linearly resampled on the reader
     * thread). Asynchronous: poll [backingTrackState] for READY or ERROR.
     */
    fun openBackingTrack(slot: Int, path: String, loop: Boolean = false) {
        if (handle != 0L) nativeOpenBackingTrack(handle, slot, path, loop)
    }

    fun playBackingTrack(slot: Int) { if (handle != 0L) nativePlayBackingTrack(handle, slot) }
    fun pauseBackingTrack(slot: Int) { if (handle != 0L) nativePauseBackingTrack(handle, slot) }
    fun closeBackingTrack(slot: Int) { if (handle != 0L) nativeCloseBackingTrack(handle, slot) }
    fun setBackingTrackGain(slot: Int, gain: Float) {
        if (handle != 0L) nativeSetBackingTrackGain(handle, slot, gain)
    }

    /** Toggle looping live (no restart, applied at the next wrap). */
    fun setBackingTrackLoop(slot: Int, loop: Boolean) {
        if (handle != 0L) nativeSetBackingTrackLoop(handle, slot, loop)
    }

    fun backingTrackState(slot: Int): BackingTrackState =
        BackingTrackState.fromNative(if (handle != 0L) nativeGetBackingTrackState(handle, slot) else 0)

    fun backingTrackPositionFrames(slot: Int): Long =
        if (handle != 0L) nativeGetBackingTrackPosition(handle, slot) else 0L

    fun backingTrackLengthFrames(slot: Int): Long =
        if (handle != 0L) nativeGetBackingTrackLength(handle, slot) else 0L

    // ------------------------------------------------------------------
    // Session finalization & stem export
    // ------------------------------------------------------------------

    /**
     * Flushes any audio still in the lock-free rings to disk, patches the
     * RIFF/data chunk sizes, and closes the capture file handle. Blocking
     * on the native side, so it runs on [Dispatchers.IO]. Returns false on
     * timeout. Call before zipping/sharing the session's .wav files.
     */
    suspend fun flushAndCloseSession(timeoutMs: Long = 5_000): Boolean =
        withContext(Dispatchers.IO) {
            val h = handle
            h != 0L && nativeFlushAndCloseSession(h, timeoutMs.toInt())
        }

    /**
     * Loads saved stem .wavs back into the loop tracks — the inverse of
     * [exportStems], driven by the persistence layer at startup. [trackPaths]
     * maps engine track index to an absolute .wav path (48 kHz). The native
     * worker fills the buffers and the audio thread adopts loop length +
     * content flags, so completion requires the engine to be started; the
     * transport is frozen until the commit lands. Requires a fresh engine
     * (no loop yet). Returns true once the loop is live.
     */
    suspend fun restoreSession(trackPaths: Map<Int, String>, timeoutMs: Long = 30_000): Boolean {
        val h = handle
        if (h == 0L || trackPaths.isEmpty()) return false
        val entries = trackPaths.entries.toList()
        val indices = IntArray(entries.size) { entries[it].key }
        val paths = Array(entries.size) { entries[it].value }
        if (!nativeRestoreSession(h, indices, paths)) return false
        val finalState = withTimeoutOrNull(timeoutMs) {
            var state = nativeGetRestoreState(h)
            while (state == RESTORE_RUNNING) {
                delay(16)
                state = nativeGetRestoreState(h)
            }
            state
        } ?: return false
        return finalState == RESTORE_DONE
    }

    // ------------------------------------------------------------------
    // Per-pass undo + offline track waveforms
    // ------------------------------------------------------------------

    /**
     * Copies [track]'s current audio into the engine's undo buffer — call
     * right before starting a pass so Backspace can restore the pre-pass
     * state. A multi-MB memcpy: call from a background dispatcher.
     */
    fun snapshotTrackForUndo(track: Int) {
        if (handle != 0L) nativeSnapshotTrackForUndo(handle, track)
    }

    /**
     * Restores the last snapshot (or wipes the take if the pass started on
     * an empty track). Returns true if the undo was handled; false when no
     * snapshot exists (fall back to [clearTrack]). Blocks ~30 ms natively.
     */
    suspend fun undoLastPass(): Boolean = withContext(Dispatchers.IO) {
        val h = handle
        h != 0L && nativeUndoLastPass(h)
    }

    /** Track the pending undo snapshot belongs to, -1 if none. */
    val undoPassTrack: Int
        get() = if (handle != 0L) nativeGetUndoPassTrack(handle) else -1

    /**
     * Silences the first [millis] of a track's loop — cuts a bad start while
     * keeping loop length + sync. Undoable via [undoLastPass]. Blocks ~30 ms
     * natively, so runs on [Dispatchers.IO]. Returns true on success.
     */
    suspend fun trimTrackStart(track: Int, millis: Int): Boolean = withContext(Dispatchers.IO) {
        val h = handle
        val rate = sampleRate
        if (h == 0L || rate <= 0 || millis <= 0) return@withContext false
        val frames = (millis.toLong() * rate / 1000L).toInt()
        nativeTrimTrackStart(h, track, frames)
    }

    // ------------------------------------------------------------------
    // Non-destructive trim: an audible window over the whole take.
    // ------------------------------------------------------------------

    /** Set the audible window (ms). endMs <= 0 means "to the loop end". */
    fun setTrackTrimMillis(track: Int, startMs: Int, endMs: Int) {
        val h = handle
        val rate = sampleRate
        if (h == 0L || rate <= 0) return
        val start = (startMs.toLong() * rate / 1000L).toInt()
        val end = if (endMs <= 0) 0 else (endMs.toLong() * rate / 1000L).toInt()
        nativeSetTrackTrim(h, track, start, end)
    }

    /** Current trim start in ms. */
    fun trackTrimStartMillis(track: Int): Int {
        val h = handle
        val rate = sampleRate
        if (h == 0L || rate <= 0) return 0
        return (nativeGetTrackTrimStart(h, track).toLong() * 1000L / rate).toInt()
    }

    /** Current trim end in ms (resolved to the loop length when unset). */
    fun trackTrimEndMillis(track: Int): Int {
        val h = handle
        val rate = sampleRate
        if (h == 0L || rate <= 0) return 0
        return (nativeGetTrackTrimEnd(h, track).toLong() * 1000L / rate).toInt()
    }

    /** Auto-trim leading/trailing silence (non-destructive). Off-main. */
    suspend fun autoTrimTrack(track: Int) = withContext(Dispatchers.IO) {
        if (handle != 0L) nativeAutoTrimTrack(handle, track)
    }

    /**
     * Commit the track's trim window as the real loop: the shared loop is
     * shortened to the window and every track is cropped to the same region so
     * the layers stay in sync. Discards audio outside the window. Blocks ~40 ms,
     * so it runs off the main thread. Returns true if the loop changed.
     */
    suspend fun applyTrimToLoop(track: Int): Boolean = withContext(Dispatchers.IO) {
        if (handle == 0L) false else nativeApplyTrimToLoop(handle, track)
    }

    // ------------------------------------------------------------------
    // Play modes: per-track loop vs 1-shot, and global Single/Multi.
    // ------------------------------------------------------------------

    /** Loop (false, default) vs 1-shot (true: play once from the top, stop). */
    fun setTrackOneShot(track: Int, oneShot: Boolean) {
        if (handle != 0L) nativeSetTrackOneShot(handle, track, oneShot)
    }

    /** Single (true) = only one track sounds at a time (verse/chorus). */
    fun setSinglePlayMode(single: Boolean) {
        if (handle != 0L) nativeSetPlayMode(handle, if (single) 1 else 0)
    }

    /** Per-track fade in/out (ms) at the audible-window edges (0 = hard edges). */
    fun setTrackFadeMillis(track: Int, millis: Int) {
        val h = handle
        val rate = sampleRate
        if (h == 0L || rate <= 0) return
        nativeSetTrackFade(h, track, (millis.toLong() * rate / 1000L).toInt())
    }

    // ------------------------------------------------------------------
    // Auto-record (hands-free start on an input threshold) + tuner.
    // ------------------------------------------------------------------

    /** Arm [track] to start recording when the input peak crosses [threshold] (0..1). */
    fun armAutoRecord(track: Int, threshold: Float) {
        if (handle != 0L) nativeArmAutoRecord(handle, track, threshold)
    }

    fun cancelAutoRecord() { if (handle != 0L) nativeCancelAutoRecord(handle) }
    fun autoRecordArmed(): Boolean = handle != 0L && nativeAutoRecordArmed(handle)

    /** Enable/disable filling the tuner analysis buffer (call when the tuner UI opens). */
    fun setTunerActive(active: Boolean) { if (handle != 0L) nativeSetTunerActive(handle, active) }

    /** Detected input pitch in Hz, or -1 if there's no clear pitch. */
    fun detectPitchHz(): Float = if (handle != 0L) nativeDetectPitch(handle) else -1f

    /** Manual overdub-timing offset (ms) — nudge by ear when calibration can't run. */
    fun setRecordOffsetMillis(millis: Int) {
        val rate = sampleRate
        if (handle != 0L && rate > 0) {
            setRecordOffsetFrames((millis.toLong() * rate / 1000L).toInt())
        }
    }

    /**
     * Downsampled |peak| bins of a recorded track's loop audio for the
     * offline waveform display, or null if the track is empty. A full-track
     * scan — fetch on content changes, not per frame.
     */
    fun trackWaveform(track: Int, bins: Int = 96): FloatArray? {
        val h = handle
        if (h == 0L || bins <= 0) return null
        val out = FloatArray(bins)
        val n = nativeGetTrackWaveform(h, track, out)
        return when {
            n <= 0 -> null
            n == bins -> out
            else -> out.copyOf(n)
        }
    }

    /**
     * Writes one IEEE-float32 .wav per non-empty loop track into
     * [directory] (created by the caller) on the engine's export worker.
     * Requires the transport to be quiet (playing is fine; recording or
     * clearing is not). Returns the number of stems written, or null on
     * failure/timeout.
     */
    suspend fun exportStems(directory: String, timeoutMs: Long = 60_000): Int? {
        val h = handle
        if (h == 0L || !nativeExportStems(h, directory)) return null
        val finalState = withTimeoutOrNull(timeoutMs) {
            var state = nativeGetExportState(h)
            while (state == EXPORT_RUNNING) {
                delay(16)
                state = nativeGetExportState(h)
            }
            state
        } ?: return null
        return if (finalState == EXPORT_DONE) nativeGetExportedStemCount(h) else null
    }

    // ------------------------------------------------------------------
    // Parameters
    // ------------------------------------------------------------------

    fun selectTrack(track: Int) { if (handle != 0L) nativeSelectTrack(handle, track) }
    fun setTrackGain(track: Int, gain: Float) { if (handle != 0L) nativeSetTrackGain(handle, track, gain) }

    /** Balance-law pan, -1..1; center is unity on both channels. */
    fun setTrackPan(track: Int, pan: Float) { if (handle != 0L) nativeSetTrackPan(handle, track, pan) }
    fun setTrackMuted(track: Int, muted: Boolean) { if (handle != 0L) nativeSetTrackMuted(handle, track, muted) }

    /** Slide a track's start time by [millis] (signed) — instant, glitch-free. */
    fun nudgeTrack(track: Int, millis: Int) {
        val h = handle
        val rate = sampleRate
        if (h == 0L || rate <= 0) return
        val cur = nativeGetTrackShift(h, track)
        nativeSetTrackShift(h, track, cur + (millis.toLong() * rate / 1000L).toInt())
    }

    /** Reset a track's start-shift to zero. */
    fun resetTrackShift(track: Int) { if (handle != 0L) nativeSetTrackShift(handle, track, 0) }

    /** Current start-shift in milliseconds (signed). */
    fun trackShiftMillis(track: Int): Int {
        val h = handle
        val rate = sampleRate
        if (h == 0L || rate <= 0) return 0
        return (nativeGetTrackShift(h, track).toLong() * 1000L / rate).toInt()
    }

    /** Bit t = track t has content, bit (t+16) = track t is clearing. */
    val trackContentMask: Int
        get() = if (handle != 0L) nativeGetTrackContentMask(handle) else 0

    /** Software monitoring level. Default is 0: hardware monitoring through the interface. */
    fun setMonitorGain(gain: Float) { if (handle != 0L) nativeSetMonitorGain(handle, gain) }

    /** Fed by the Phase-4 latency calibration tool. */
    fun setRecordOffsetFrames(frames: Int) { if (handle != 0L) nativeSetRecordOffsetFrames(handle, frames) }

    val state: State get() = State.fromNative(if (handle != 0L) nativeGetState(handle) else 0)
    val sampleRate: Int get() = if (handle != 0L) nativeGetSampleRate(handle) else 0
    val trackCount: Int get() = if (handle != 0L) nativeGetTrackCount(handle) else 0

    // ------------------------------------------------------------------
    // 60 Hz meter polling — drives the live waveform visualizer
    // ------------------------------------------------------------------

    private val pollBuffer = FloatArray(MAX_POINTS_PER_POLL * FLOATS_PER_POINT)
    private val waveformBars = FloatArray(WAVEFORM_BARS)

    private val _meters = MutableStateFlow(
        MeterUiState(FloatArray(WAVEFORM_BARS), null, false, 0, 0L)
    )
    val meters: StateFlow<MeterUiState> = _meters.asStateFlow()

    /**
     * Starts the single polling coroutine. The audio callback publishes one
     * lock-free [MeterPoint] per block (~2-10 ms); this loop drains the queue
     * every ~16 ms and folds the points into the rolling waveform, so no
     * blocks are missed even though the UI only refreshes at 60 Hz.
     */
    fun startMeterPolling(scope: CoroutineScope, periodMillis: Long = 16L): Job =
        scope.launch(Dispatchers.Default) {
            while (isActive) {
                pollOnce()
                delay(periodMillis)
            }
        }

    private fun pollOnce() {
        val h = handle
        if (h == 0L) return

        val points = nativeReadWaveform(h, pollBuffer)
        var latest: MeterPoint? = null
        if (points > 0) {
            // Roll the history left and append one bar per engine block.
            val usable = min(points, WAVEFORM_BARS)
            if (usable < WAVEFORM_BARS) {
                System.arraycopy(waveformBars, usable, waveformBars, 0, WAVEFORM_BARS - usable)
            }
            val firstPoint = points - usable
            for (i in 0 until usable) {
                waveformBars[WAVEFORM_BARS - usable + i] =
                    pollBuffer[(firstPoint + i) * FLOATS_PER_POINT] // inputRms
            }

            val o = (points - 1) * FLOATS_PER_POINT
            latest = MeterPoint(
                inputRms = pollBuffer[o],
                inputPeak = pollBuffer[o + 1],
                mixRms = pollBuffer[o + 2],
                mixPeak = pollBuffer[o + 3],
                playheadFrames = pollBuffer[o + 4].toInt(),
                loopLengthFrames = pollBuffer[o + 5].toInt(),
                state = State.fromNative(pollBuffer[o + 6].toInt()),
            )
        }

        // Beat info decoding must match AudioEngine_JNI.cpp: bit 7 = active,
        // bits 0..6 = beat-in-bar, bits 8+ = monotonic beat count.
        val beat = nativeGetBeatInfo(h)
        _meters.value = MeterUiState(
            waveform = waveformBars.copyOf(),
            latest = latest ?: _meters.value.latest,
            metronomeActive = (beat and 0x80L) != 0L,
            beatInBar = (beat and 0x7FL).toInt(),
            beatCount = beat ushr 8,
            countInBeatsRemaining = nativeGetCountInBeats(h),
        )
    }

    companion object {
        init {
            System.loadLibrary("looperengine")
        }

        /** Must match kFloatsPerPoint in AudioEngine_JNI.cpp. */
        private const val FLOATS_PER_POINT = 7

        /** Must match kMaxPointsPerPoll in AudioEngine_JNI.cpp. */
        private const val MAX_POINTS_PER_POLL = 128

        /** Bars kept in the rolling waveform history. */
        private const val WAVEFORM_BARS = 512

        /** Must match kMaxBackingStreams in DiskSpooler.h. */
        const val BACKING_STREAM_SLOTS = 2

        /** LatencyCalibrator::State values. */
        private const val CAL_RUNNING = 1
        private const val CAL_SUCCEEDED = 2

        /** AudioEngine::kExport* values. */
        private const val EXPORT_RUNNING = 1
        private const val EXPORT_DONE = 2

        /** AudioEngine::kRestore* values. */
        private const val RESTORE_RUNNING = 1
        private const val RESTORE_DONE = 2

        /**
         * Allocates a capture file in app-private storage — no runtime
         * storage permission needed. Pass the returned absolute path to
         * [startCapture].
         */
        fun newCaptureFile(context: Context): File {
            val dir = File(context.filesDir, "takes").apply { mkdirs() }
            return File(dir, "take_${System.currentTimeMillis()}.wav")
        }

        /**
         * Creates an engine sized for the device class. Track memory is
         * `tracks * seconds * 48000 * 2ch * 4B`, so a Tab S9 Ultra-class
         * tablet gets 8 tracks x 120 s (~368 MB) while a small phone stays
         * at 4 x 30 s (~46 MB). The UI itself adapts via window size classes
         * in the Phase 3 Compose layer.
         */
        fun create(
            context: Context,
            inputDeviceId: Int = 0,   // 0 = system default; set from AudioManager device
            outputDeviceId: Int = 0,  // enumeration when targeting the USB interface
        ): AudioEngine {
            val isTablet = context.resources.configuration.smallestScreenWidthDp >= 600
            val activityManager =
                context.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
            val largeHeap = activityManager.memoryClass >= 384 // MB per-app budget

            val (tracks, loopSeconds) = when {
                isTablet && largeHeap -> 8 to 120 // Galaxy Tab S9 Ultra class
                isTablet -> 6 to 60
                else -> 4 to 30
            }

            val handle = nativeCreate(
                sampleRate = 48_000,
                channelCount = 2,
                trackCount = tracks,
                maxLoopSeconds = loopSeconds,
                lookAheadMillis = 15,
                driftSlackMillis = 5,
                inputDeviceId = inputDeviceId,
                outputDeviceId = outputDeviceId,
            )
            check(handle != 0L) { "native engine creation failed" }
            return AudioEngine(handle)
        }

        // ---------------- native declarations ----------------

        @JvmStatic private external fun nativeCreate(
            sampleRate: Int,
            channelCount: Int,
            trackCount: Int,
            maxLoopSeconds: Int,
            lookAheadMillis: Int,
            driftSlackMillis: Int,
            inputDeviceId: Int,
            outputDeviceId: Int,
        ): Long
    }

    private external fun nativeDestroy(handle: Long)
    private external fun nativeStart(handle: Long): Boolean
    private external fun nativeStop(handle: Long)
    private external fun nativeSetOutputDevice(handle: Long, deviceId: Int): Boolean
    private external fun nativeSetInputDevice(handle: Long, deviceId: Int): Boolean
    private external fun nativeGetOutputDevice(handle: Long): Int
    private external fun nativeSetEventListener(handle: Long, listener: Any?)

    private external fun nativeStartCalibration(handle: Long)
    private external fun nativeCancelCalibration(handle: Long)
    private external fun nativeGetCalibrationState(handle: Long): Int
    private external fun nativeGetCalibratedLatencyFrames(handle: Long): Int

    private external fun nativeStartRecording(handle: Long)
    private external fun nativeStopRecording(handle: Long)
    private external fun nativeStartPlayback(handle: Long)
    private external fun nativeStopPlayback(handle: Long)
    private external fun nativeRecordTrack(handle: Long, track: Int)
    private external fun nativePlayTrack(handle: Long, track: Int)
    private external fun nativeStopTrack(handle: Long, track: Int)
    private external fun nativePlayAll(handle: Long)
    private external fun nativeStopAll(handle: Long)
    private external fun nativeGetTrackTransport(handle: Long, track: Int): Int
    private external fun nativeGetTrackPosition(handle: Long, track: Int): Int
    private external fun nativeClearAll(handle: Long)
    private external fun nativeClearTrack(handle: Long, track: Int)
    private external fun nativeResetLoopForRestore(handle: Long)

    private external fun nativeSetMetronomeState(
        handle: Long,
        isActive: Boolean,
        bpm: Float,
        beatsPerMeasure: Int,
    )
    private external fun nativeSetMetronomeGain(handle: Long, gain: Float)
    private external fun nativeSetMetronomeStyle(handle: Long, style: Int)
    private external fun nativeSetMetronomeSync(handle: Long, enabled: Boolean)
    private external fun nativeSetCountInEnabled(handle: Long, enabled: Boolean)
    private external fun nativeSetCountInBars(handle: Long, bars: Int)
    private external fun nativeSetLoopQuantize(handle: Long, enabled: Boolean)
    private external fun nativeSetInputChannels(handle: Long, channels: Int, mapLeft: Int, mapRight: Int)
    private external fun nativeGetBeatInfo(handle: Long): Long
    private external fun nativeGetCountInBeats(handle: Long): Int

    private external fun nativeSetReverbRoomSize(handle: Long, size: Float)
    private external fun nativeSetReverbMix(handle: Long, mix: Float)
    private external fun nativeSetReverbDamping(handle: Long, damping: Float)
    private external fun nativeSetFilterEnabled(handle: Long, enabled: Boolean)
    private external fun nativeSetFilterCutoff(handle: Long, hz: Float)
    private external fun nativeSetFilterResonance(handle: Long, r: Float)
    private external fun nativeSetFilterMode(handle: Long, mode: Int)
    private external fun nativeSetDroneEnabled(handle: Long, enabled: Boolean)
    private external fun nativeSetDroneFrequency(handle: Long, hz: Float)
    private external fun nativeSetDroneGain(handle: Long, gain: Float)
    private external fun nativeSetReverbEnabled(handle: Long, enabled: Boolean)

    private external fun nativeStartCapture(handle: Long, path: String, captureMix: Boolean)
    private external fun nativeStopCapture(handle: Long)
    private external fun nativeIsCapturing(handle: Long): Boolean
    private external fun nativeGetCapturedFrames(handle: Long): Long
    private external fun nativeGetCaptureDroppedFrames(handle: Long): Long

    private external fun nativeFlushAndCloseSession(handle: Long, timeoutMillis: Int): Boolean
    private external fun nativeExportStems(handle: Long, directory: String): Boolean
    private external fun nativeGetExportState(handle: Long): Int
    private external fun nativeGetExportedStemCount(handle: Long): Int
    private external fun nativeRestoreSession(
        handle: Long,
        trackIndices: IntArray,
        paths: Array<String>,
    ): Boolean
    private external fun nativeGetRestoreState(handle: Long): Int

    private external fun nativeSnapshotTrackForUndo(handle: Long, track: Int)
    private external fun nativeUndoLastPass(handle: Long): Boolean
    private external fun nativeGetUndoPassTrack(handle: Long): Int
    private external fun nativeTrimTrackStart(handle: Long, track: Int, frames: Int): Boolean
    private external fun nativeSetTrackTrim(handle: Long, track: Int, startFrames: Int, endFrames: Int)
    private external fun nativeGetTrackTrimStart(handle: Long, track: Int): Int
    private external fun nativeGetTrackTrimEnd(handle: Long, track: Int): Int
    private external fun nativeAutoTrimTrack(handle: Long, track: Int)
    private external fun nativeApplyTrimToLoop(handle: Long, track: Int): Boolean
    private external fun nativeSetTrackOneShot(handle: Long, track: Int, oneShot: Boolean)
    private external fun nativeSetPlayMode(handle: Long, mode: Int)
    private external fun nativeSetTrackFade(handle: Long, track: Int, frames: Int)
    private external fun nativeArmAutoRecord(handle: Long, track: Int, threshold: Float)
    private external fun nativeCancelAutoRecord(handle: Long)
    private external fun nativeAutoRecordArmed(handle: Long): Boolean
    private external fun nativeSetTunerActive(handle: Long, active: Boolean)
    private external fun nativeDetectPitch(handle: Long): Float
    private external fun nativeGetTrackWaveform(handle: Long, track: Int, dest: FloatArray): Int

    private external fun nativeOpenBackingTrack(handle: Long, slot: Int, path: String, loop: Boolean)
    private external fun nativePlayBackingTrack(handle: Long, slot: Int)
    private external fun nativePauseBackingTrack(handle: Long, slot: Int)
    private external fun nativeCloseBackingTrack(handle: Long, slot: Int)
    private external fun nativeSetBackingTrackGain(handle: Long, slot: Int, gain: Float)
    private external fun nativeSetBackingTrackLoop(handle: Long, slot: Int, loop: Boolean)
    private external fun nativeGetBackingTrackState(handle: Long, slot: Int): Int
    private external fun nativeGetBackingTrackPosition(handle: Long, slot: Int): Long
    private external fun nativeGetBackingTrackLength(handle: Long, slot: Int): Long

    private external fun nativeSelectTrack(handle: Long, track: Int)
    private external fun nativeSetTrackGain(handle: Long, track: Int, gain: Float)
    private external fun nativeSetTrackPan(handle: Long, track: Int, pan: Float)
    private external fun nativeSetTrackMuted(handle: Long, track: Int, muted: Boolean)
    private external fun nativeSetTrackShift(handle: Long, track: Int, frames: Int)
    private external fun nativeGetTrackShift(handle: Long, track: Int): Int
    private external fun nativeGetTrackContentMask(handle: Long): Int
    private external fun nativeSetMonitorGain(handle: Long, gain: Float)
    private external fun nativeSetRecordOffsetFrames(handle: Long, frames: Int)

    private external fun nativeReadWaveform(handle: Long, dest: FloatArray): Int
    private external fun nativeGetState(handle: Long): Int
    private external fun nativeGetSampleRate(handle: Long): Int
    private external fun nativeGetLoopLengthFrames(handle: Long): Int
    private external fun nativeGetPlayheadFrames(handle: Long): Int
    private external fun nativeGetTrackCount(handle: Long): Int
}
