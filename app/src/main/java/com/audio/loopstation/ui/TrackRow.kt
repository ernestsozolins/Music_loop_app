package com.audio.loopstation.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.FilledIconToggleButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Slider
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
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
    modifier: Modifier = Modifier,
) {
    Card(
        onClick = onSelect,
        modifier = modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(
            containerColor = if (track.isSelected) {
                MaterialTheme.colorScheme.secondaryContainer
            } else {
                MaterialTheme.colorScheme.surfaceVariant
            },
        ),
    ) {
        Column(Modifier.padding(horizontal = 16.dp, vertical = 12.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    Text(track.name, style = MaterialTheme.typography.titleMedium)
                    Text(
                        text = when {
                            track.isClearing -> "Clearing…"
                            track.hasContent -> "Recorded"
                            track.isSelected -> "Armed"
                            else -> "Empty"
                        },
                        style = MaterialTheme.typography.labelMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
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

