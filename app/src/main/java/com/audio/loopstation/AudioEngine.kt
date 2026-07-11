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
        IDLE, RECORDING_MASTER, PLAYING, OVERDUBBING, STOPPED;

        companion object {
            fun fromNative(ordinal: Int): State = entries.getOrElse(ordinal) { IDLE }
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

    /** (Re)starts playback from the top of the loop. */
    fun startPlayback() { if (handle != 0L) nativeStartPlayback(handle) }

    /** Halts the transport; cancels a master recording in progress. */
    fun stopPlayback() { if (handle != 0L) nativeStopPlayback(handle) }

    fun clearAll() { if (handle != 0L) nativeClearAll(handle) }
    fun clearTrack(track: Int) { if (handle != 0L) nativeClearTrack(handle, track) }

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

    /**
     * Phase-lock the click to the loop (default ON in the engine): while a
     * loop plays or is overdubbed, the loop start is bar 1 beat 1 and the
     * beat grid re-anchors at every loop wrap. Disable for a free-running
     * click that ignores the loop.
     */
    fun setMetronomeSyncToLoop(enabled: Boolean) {
        if (handle != 0L) nativeSetMetronomeSync(handle, enabled)
    }

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
     * Requirements: 48 kHz, mono or stereo, 16-bit PCM or 32-bit float.
     * Asynchronous: poll [backingTrackState] for READY or ERROR.
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
    private external fun nativeSetEventListener(handle: Long, listener: Any?)

    private external fun nativeStartCalibration(handle: Long)
    private external fun nativeCancelCalibration(handle: Long)
    private external fun nativeGetCalibrationState(handle: Long): Int
    private external fun nativeGetCalibratedLatencyFrames(handle: Long): Int

    private external fun nativeStartRecording(handle: Long)
    private external fun nativeStopRecording(handle: Long)
    private external fun nativeStartPlayback(handle: Long)
    private external fun nativeStopPlayback(handle: Long)
    private external fun nativeClearAll(handle: Long)
    private external fun nativeClearTrack(handle: Long, track: Int)

    private external fun nativeSetMetronomeState(
        handle: Long,
        isActive: Boolean,
        bpm: Float,
        beatsPerMeasure: Int,
    )
    private external fun nativeSetMetronomeGain(handle: Long, gain: Float)
    private external fun nativeSetMetronomeSync(handle: Long, enabled: Boolean)
    private external fun nativeGetBeatInfo(handle: Long): Long

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

    private external fun nativeOpenBackingTrack(handle: Long, slot: Int, path: String, loop: Boolean)
    private external fun nativePlayBackingTrack(handle: Long, slot: Int)
    private external fun nativePauseBackingTrack(handle: Long, slot: Int)
    private external fun nativeCloseBackingTrack(handle: Long, slot: Int)
    private external fun nativeSetBackingTrackGain(handle: Long, slot: Int, gain: Float)
    private external fun nativeGetBackingTrackState(handle: Long, slot: Int): Int
    private external fun nativeGetBackingTrackPosition(handle: Long, slot: Int): Long
    private external fun nativeGetBackingTrackLength(handle: Long, slot: Int): Long

    private external fun nativeSelectTrack(handle: Long, track: Int)
    private external fun nativeSetTrackGain(handle: Long, track: Int, gain: Float)
    private external fun nativeSetTrackPan(handle: Long, track: Int, pan: Float)
    private external fun nativeSetTrackMuted(handle: Long, track: Int, muted: Boolean)
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
