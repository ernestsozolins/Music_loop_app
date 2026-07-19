package com.audio.loopstation.ui

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import com.audio.loopstation.ui.MainViewModel.TransportUiState

/**
 * The whole loop workflow on one oversized button — the primary control for
 * tablet use WITHOUT a foot pedal. One tap records the master loop, the next
 * closes it, then taps toggle overdub passes (each ending advances to the
 * next track). The label always says what the next tap will do, and the
 * button turns red while a pass is live so it reads at a glance on stage.
 */
@Composable
fun SmartLoopButton(
    transport: TransportUiState,
    onTap: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val recording = transport.isRecording || transport.isCountingIn
    Button(
        onClick = onTap,
        modifier = modifier
            .fillMaxWidth()
            .height(96.dp)
            .padding(horizontal = 12.dp),
        shape = RoundedCornerShape(20.dp),
        colors = ButtonDefaults.buttonColors(
            containerColor = if (recording) {
                MaterialTheme.colorScheme.error
            } else {
                MaterialTheme.colorScheme.primary
            },
        ),
        enabled = transport.engineReady,
    ) {
        Box(contentAlignment = Alignment.Center) {
            if (transport.isCountingIn && transport.countInBeatsRemaining > 0) {
                // A big count-down number so the performer knows exactly when
                // to come in — recording begins as it hits 0 on the downbeat.
                Text(
                    text = transport.countInBeatsRemaining.toString(),
                    style = MaterialTheme.typography.displayMedium,
                    textAlign = TextAlign.Center,
                )
            } else {
                Text(
                    text = transport.loopButtonLabel,
                    style = MaterialTheme.typography.headlineSmall,
                    textAlign = TextAlign.Center,
                )
            }
        }
    }
}
