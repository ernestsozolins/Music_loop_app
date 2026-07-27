package com.audio.loopstation.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.audio.loopstation.ui.MainViewModel.TrackUiState
import com.audio.loopstation.ui.MainViewModel.TransportUiState

/**
 * FOOT MODE — the whole screen becomes three pedal-sized targets so the tablet
 * can be played on the floor with bare feet or shoes, no hardware pedal.
 *
 * Layout (landscape, the way a tablet sits on the floor):
 *
 *   +-------------------------------+-------------+
 *   |                               |    STOP     |
 *   |   REC / OVERDUB  (60% wide)   +-------------+
 *   |                               |    UNDO     |
 *   +-------------------------------+-------------+
 *
 * The big button is the same one-button loop workflow as SmartLoopButton
 * (record -> close -> overdub -> next track), so nothing new has to be
 * learned. A toe is ~35 mm wide and a foot lands imprecisely, so every target
 * is a large fraction of the screen rather than a fixed dp size, and the two
 * secondary actions are stacked away from the primary one to make a mis-stomp
 * land on nothing rather than on the wrong action. The count-in number and the
 * state label are rendered huge because the screen is ~1 m from the eye.
 */
@Composable
fun FootPedalScreen(
    transport: TransportUiState,
    tracks: List<TrackUiState>,
    onLoopTap: () -> Unit,
    onStopTap: () -> Unit,
    onUndoTap: () -> Unit,
    onExit: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val recording = transport.isRecording || transport.isCountingIn
    Surface(modifier = modifier.fillMaxSize()) {
        Column(Modifier.fillMaxSize()) {
            // Slim status strip: which track is armed, loop length, exit.
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 16.dp, vertical = 6.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                val armed = tracks.firstOrNull { it.isSelected }
                Text(
                    text = buildString {
                        append(armed?.name ?: "Track 1")
                        if (transport.hasLoop) {
                            append("  ·  ${transport.loopBars} bar")
                            if (transport.loopBars != 1) append("s")
                        }
                        append("  ·  ${transport.bpm} BPM")
                    },
                    style = MaterialTheme.typography.titleMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Box(Modifier.weight(1f))
                // Small on purpose: an accidental toe must not drop foot mode.
                TextButton(onClick = onExit) { Text("Exit foot mode") }
            }

            Row(Modifier.fillMaxSize()) {
                // ---- Primary: record / close / overdub ----
                Button(
                    onClick = onLoopTap,
                    enabled = transport.engineReady,
                    shape = RoundedCornerShape(28.dp),
                    colors = ButtonDefaults.buttonColors(
                        containerColor = if (recording) {
                            MaterialTheme.colorScheme.error
                        } else {
                            MaterialTheme.colorScheme.primary
                        },
                    ),
                    modifier = Modifier
                        .weight(0.62f)
                        .fillMaxHeight()
                        .padding(8.dp)
                        .semantics { contentDescription = transport.loopButtonLabel },
                ) {
                    if (transport.isCountingIn && transport.countInBeatsRemaining > 0) {
                        // Count-in: nothing but the number, as large as it fits.
                        Text(
                            text = transport.countInBeatsRemaining.toString(),
                            fontSize = 160.sp,
                            fontWeight = FontWeight.Bold,
                            textAlign = TextAlign.Center,
                        )
                    } else {
                        Text(
                            text = footLabel(transport),
                            fontSize = 44.sp,
                            fontWeight = FontWeight.Bold,
                            textAlign = TextAlign.Center,
                            lineHeight = 52.sp,
                        )
                    }
                }

                // ---- Secondary: stop / undo, stacked ----
                Column(
                    modifier = Modifier
                        .weight(0.38f)
                        .fillMaxHeight(),
                    verticalArrangement = Arrangement.spacedBy(0.dp),
                ) {
                    Button(
                        onClick = onStopTap,
                        shape = RoundedCornerShape(28.dp),
                        colors = ButtonDefaults.buttonColors(
                            containerColor = MaterialTheme.colorScheme.secondaryContainer,
                            contentColor = MaterialTheme.colorScheme.onSecondaryContainer,
                        ),
                        modifier = Modifier
                            .fillMaxWidth()
                            .weight(1f)
                            .padding(8.dp)
                            .semantics { contentDescription = "Stop all" },
                    ) {
                        Text("STOP", fontSize = 40.sp, fontWeight = FontWeight.Bold)
                    }
                    Button(
                        onClick = onUndoTap,
                        shape = RoundedCornerShape(28.dp),
                        colors = ButtonDefaults.buttonColors(
                            containerColor = MaterialTheme.colorScheme.tertiaryContainer,
                            contentColor = MaterialTheme.colorScheme.onTertiaryContainer,
                        ),
                        modifier = Modifier
                            .fillMaxWidth()
                            .weight(1f)
                            .padding(8.dp)
                            .semantics { contentDescription = "Undo last pass" },
                    ) {
                        Text("UNDO", fontSize = 40.sp, fontWeight = FontWeight.Bold)
                    }
                }
            }
        }
    }
}

/** Short, high-contrast wording — read at arm's length, not on a phone. */
private fun footLabel(t: TransportUiState): String = when {
    t.isCountingIn -> "GET READY"
    t.engineState == com.audio.loopstation.AudioEngine.State.RECORDING_MASTER ->
        "RECORDING\ntap to close"
    t.engineState == com.audio.loopstation.AudioEngine.State.OVERDUBBING ->
        "OVERDUBBING\ntap to end"
    !t.hasLoop -> "RECORD"
    else -> "OVERDUB"
}
