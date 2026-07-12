package com.audio.loopstation.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.material3.Button
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedCard
import androidx.compose.material3.Slider
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.audio.loopstation.ui.MainViewModel.CalibrationUiState
import com.audio.loopstation.ui.MainViewModel.ReverbUiState

/**
 * Monitoring / setup card: the output reverb plus latency calibration. The
 * reverb lives in the C++ output chain and touches only what reaches the
 * headphones — takes, loop tracks, and exported stems stay 100% dry, which
 * is why the caption says so out loud.
 */
@Composable
fun MonitorFxPanel(
    reverb: ReverbUiState,
    calibration: CalibrationUiState,
    monitorLevel: Float,
    onMonitorLevelChange: (Float) -> Unit,
    onMixChange: (Float) -> Unit,
    onRoomSizeChange: (Float) -> Unit,
    onCalibrate: () -> Unit,
    modifier: Modifier = Modifier,
) {
    OutlinedCard(modifier = modifier.fillMaxWidth()) {
        Column(Modifier.padding(horizontal = 16.dp, vertical = 12.dp)) {
            Text("Monitoring", style = MaterialTheme.typography.titleMedium)
            Text(
                "Input monitor level (raise it if you monitor on headphones and " +
                    "don't hear yourself; leave at 0 for hardware monitoring).",
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            FxSlider(label = "Monitor", value = monitorLevel, onChange = onMonitorLevelChange)

            Text(
                "Reverb — headphones only, recordings and stems stay dry",
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(top = 8.dp),
            )
            FxSlider(label = "Mix", value = reverb.mix, onChange = onMixChange)
            FxSlider(label = "Room", value = reverb.roomSize, onChange = onRoomSizeChange)

            HorizontalDivider(Modifier.padding(vertical = 12.dp))

            Text("Latency calibration", style = MaterialTheme.typography.titleMedium)
            Text(
                "Aligns overdubs to the beat. Point the output at the speaker " +
                    "(near the mic) or use a loopback cable, then calibrate.",
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(12.dp),
                modifier = Modifier.padding(top = 8.dp),
            ) {
                Button(onClick = onCalibrate, enabled = !calibration.running) {
                    Text(if (calibration.running) "Calibrating…" else "Calibrate latency")
                }
                Spacer(Modifier.width(4.dp))
                Text(
                    calibration.message,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
    }
}

@Composable
private fun FxSlider(label: String, value: Float, onChange: (Float) -> Unit) {
    Row(
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Text(
            label,
            style = MaterialTheme.typography.labelLarge,
            modifier = Modifier.width(44.dp),
        )
        Slider(
            value = value,
            onValueChange = onChange,
            valueRange = 0f..1f,
            modifier = Modifier
                .weight(1f)
                .height(48.dp),
        )
    }
}
