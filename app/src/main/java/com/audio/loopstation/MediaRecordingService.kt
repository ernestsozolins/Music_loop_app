package com.audio.loopstation

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.content.pm.ServiceInfo
import android.media.AudioAttributes
import android.media.AudioFocusRequest
import android.media.AudioManager
import android.os.Binder
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import androidx.core.app.NotificationCompat
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch

/**
 * Owns the native [AudioEngine] for the whole app.
 *
 * Lifecycle design:
 *  - The UI BINDS (never owns): MainActivity binds in onStart and unbinds in
 *    onStop, so configuration changes and backgrounding detach the UI
 *    without ever touching the C++ audio thread.
 *  - The service promotes itself to a FOREGROUND service (microphone type,
 *    ongoing notification with a Stop action) whenever the transport is
 *    active — and also while loops are held in memory or a take is being
 *    captured, because demoting then would let the OS kill the process and
 *    silently destroy the session.
 *  - AUDIO FOCUS: focus is requested when the transport starts. On
 *    AUDIOFOCUS_LOSS / LOSS_TRANSIENT (e.g. an incoming call) the engine is
 *    paused immediately over its lock-free JNI hooks — the take is
 *    finalized on disk BEFORE the streams are closed, so nothing recorded
 *    is ever lost — and the mic is released to telephony. After a
 *    transient loss, playback (never recording) resumes when focus returns.
 */
class MediaRecordingService : Service() {

    inner class LocalBinder : Binder() {
        fun getService(): MediaRecordingService = this@MediaRecordingService
    }

