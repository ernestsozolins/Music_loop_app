package com.audio.loopstation.ui

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.FilledIconToggleButton
import androidx.compose.material3.FilledTonalIconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Slider
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import com.audio.loopstation.ui.MainViewModel.TrackUiState
import kotlinx.coroutines.flow.StateFlow

/**
 * One channel strip in the multi-track grid: name + status, Mute/Solo,
 * volume (0..1), pan (-1..1), and the live waveform. Tapping the card
 * selects the track as the record/overdub target. Sliders span the full
 * width with 48 dp lanes for reliable finger control.
 *
 * [waveform] is the UNCOLLECTED flow of rolling RMS data for the selected
 * track (null on the others): passing the flow instead of a value keeps
 * 60 Hz meter data out of composition entirely — WaveformVisualizer defers
 * every read to the draw phase.
 */
@Composable
fun TrackRow(
    track: TrackUiState,
    waveform: StateFlow<FloatArray>?,
    offlineWaveform: FloatArray?,
    onSelect: () -> Unit,
    onMuteToggle: () -> Unit,
    onSoloToggle: () -> Unit,
    onVolumeChange: (Float) -> Unit,
    onPanChange: (Float) -> Unit,
    onClear: () -> Unit,
    onTrim: () -> Unit,
    onNudge: (Int) -> Unit,
    modifier: Modifier = Modifier,
) {
    Card(
        onClick = onSelect,
        modifier = modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(
            containerColor = if (track.isSelected) {
                MaterialTheme.colorScheme.primaryContainer
            } else {
                MaterialTheme.colorScheme.surfaceVariant
            },
        ),
        // Selected track gets a bold accent border so it's unmistakable.
        border = if (track.isSelected) {
            BorderStroke(3.dp, MaterialTheme.colorScheme.primary)
        } else {
            null
        },
        elevation = CardDefaults.cardElevation(
            defaultElevation = if (track.isSelected) 6.dp else 1.dp,
        ),
    ) {
        Column(Modifier.padding(horizontal = 16.dp, vertical = 12.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                // Accent bar on the selected track's leading edge.
                if (track.isSelected) {
                    Box(
                        Modifier
                            .width(5.dp)
                            .height(36.dp)
                            .clip(RoundedCornerShape(3.dp))
                            .background(MaterialTheme.colorScheme.primary),
                    )
                    Spacer(Modifier.width(12.dp))
                }
                Column(Modifier.weight(1f)) {
                    Text(
                        track.name,
                        style = MaterialTheme.typography.titleMedium,
                        color = if (track.isSelected) {
                            MaterialTheme.colorScheme.onPrimaryContainer
                        } else {
                            MaterialTheme.colorScheme.onSurface
                        },
                    )
                    Text(
                        text = when {
                            track.isClearing -> "Clearing…"
                            track.isSelected && track.hasContent -> "● SELECTED · Recorded"
                            track.isSelected -> "● SELECTED · Armed to record"
                            track.hasContent -> "Recorded"
                            else -> "Empty"
                        },
                        style = MaterialTheme.typography.labelMedium,
                        color = if (track.isSelected) {
                            MaterialTheme.colorScheme.primary
                        } else {
                            MaterialTheme.colorScheme.onSurfaceVariant
                        },
                    )
                }
                ToggleChip(label = "M", checked = track.muted, onToggle = onMuteToggle)
                Spacer(Modifier.width(8.dp))
                ToggleChip(label = "S", checked = track.soloed, onToggle = onSoloToggle)
            }

            // Selected row: live input meter. Recorded rows: the actual loop
            // audio (offline peaks). Empty rows: resting line.
            when {
                waveform != null -> WaveformVisualizer(
                    waveform = waveform,
                    modifier = Modifier.padding(vertical = 8.dp),
                )
                offlineWaveform != null -> StaticWaveform(
                    bins = offlineWaveform,
                    modifier = Modifier.padding(vertical = 8.dp),
                )
                else -> WaveformVisualizer(
                    waveform = null,
                    modifier = Modifier.padding(vertical = 8.dp),
                )
            }

            LabeledSlider(
                label = "Vol",
                value = track.volume,
                range = 0f..1f,
                onChange = onVolumeChange,
            )
            LabeledSlider(
                label = "Pan",
                value = track.pan,
                range = -1f..1f,
                onChange = onPanChange,
            )

            // Edit actions — only meaningful once the track holds audio.
            if (track.hasContent) {
                // Live start-shift stepper: nudge timing without stopping.
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(6.dp),
                    modifier = Modifier.padding(top = 4.dp),
                ) {
                    Text("Shift", style = MaterialTheme.typography.labelLarge)
                    FilledTonalIconButton(
                        onClick = { onNudge(-NUDGE_MS) },
                        modifier = Modifier.size(44.dp),
                    ) { Text("◀", style = MaterialTheme.typography.titleMedium) }
                    Text(
                        text = "%+d ms".format(track.shiftMs),
                        style = MaterialTheme.typography.bodyMedium,
                        modifier = Modifier.width(64.dp),
                        textAlign = TextAlign.Center,
                    )
                    FilledTonalIconButton(
                        onClick = { onNudge(NUDGE_MS) },
                        modifier = Modifier.size(44.dp),
                    ) { Text("▶", style = MaterialTheme.typography.titleMedium) }
                }
                Row(
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                    modifier = Modifier.padding(top = 4.dp),
                ) {
                    TextButton(onClick = onTrim) { Text("Trim start") }
                    TextButton(onClick = onClear) { Text("Clear") }
                }
            }
        }
    }
}

@Composable
private fun ToggleChip(label: String, checked: Boolean, onToggle: () -> Unit) {
    FilledIconToggleButton(
        checked = checked,
        onCheckedChange = { onToggle() },
        modifier = Modifier.size(48.dp),
    ) {
        Text(label, style = MaterialTheme.typography.titleMedium)
    }
}

@Composable
private fun LabeledSlider(
    label: String,
    value: Float,
    range: ClosedFloatingPointRange<Float>,
    onChange: (Float) -> Unit,
) {
    Row(
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Text(
            label,
            style = MaterialTheme.typography.labelLarge,
            modifier = Modifier.width(32.dp),
        )
        Slider(
            value = value,
            onValueChange = onChange,
            valueRange = range,
            modifier = Modifier
                .weight(1f)
                .height(48.dp),  // full-height touch lane
        )
    }
}


private const val NUDGE_MS = 10  // per-tap start-shift step
