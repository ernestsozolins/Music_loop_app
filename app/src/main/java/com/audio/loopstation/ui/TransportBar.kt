package com.audio.loopstation.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Share
import androidx.compose.material3.FilledIconButton
import androidx.compose.material3.FilledIconToggleButton
import androidx.compose.material3.FilledTonalIconButton
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButtonDefaults
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import com.audio.loopstation.ui.MainViewModel.TransportUiState

/**
 * Fixed transport strip (hosted in the Scaffold's bottomBar so it never
 * scrolls away): Record / Play / Stop / metronome toggle, BPM adjuster, and
 * Export. All controls are >= 64 dp touch targets per Material 3 ergonomics
 * for stage use. A thin progress line on top shows the playhead within the
 * loop.
 */
@Composable
fun TransportBar(
    transport: TransportUiState,
    onRecordTap: () -> Unit,
    onPlayTap: () -> Unit,
    onStopTap: () -> Unit,
    onMetronomeToggle: () -> Unit,
    onBpmChange: (Int) -> Unit,
    onExportTap: () -> Unit,
    modifier: Modifier = Modifier,
) {
    Surface(modifier = modifier, tonalElevation = 3.dp) {
        Column {
            LinearProgressIndicator(
                progress = { transport.positionFraction },
                modifier = Modifier.fillMaxWidth(),
            )
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 12.dp, vertical = 10.dp),
                horizontalArrangement = Arrangement.SpaceEvenly,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                // Record: red while a take/overdub is running.
                FilledIconButton(
                    onClick = onRecordTap,
                    modifier = Modifier.size(TRANSPORT_BUTTON),
                    colors = IconButtonDefaults.filledIconButtonColors(
                        containerColor = if (transport.isRecording) {
                            MaterialTheme.colorScheme.error
                        } else {
                            MaterialTheme.colorScheme.errorContainer
                        },
                    ),
                ) {
                    RecordGlyph(
                        color = if (transport.isRecording) {
                            MaterialTheme.colorScheme.onError
                        } else {
                            MaterialTheme.colorScheme.error
                        },
                    )
                }

                FilledTonalIconButton(
                    onClick = onPlayTap,
                    modifier = Modifier.size(TRANSPORT_BUTTON),
                    enabled = transport.loopLengthFrames > 0,
                ) {
                    Icon(Icons.Filled.PlayArrow, contentDescription = "Play", Modifier.size(36.dp))
                }

                FilledTonalIconButton(
                    onClick = onStopTap,
                    modifier = Modifier.size(TRANSPORT_BUTTON),
                ) {
                    StopGlyph(color = MaterialTheme.colorScheme.onSecondaryContainer)
                }

                // Metronome: fills while active, ticks brighter on the downbeat.
                FilledIconToggleButton(
                    checked = transport.metronomeOn,
                    onCheckedChange = { onMetronomeToggle() },
                    modifier = Modifier.size(TRANSPORT_BUTTON),
                ) {
                    Text(
                        text = "♩",  // quarter note
                        style = MaterialTheme.typography.headlineMedium,
                        color = if (transport.metronomeOn && transport.beatInBar == 0) {
                            MaterialTheme.colorScheme.onPrimary
                        } else {
                            Color.Unspecified
                        },
                    )
                }

                BpmAdjuster(bpm = transport.bpm, onBpmChange = onBpmChange)

                FilledTonalIconButton(
                    onClick = onExportTap,
                    modifier = Modifier.size(TRANSPORT_BUTTON),
                    enabled = !transport.exporting,
                ) {
                    Icon(Icons.Filled.Share, contentDescription = "Export stems")
                }
            }
        }
    }
}

@Composable
private fun BpmAdjuster(bpm: Int, onBpmChange: (Int) -> Unit) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        FilledTonalIconButton(
            onClick = { onBpmChange(-BPM_STEP) },
            modifier = Modifier.size(BPM_BUTTON),
        ) { Text("−", style = MaterialTheme.typography.titleLarge) }
        Column(horizontalAlignment = Alignment.CenterHorizontally) {
            Text("$bpm", style = MaterialTheme.typography.titleLarge)
            Text("BPM", style = MaterialTheme.typography.labelSmall)
        }
        FilledTonalIconButton(
            onClick = { onBpmChange(BPM_STEP) },
            modifier = Modifier.size(BPM_BUTTON),
        ) { Text("+", style = MaterialTheme.typography.titleLarge) }
    }
}

// The core material-icons set ships no record/stop glyphs; drawing them
// keeps the extended-icons dependency out of the build.
@Composable
private fun RecordGlyph(color: Color) {
    Canvas(Modifier.size(28.dp)) { drawCircle(color = color) }
}

@Composable
private fun StopGlyph(color: Color) {
    Canvas(Modifier.size(24.dp)) {
        drawRoundRect(color = color, cornerRadius = CornerRadius(4.dp.toPx()))
    }
}

private val TRANSPORT_BUTTON = 64.dp
private val BPM_BUTTON = 48.dp
private const val BPM_STEP = 1