    private val binder = LocalBinder()
    private val serviceScope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)

    /** The engine the bound UI attaches its ViewModel to. */
    lateinit var engine: AudioEngine
        private set

    private lateinit var audioManager: AudioManager
    private var focusRequest: AudioFocusRequest? = null

    private enum class FocusState { NONE, HELD, LOST_TRANSIENT }
    private var focusState = FocusState.NONE
    private var resumePlaybackOnGain = false

    private var engineStarted = false
    private var isForeground = false
    private var lastState = AudioEngine.State.IDLE

    // ------------------------------------------------------------------
    // Service lifecycle
    // ------------------------------------------------------------------

    override fun onCreate() {
        super.onCreate()
        audioManager = getSystemService(AUDIO_SERVICE) as AudioManager
        engine = AudioEngine.create(applicationContext)
        // The meter poll lives here, parented to the service scope — exactly
        // one poller for the engine's single-consumer meter queue, running
        // for as long as the engine exists.
        engine.startMeterPolling(serviceScope)
        createNotificationChannel()
        serviceScope.launch { engine.meters.collect { onTransportTick() } }
    }

    override fun onBind(intent: Intent?): IBinder = binder

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            serviceScope.launch { stopTransportAndFinalize() }
        }
        // Never auto-restart a recorder without user intent.
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        abandonFocus()
        serviceScope.cancel()
        engine.release()
        super.onDestroy()
    }

    /** Called by the UI once RECORD_AUDIO is granted. Idempotent. */
    fun ensureEngineStarted(): Boolean {
        if (!engineStarted) engineStarted = engine.start()
        return engineStarted
    }

    // ------------------------------------------------------------------
    // Transport observation -> focus + foreground management
    // ------------------------------------------------------------------

    private fun onTransportTick() {
        val state = engine.meters.value.latest?.state ?: lastState
        val transportActive = state == AudioEngine.State.RECORDING_MASTER ||
            state == AudioEngine.State.OVERDUBBING ||
            state == AudioEngine.State.PLAYING
        val sessionAlive = (engine.trackContentMask and 0xFFFF) != 0 || engine.isCapturing

        if (transportActive && focusState == FocusState.NONE) requestFocus()
        if (!transportActive && focusState == FocusState.HELD) abandonFocus()

        val shouldBeForeground = transportActive || sessionAlive
        when {
            shouldBeForeground && !isForeground -> promoteToForeground(statusText(state))
            !shouldBeForeground && isForeground -> demoteFromForeground()
            isForeground && state != lastState -> updateNotification(statusText(state))
        }
        lastState = state
    }

    // ------------------------------------------------------------------
    // Audio focus (CRITICAL: protects the recording from phone calls)
    // ------------------------------------------------------------------

    private val focusListener = AudioManager.OnAudioFocusChangeListener { change ->
        when (change) {
            AudioManager.AUDIOFOCUS_LOSS -> onFocusLost(permanent = true)
            AudioManager.AUDIOFOCUS_LOSS_TRANSIENT -> onFocusLost(permanent = false)
            AudioManager.AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK -> {
                // Navigation prompts / notification beeps: a live looper must
                // neither duck nor pause for these. Deliberate no-op.
            }
            AudioManager.AUDIOFOCUS_GAIN -> onFocusRegained()
        }
    }

    private fun requestFocus() {
        val attributes = AudioAttributes.Builder()
            .setUsage(AudioAttributes.USAGE_MEDIA)
            .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
            .build()
        val request = AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
            .setAudioAttributes(attributes)
            .setWillPauseWhenDucked(false)  // CAN_DUCK handled (ignored) above
            .setOnAudioFocusChangeListener(focusListener, Handler(Looper.getMainLooper()))
            .build()
        focusRequest = request
        val granted = audioManager.requestAudioFocus(request)
        focusState = if (granted == AudioManager.AUDIOFOCUS_REQUEST_GRANTED) {
            FocusState.HELD
        } else {
            FocusState.NONE
        }
    }

    private fun abandonFocus() {
        focusRequest?.let { audioManager.abandonAudioFocusRequest(it) }
        focusRequest = null
        focusState = FocusState.NONE
    }

    private fun onFocusLost(permanent: Boolean) {
        // Resume playback after a call ends — but never auto-resume RECORDING;
        // re-arming a mic without explicit user intent is wrong.
        resumePlaybackOnGain = !permanent &&
            (lastState == AudioEngine.State.PLAYING || lastState == AudioEngine.State.OVERDUBBING)
        focusState = if (permanent) FocusState.NONE else FocusState.LOST_TRANSIENT
        if (permanent) abandonFocus()

        serviceScope.launch {
            // ORDER MATTERS: park the transport, get the take safely onto
            // disk (ring drained, RIFF header patched), THEN release the
            // hardware streams to telephony.
            engine.stopRecording()
            engine.stopPlayback()
            engine.flushAndCloseSession()
            engine.stop()
            engineStarted = false
            if (isForeground) {
                updateNotification(if (permanent) "Stopped — audio focus lost" else "Paused — phone call")
            }
        }
    }

    private fun onFocusRegained() {
        focusState = FocusState.HELD
        serviceScope.launch {
            if (!engineStarted) engineStarted = engine.start()
            if (resumePlaybackOnGain && engineStarted) engine.startPlayback()
            resumePlaybackOnGain = false
        }
    }

    // ------------------------------------------------------------------
    // Foreground promotion & notification
    // ------------------------------------------------------------------

    private fun promoteToForeground(text: String) {
        // Being merely bound is not enough to survive the UI unbinding —
        // become a *started* service first, then go foreground.
        startService(Intent(this, MediaRecordingService::class.java))
        val notification = buildNotification(text)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(NOTIFICATION_ID, notification,
                ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE)
        } else {
            startForeground(NOTIFICATION_ID, notification)
        }
        isForeground = true
    }

    private fun demoteFromForeground() {
        stopForeground(STOP_FOREGROUND_REMOVE)
        stopSelf()  // stays alive while the UI is bound; dies with it otherwise
        isForeground = false
    }

    private suspend fun stopTransportAndFinalize() {
        engine.stopRecording()
        engine.stopPlayback()
        engine.flushAndCloseSession()
        // Foreground/focus demotion follows automatically via onTransportTick.
    }

    private fun statusText(state: AudioEngine.State): String = when (state) {
        AudioEngine.State.RECORDING_MASTER -> "Recording loop…"
        AudioEngine.State.OVERDUBBING -> "Overdubbing…"
        AudioEngine.State.PLAYING -> "Playing"
        else -> "Session on standby — loops held in memory"
    }

    private fun buildNotification(text: String): Notification {
        val openApp = PendingIntent.getActivity(
            this, 0,
            Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE,
        )
        val stop = PendingIntent.getService(
            this, 1,
            Intent(this, MediaRecordingService::class.java).setAction(ACTION_STOP),
            PendingIntent.FLAG_IMMUTABLE,
        )
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(android.R.drawable.ic_btn_speak_now)
            .setContentTitle("Loop Station")
            .setContentText(text)
            .setContentIntent(openApp)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .setCategory(NotificationCompat.CATEGORY_SERVICE)
            .addAction(0, "Stop", stop)
            .build()
    }

    private fun updateNotification(text: String) {
        (getSystemService(NOTIFICATION_SERVICE) as NotificationManager)
            .notify(NOTIFICATION_ID, buildNotification(text))
    }

    private fun createNotificationChannel() {
        val channel = NotificationChannel(
            CHANNEL_ID, "Recording session", NotificationManager.IMPORTANCE_LOW,
        ).apply { description = "Shown while the looper is recording or holding a session" }
        (getSystemService(NOTIFICATION_SERVICE) as NotificationManager)
            .createNotificationChannel(channel)
    }

    companion object {
        private const val CHANNEL_ID = "recording_session"
        private const val NOTIFICATION_ID = 1
        const val ACTION_STOP = "com.audio.loopstation.action.STOP"
    }
}
