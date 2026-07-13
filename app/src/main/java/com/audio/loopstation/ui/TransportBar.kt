package com.audio.loopstation.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Share
import androidx.compose.material3.AssistChip
import androidx.compose.material3.Button
import androidx.compose.material3.FilledIconButton
import androidx.compose.material3.FilledIconToggleButton
import androidx.compose.material3.FilledTonalIconButton
import androidx.compose.material3.FilterChip
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButtonDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.foundation.rememberScrollState
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import com.audio.loopstation.ui.MainViewModel.TransportUiState
import kotlinx.coroutines.flow.StateFlow

/**
 * Fixed transport strip (hosted in the Scaffold's bottomBar so it never
 * scrolls away): Record / Play / Stop / metronome toggle, BPM adjuster, and
 * Export. All controls are >= 64 dp touch targets per Material 3 ergonomics
 * for stage use. A thin playhead line on top moves at 60 fps without
 * recomposing the bar (see PlayheadProgressLine), and every button drives
 * the same ViewModel intents the Bluetooth foot pedal does — hardware and
 * touch can be mixed freely mid-performance (the caption spells out the
 * pedal mapping).
 */
@Composable
fun TransportBar(
    transport: TransportUiState,
    position: StateFlow<Float>,
    onRecordTap: () -> Unit,
    onPlayTap: () -> Unit,
    onStopTap: () -> Unit,
    onMetronomeToggle: () -> Unit,
    onBpmChange: (Int) -> Unit,
    onTapTempo: () -> Unit,
    onRhythmStyleToggle: () -> Unit,
    onExportTap: () -> Unit,
    onExportToFileTap: () -> Unit,
    onCountInToggle: () -> Unit,
    onQuantizeToggle: () -> Unit,
    onSyncToggle: () -> Unit,
    onSingleModeToggle: () -> Unit,
    onTimeSignatureTap: () -> Unit,
    onUndoTap: () -> Unit,
    onClearAllTap: () -> Unit,
    onSaveTap: () -> Unit,
    onSessionsTap: () -> Unit,
    onInputTap: () -> Unit,
    onOutputTap: () -> Unit,
    modifier: Modifier = Modifier,
) {
    Surface(modifier = modifier, tonalElevation = 3.dp) {
        Column {
            PlayheadProgressLine(position = position)
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

                TextButton(onClick = onTapTempo) { Text("Tap") }

                FilledTonalIconButton(
                    onClick = onExportTap,
                    modifier = Modifier.size(TRANSPORT_BUTTON),
                    enabled = !transport.exporting,
                ) {
                    Icon(Icons.Filled.Share, contentDescription = "Export stems")
                }
            }
            // Metronome / edit options — scrollable so it never overflows.
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .horizontalScroll(rememberScrollState())
                    .padding(horizontal = 12.dp),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                FilterChip(
                    selected = transport.countIn,
                    onClick = onCountInToggle,
                    label = {
                        Text(
                            if (transport.countIn) "Count-in ${transport.countInBars} bar"
                            else "Count-in off",
                        )
                    },
                )
                FilterChip(
                    selected = transport.quantize,
                    onClick = onQuantizeToggle,
                    label = { Text("Quantize") },
                )
                FilterChip(
                    selected = transport.syncToLoop,
                    onClick = onSyncToggle,
                    label = { Text("Click sync") },
                )
                FilterChip(
                    selected = transport.singleMode,
                    onClick = onSingleModeToggle,
                    label = { Text(if (transport.singleMode) "Single" else "Multi") },
                )
                AssistChip(
                    onClick = onTimeSignatureTap,
                    label = { Text("${transport.beatsPerMeasure}/4") },
                )
                FilterChip(
                    selected = transport.rhythmBeat,
                    onClick = onRhythmStyleToggle,
                    label = { Text(if (transport.rhythmBeat) "Beat" else "Click") },
                )
                TextButton(onClick = onUndoTap) { Text("Undo") }
                TextButton(onClick = onClearAllTap) { Text("Clear all") }
            }
            // Devices / session actions row.
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 12.dp),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Spacer(Modifier.weight(1f))
                TextButton(onClick = onInputTap) { Text("Input") }
                TextButton(onClick = onOutputTap) { Text("Output") }
                TextButton(onClick = onSessionsTap) { Text("Sessions") }
                // Writes the stems zip to a location the user picks.
                TextButton(onClick = onExportToFileTap, enabled = !transport.exporting) {
                    Text(if (transport.exporting) "Exporting…" else "Save to file…")
                }
                Button(onClick = onSaveTap, enabled = !transport.saving) {
                    Text(if (transport.saving) "Saving…" else "Save")
                }
            }
            Text(
                text = "Pedal:  Space = Play/Stop   •   Enter = Overdub/Next   •   Backspace = Undo",
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier
                    .align(Alignment.CenterHorizontally)
                    .padding(vertical = 6.dp),
            )
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
