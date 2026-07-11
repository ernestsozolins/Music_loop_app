package com.audio.loopstation

import android.Manifest
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.os.IBinder
import android.view.KeyEvent
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.viewModels
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.darkColorScheme
import androidx.core.content.ContextCompat
import com.audio.loopstation.ui.MainScreen
import com.audio.loopstation.ui.MainViewModel
import com.audio.loopstation.ui.PedalController

/**
 * Thin UI shell. The audio engine lives in [MediaRecordingService]; this
 * activity only BINDS to it (onStart/onStop), attaching the engine to the
 * ViewModel while visible and detaching when backgrounded — the C++ audio
 * thread is never created or destroyed by UI lifecycle events. If the
 * transport is active (or loops are in memory), the service is foregrounded
 * and survives the unbind; otherwise it winds down with the UI.
 */
class MainActivity : ComponentActivity() {

    private val viewModel: MainViewModel by viewModels()
    private val pedal by lazy { PedalController(viewModel) }

    private var recordingService: MediaRecordingService? = null
    private var micPermissionGranted = false

    private val serviceConnection = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName, binder: IBinder) {
            val service = (binder as MediaRecordingService.LocalBinder).getService()
            recordingService = service
            // Reattach is seamless: the service-owned engine (and any loops
            // or in-flight recording) is exactly where we left it.
            viewModel.attachEngine(service.engine)
            maybeStartEngine()
        }

        override fun onServiceDisconnected(name: ComponentName) {
            // Only called if the service process crashed.
            recordingService = null
            viewModel.detachEngine()
        }
    }

    private val requestPermissions =
        registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { results ->
            micPermissionGranted = results[Manifest.permission.RECORD_AUDIO] == true ||
                hasPermission(Manifest.permission.RECORD_AUDIO)
            maybeStartEngine()
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // WAKE LOCK: a looper is played hands-on-instrument for long
        // stretches without touching the screen — never let it sleep while
        // this activity is in the foreground. FLAG_KEEP_SCREEN_ON is
        // self-releasing (cleared when the activity loses the foreground),
        // unlike a PowerManager wake lock, so it can't drain the battery
        // from the background.
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        micPermissionGranted = hasPermission(Manifest.permission.RECORD_AUDIO)
        val wanted = buildList {
            if (!micPermissionGranted) add(Manifest.permission.RECORD_AUDIO)
            // The foreground-service notification needs this on API 33+.
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
                !hasPermission(Manifest.permission.POST_NOTIFICATIONS)
            ) {
                add(Manifest.permission.POST_NOTIFICATIONS)
            }
        }
        if (wanted.isNotEmpty()) requestPermissions.launch(wanted.toTypedArray())

        setContent {
            // Dark, stage-friendly Material 3 theme.
            MaterialTheme(colorScheme = darkColorScheme()) {
                Surface { MainScreen(viewModel) }
            }
        }
    }

    /**
     * Bluetooth foot pedals arrive as HID keyboards; intercepting here (not
     * via a Compose focus modifier) keeps the pedal alive regardless of
     * on-screen focus. Unhandled keys fall through to the framework.
     */
    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean =
        pedal.onKeyDown(keyCode, event) || super.onKeyDown(keyCode, event)

    override fun onStart() {
        super.onStart()
        bindService(
            Intent(this, MediaRecordingService::class.java),
            serviceConnection,
            Context.BIND_AUTO_CREATE,
        )
    }

    override fun onStop() {
        // Detach the UI. The service keeps the engine (and the recording)
        // alive on its own if it has promoted itself to the foreground.
        viewModel.detachEngine()
        recordingService = null
        unbindService(serviceConnection)
        super.onStop()
    }

    /** Streams start only when both the service and the mic permission exist. */
    private fun maybeStartEngine() {
        val service = recordingService ?: return
        if (micPermissionGranted) {
            viewModel.onEngineReady(service.ensureEngineStarted())
        }
    }

    private fun hasPermission(permission: String): Boolean =
        ContextCompat.checkSelfPermission(this, permission) == PackageManager.PERMISSION_GRANTED
}
