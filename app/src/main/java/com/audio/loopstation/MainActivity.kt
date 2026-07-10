package com.audio.loopstation

import android.Manifest
import android.content.pm.PackageManager
import android.os.Bundle
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

class MainActivity : ComponentActivity() {

    private val viewModel: MainViewModel by viewModels()

    private val requestMicPermission =
        registerForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
            if (granted) viewModel.onAudioPermissionGranted()
            // Denied: the UI still renders; transport does nothing until the
            // user grants mic access from settings and the engine starts.
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

        if (ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO) ==
            PackageManager.PERMISSION_GRANTED
        ) {
            viewModel.onAudioPermissionGranted()
        } else {
            requestMicPermission.launch(Manifest.permission.RECORD_AUDIO)
        }

        setContent {
            // Dark, stage-friendly Material 3 theme.
            MaterialTheme(colorScheme = darkColorScheme()) {
                Surface { MainScreen(viewModel) }
            }
        }
    }
}
