package com.audio.loopstation.ui

import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.lerp
import androidx.compose.ui.unit.dp
import kotlin.math.max
import kotlin.math.sqrt
import kotlinx.coroutines.flow.StateFlow

/**
 * 60 fps rolling-RMS waveform.
 *
 * RECOMPOSITION DISCIPLINE — nothing in this composable's body reads a
 * value that changes per frame, so neither it nor its parents ever
 * recompose while audio runs. Two per-frame signals exist, and both are
 * read ONLY inside the drawBehind lambda, which scopes their invalidation
 * to the draw phase (a redraw, not a recomposition):
 *
 *  1. [produceState] collects the engine's polled RMS flow into a State
 *     whose sole reader is the draw lambda.
 *  2. A [withFrameNanos] loop bumps a frame-clock State every vsync, so the
 *     draw lambda re-executes at display rate and can advance its meter
 *     ballistics (instant attack, exponential release) between data blocks.
 *
 * The smoothed presentation copy is a plain array in a [remember]ed holder —
 * deliberately NOT snapshot state, since mutating it must not invalidate
 * anything (the frame clock already drives the redraw).
 *
 * Pass waveform = null for an inert resting line (empty track rows) — no
 * flows, no frame clock, zero per-frame work.
 */
@Composable
fun WaveformVisualizer(
    waveform: StateFlow<FloatArray>?,
    modifier: Modifier = Modifier,
) {
    val barColor = MaterialTheme.colorScheme.primary
    val hotColor = MaterialTheme.colorScheme.tertiary
    val restColor = MaterialTheme.colorScheme.outlineVariant

    if (waveform == null) {
        Spacer(
            modifier
                .fillMaxWidth()
                .height(DEFAULT_HEIGHT)
                .drawBehind {
                    val midY = size.height / 2f
                    drawLine(restColor, Offset(0f, midY), Offset(size.width, midY), 2f)
                },
        )
        return
    }

    // (1) Engine data, collected outside composition; read only when drawing.
    val target = produceState(initialValue = FloatArray(0), waveform) {
        waveform.collect { value = it }
    }

    // (2) Frame clock: one Long bump per vsync.
    val frameClock = remember { mutableLongStateOf(0L) }
    LaunchedEffect(waveform) {
        while (true) withFrameNanos { frameClock.longValue = it }
    }

    // Presentation copy for the meter ballistics (plain array on purpose).
    val smoothed = remember(waveform) { FloatArrayHolder() }

    Spacer(
        modifier
            .fillMaxWidth()
            .height(DEFAULT_HEIGHT)
            .drawBehind {
                frameClock.longValue  // subscribe this draw scope to the vsync clock
                val data = target.value

                val bars = smoothed.fitTo(data.size)
                for (i in data.indices) {
                    val v = data[i]
                    // Instant attack, ~ -1 dB/frame release: peaks land hard
                    // and decay smoothly instead of flickering.
                    bars[i] = if (v >= bars[i]) v else max(v, bars[i] * RELEASE_PER_FRAME)
                }

                val midY = size.height / 2f
                if (bars.isEmpty()) {
                    drawLine(restColor, Offset(0f, midY), Offset(size.width, midY), 2f)
                    return@drawBehind
                }
                val step = size.width / bars.size
                val strokeWidth = step * 0.7f
                for (i in bars.indices) {
                    val v = bars[i].coerceIn(0f, 1f)
                    // sqrt lifts quiet material into the visible range
                    // (linear RMS leaves most music hugging the midline).
                    val half = sqrt(v) * midY
                    val x = i * step + step / 2f
                    drawLine(
                        color = lerp(barColor, hotColor, v),
                        start = Offset(x, midY - half),
                        end = Offset(x, midY + half),
                        strokeWidth = strokeWidth,
                        cap = StrokeCap.Round,
                    )
                }
            },
    )
}

/**
 * Playhead progress line for the transport bar. Same deferred-read pattern:
 * the ~60 Hz position flow is collected via [produceState] and read only in
 * the draw lambda, so the moving playhead redraws this 3 dp strip without
 * recomposing the transport bar (or anything else).
 */
@Composable
fun PlayheadProgressLine(
    position: StateFlow<Float>,
    modifier: Modifier = Modifier,
) {
    val trackColor = MaterialTheme.colorScheme.surfaceVariant
    val fillColor = MaterialTheme.colorScheme.primary
    val pos = produceState(initialValue = 0f, position) {
        position.collect { value = it }
    }
    Spacer(
        modifier
            .fillMaxWidth()
            .height(3.dp)
            .drawBehind {
                drawRect(trackColor)
                drawRect(fillColor, size = Size(size.width * pos.value.coerceIn(0f, 1f), size.height))
            },
    )
}

/**
 * Static (offline) waveform for a recorded track: the same bar styling as
 * the live visualizer but drawn from a fixed peak-bin array — no flows, no
 * frame clock, redraws only when [bins] changes.
 */
@Composable
fun StaticWaveform(bins: FloatArray, modifier: Modifier = Modifier) {
    val barColor = MaterialTheme.colorScheme.secondary
    val hotColor = MaterialTheme.colorScheme.tertiary
    Spacer(
        modifier
            .fillMaxWidth()
            .height(DEFAULT_HEIGHT)
            .drawBehind {
                val midY = size.height / 2f
                if (bins.isEmpty()) return@drawBehind
                val step = size.width / bins.size
                val strokeWidth = step * 0.7f
                for (i in bins.indices) {
                    val v = bins[i].coerceIn(0f, 1f)
                    val half = sqrt(v) * midY
                    val x = i * step + step / 2f
                    drawLine(
                        color = lerp(barColor, hotColor, v),
                        start = Offset(x, midY - half),
                        end = Offset(x, midY + half),
                        strokeWidth = strokeWidth,
                        cap = StrokeCap.Round,
                    )
                }
            },
    )
}

/** Mutable, non-snapshot scratch array reused across draw passes. */
private class FloatArrayHolder {
    private var values = FloatArray(0)

    fun fitTo(size: Int): FloatArray {
        if (values.size != size) values = FloatArray(size)
        return values
    }
}

private val DEFAULT_HEIGHT = 48.dp
private const val RELEASE_PER_FRAME = 0.88f  // tuned for 60 Hz displays
