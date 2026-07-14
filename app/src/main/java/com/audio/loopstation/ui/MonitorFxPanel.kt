package com.audio.loopstation.ui

import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.material3.Button
import androidx.compose.material3.FilterChip
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedCard
import androidx.compose.material3.Slider
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.flow.StateFlow
import com.audio.loopstation.AudioEngine
import com.audio.loopstation.ui.MainViewModel.CalibrationUiState
import com.audio.loopstation.ui.MainViewModel.DroneNote
import com.audio.loopstation.ui.MainViewModel.DroneUiState
import com.audio.loopstation.ui.MainViewModel.FilterUiState
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
    filter: FilterUiState,
    drone: DroneUiState,
    droneNotes: List<DroneNote>,
    calibration: CalibrationUiState,
    inputLevel: StateFlow<Float>,
    manualLatencyMs: Int,
    onManualLatencyChange: (Int) -> Unit,
    monitorLevel: Float,
    onMonitorLevelChange: (Float) -> Unit,
    onMixChange: (Float) -> Unit,
    onRoomSizeChange: (Float) -> Unit,
    onFilterToggle: () -> Unit,
    onFilterCutoff: (Float) -> Unit,
    onFilterResonance: (Float) -> Unit,
    onFilterMode: (AudioEngine.FilterMode) -> Unit,
    onDroneToggle: () -> Unit,
    onDroneNote: (Float) -> Unit,
    onDroneGain: (Float) -> Unit,
    onCalibrate: () -> Unit,
    modifier: Modifier = Modifier,
) {
    OutlinedCard(modifier = modifier.fillMaxWidth()) {
        Column(Modifier.padding(horizontal = 16.dp, vertical = 12.dp)) {
            Text("Input level", style = MaterialTheme.typography.titleMedium)
            Text(
                "Set your interface/mic so loud bowing peaks near — but not into — " +
                    "the red.",
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            InputLevelMeter(level = inputLevel, modifier = Modifier.padding(vertical = 8.dp))

            HorizontalDivider(Modifier.padding(vertical = 12.dp))

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

            // Filter FX — sweepable low/high/band-pass on the monitor mix.
            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                Text("Filter", style = MaterialTheme.typography.titleMedium)
                Spacer(Modifier.width(4.dp))
                FilterChip(
                    selected = filter.enabled,
                    onClick = onFilterToggle,
                    label = { Text(if (filter.enabled) "On" else "Off") },
                )
            }
            Row(
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                modifier = Modifier.padding(top = 4.dp),
            ) {
                FilterModeChip("LP", AudioEngine.FilterMode.LOW_PASS, filter.mode, onFilterMode)
                FilterModeChip("HP", AudioEngine.FilterMode.HIGH_PASS, filter.mode, onFilterMode)
                FilterModeChip("BP", AudioEngine.FilterMode.BAND_PASS, filter.mode, onFilterMode)
            }
            FxSlider(label = "Freq", value = filter.cutoffNorm, onChange = onFilterCutoff)
            FxSlider(label = "Res", value = filter.resonance, onChange = onFilterResonance)

            HorizontalDivider(Modifier.padding(vertical = 12.dp))

            // Tuning / practice drone — tune the cello to it or play over it.
            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                Text("Tuning drone", style = MaterialTheme.typography.titleMedium)
                Spacer(Modifier.width(4.dp))
                FilterChip(
                    selected = drone.enabled,
                    onClick = onDroneToggle,
                    label = { Text(if (drone.enabled) "On" else "Off") },
                )
            }
            Text(
                "Sustained reference to tune each open string to, or a tonic to " +
                    "improvise over. Output only — never recorded.",
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Row(
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                modifier = Modifier
                    .padding(top = 4.dp)
                    .horizontalScroll(rememberScrollState()),
            ) {
                droneNotes.forEach { note ->
                    FilterChip(
                        selected = drone.hz == note.hz,
                        onClick = { onDroneNote(note.hz) },
                        label = { Text(note.label) },
                    )
                }
            }
            FxSlider(label = "Level", value = drone.gain, onChange = onDroneGain)

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

            Text(
                "Manual offset: $manualLatencyMs ms — for Bluetooth headphones " +
                    "where calibration can't hear the output, nudge until overdubs " +
                    "line up by ear.",
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(top = 8.dp),
            )
            Slider(
                value = manualLatencyMs.toFloat(),
                onValueChange = { onManualLatencyChange(it.toInt()) },
                valueRange = 0f..500f,
                modifier = Modifier
                    .fillMaxWidth()
                    .height(48.dp),
            )
        }
    }
}

/**
 * Peak input meter with a decaying peak-hold marker and a clip indicator that
 * latches for ~1.5 s, so a fast bow transient is easy to catch when setting
 * gain. Everything is read in the draw phase (a vsync clock drives the decay),
 * so it never recomposes the panel.
 */
@Composable
private fun InputLevelMeter(level: StateFlow<Float>, modifier: Modifier = Modifier) {
    val trackColor = MaterialTheme.colorScheme.surfaceVariant
    val okColor = MaterialTheme.colorScheme.primary
    val hotColor = Color(0xFFE0A030)
    val clipColor = MaterialTheme.colorScheme.error
    val holdColor = MaterialTheme.colorScheme.onSurface

    val peak = produceState(initialValue = 0f, level) { level.collect { value = it } }
    val frameClock = remember { mutableLongStateOf(0L) }
    LaunchedEffect(level) { while (true) withFrameNanos { frameClock.longValue = it } }
    val hold = remember { PeakHold() }

    androidx.compose.foundation.layout.Spacer(
        modifier
            .fillMaxWidth()
            .height(18.dp)
            .drawBehind {
                val now = frameClock.longValue  // subscribe this draw scope to vsync
                val v = peak.value.coerceIn(0f, 1f)
                hold.update(v, now)
                drawRect(trackColor)
                val color = when {
                    v >= 0.98f -> clipColor
                    v >= 0.8f -> hotColor
                    else -> okColor
                }
                drawRect(color, size = Size(size.width * v, size.height))
                // Decaying peak-hold marker.
                val hx = size.width * hold.peak.coerceIn(0f, 1f)
                drawLine(holdColor, Offset(hx, 0f), Offset(hx, size.height), strokeWidth = 3f)
                // Latched clip zone (last ~5% of the bar) turns solid red on a clip.
                if (hold.clipped(now)) {
                    drawRect(
                        clipColor,
                        topLeft = Offset(size.width * 0.95f, 0f),
                        size = Size(size.width * 0.05f, size.height),
                    )
                }
            },
    )
}

/** Draw-phase peak-hold + clip latch (plain object; mutated inside drawBehind). */
private class PeakHold {
    var peak = 0f
        private set
    private var clipUntil = 0L
    private var lastNanos = 0L

    fun update(v: Float, now: Long) {
        if (lastNanos == 0L) lastNanos = now
        val dt = (now - lastNanos).coerceAtLeast(0L)
        lastNanos = now
        peak = (peak - dt / 1_200_000_000f).coerceAtLeast(0f)  // ~1.2 s fall
        if (v > peak) peak = v
        if (v >= 0.98f) clipUntil = now + 1_500_000_000L  // latch 1.5 s
    }

    fun clipped(now: Long): Boolean = now < clipUntil
}

@Composable
private fun FilterModeChip(
    label: String,
    mode: AudioEngine.FilterMode,
    selected: AudioEngine.FilterMode,
    onSelect: (AudioEngine.FilterMode) -> Unit,
) {
    FilterChip(
        selected = mode == selected,
        onClick = { onSelect(mode) },
        label = { Text(label) },
    )
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
