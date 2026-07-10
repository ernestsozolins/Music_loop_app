package com.audio.loopstation.ui

import androidx.compose.foundation.Canvas
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
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.unit.dp
import com.audio.loopstation.ui.MainViewModel.TrackUiState

/**
 * One channel strip in the multi-track grid: name + status, Mute/Solo,
 * volume (0..1), pan (-1..1), and the waveform area. Tapping the card
 * selects the track as the record/overdub target. Sliders span the full
 * width with 48 dp lanes for reliable finger control.
 *
 * [waveform] carries the live rolling RMS history for the selected track
 * (per-track offline waveforms arrive in a later phase — this is the
 * visualizer placeholder the layout reserves).
 */
@Composable
fun TrackRow(
    track: TrackUiState,
    waveform: FloatArray?,
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

            WaveformArea(
                waveform = if (track.hasContent || track.isSelected) waveform else null,
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(vertical = 8.dp),
            )

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

/**
 * Waveform visualizer placeholder: draws the rolling RMS bars when live
 * data is supplied, otherwise a resting center line.
 */
@Composable
private fun WaveformArea(waveform: FloatArray?, modifier: Modifier = Modifier) {
    val barColor = MaterialTheme.colorScheme.primary
    val restColor = MaterialTheme.colorScheme.outlineVariant
    Canvas(modifier.height(48.dp)) {
        val midY = size.height / 2f
        if (waveform == null || waveform.isEmpty()) {
            drawLine(restColor, Offset(0f, midY), Offset(size.width, midY), strokeWidth = 2f)
            return@Canvas
        }
        val step = size.width / waveform.size
        for (i in waveform.indices) {
            val half = (waveform[i].coerceIn(0f, 1f)) * midY
            val x = i * step + step / 2f
            drawLine(barColor, Offset(x, midY - half), Offset(x, midY + half), strokeWidth = step * 0.7f)
        }
    }
}
