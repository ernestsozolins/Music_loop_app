package com.audio.loopstation.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Slider
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.lerp
import androidx.compose.ui.unit.dp
import kotlin.math.sqrt
import kotlinx.coroutines.launch

/**
 * Non-destructive trim editor: the WHOLE take stays visible, and two handles
 * set the audible window (start / end). Everything is applied live through
 * [onChange] so the change is heard while dragging the sliders. Auto-trim runs
 * the engine's silence detector; Reset restores the full take.
 *
 * Moving the handles only MUTES the ends — the loop keeps its original length.
 * "Shorten loop to this" ([onApplyToLoop]) commits the window as the actual
 * loop: every track is cropped to the same region (so the layers stay in sync)
 * and the loop repeats sooner. That discards the audio outside the window, so
 * it asks for confirmation first.
 */
@Composable
fun TrimEditorDialog(
    trackName: String,
    loopMs: Int,
    initialStartMs: Int,
    initialEndMs: Int,
    bins: FloatArray?,
    onChange: (startMs: Int, endMs: Int) -> Unit,
    onAutoTrim: suspend () -> Pair<Int, Int>,
    onApplyToLoop: () -> Unit,
    onDismiss: () -> Unit,
) {
    val scope = rememberCoroutineScope()
    val safeLoop = loopMs.coerceAtLeast(1)
    val minWin = (safeLoop / 50).coerceAtLeast(20)  // keep a non-empty window
    var startMs by remember { mutableIntStateOf(initialStartMs.coerceIn(0, safeLoop)) }
    var endMs by remember {
        mutableIntStateOf(if (initialEndMs <= 0 || initialEndMs > safeLoop) safeLoop else initialEndMs)
    }
    var confirmApply by remember { mutableStateOf(false) }

    AlertDialog(
        onDismissRequest = onDismiss,
        confirmButton = { TextButton(onClick = onDismiss) { Text("Done") } },
        title = { Text("Trim — $trackName") },
        text = {
            Column {
                Text(
                    "The whole take is kept — only the highlighted window plays. " +
                        "Move the handles or auto-trim the silence. To make the loop " +
                        "itself shorter, use “Shorten loop to this”.",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                TrimWaveform(
                    bins = bins,
                    startFrac = startMs.toFloat() / safeLoop,
                    endFrac = endMs.toFloat() / safeLoop,
                    modifier = Modifier.padding(vertical = 12.dp),
                )
                TrimSlider(
                    label = "Start",
                    valueMs = startMs,
                    rangeMs = 0f..(endMs - minWin).coerceAtLeast(0).toFloat(),
                    onChange = {
                        startMs = it.coerceIn(0, endMs - minWin)
                        onChange(startMs, endMs)
                    },
                )
                TrimSlider(
                    label = "End",
                    valueMs = endMs,
                    rangeMs = (startMs + minWin).coerceAtMost(safeLoop).toFloat()..safeLoop.toFloat(),
                    onChange = {
                        endMs = it.coerceIn(startMs + minWin, safeLoop)
                        onChange(startMs, endMs)
                    },
                )
                Row(
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                    modifier = Modifier.padding(top = 4.dp),
                ) {
                    Button(onClick = {
                        scope.launch {
                            val (s, e) = onAutoTrim()
                            startMs = s.coerceIn(0, safeLoop)
                            endMs = if (e <= 0 || e > safeLoop) safeLoop else e
                        }
                    }) { Text("Auto-trim") }
                    OutlinedButton(onClick = {
                        startMs = 0
                        endMs = safeLoop
                        onChange(0, safeLoop)
                    }) { Text("Reset") }
                }
                // Commit the window as the real loop length. Destructive, so
                // it confirms — and it is disabled when nothing would change.
                val wouldChange = startMs > 0 || endMs < safeLoop
                Button(
                    onClick = { confirmApply = true },
                    enabled = wouldChange,
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(top = 12.dp),
                ) {
                    Text(
                        if (wouldChange) {
                            "Shorten loop to this (%.1fs)".format((endMs - startMs) / 1000f)
                        } else {
                            "Shorten loop to this"
                        },
                    )
                }
            }
        },
    )

    if (confirmApply) {
        AlertDialog(
            onDismissRequest = { confirmApply = false },
            title = { Text("Shorten the loop?") },
            text = {
                Text(
                    "The loop becomes %.1fs. Every track is cropped to the same region so "
                        .format((endMs - startMs) / 1000f) +
                        "they stay in sync. Audio outside the window is discarded — this " +
                        "can't be undone.",
                )
            },
            confirmButton = {
                TextButton(onClick = {
                    confirmApply = false
                    onApplyToLoop()
                    onDismiss()
                }) { Text("Shorten") }
            },
            dismissButton = {
                TextButton(onClick = { confirmApply = false }) { Text("Cancel") }
            },
        )
    }
}

@Composable
private fun TrimSlider(
    label: String,
    valueMs: Int,
    rangeMs: ClosedFloatingPointRange<Float>,
    onChange: (Int) -> Unit,
) {
    Row(
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Text(label, style = MaterialTheme.typography.labelLarge, modifier = Modifier.width(44.dp))
        Slider(
            value = valueMs.toFloat().coerceIn(rangeMs.start, rangeMs.endInclusive),
            onValueChange = { onChange(it.toInt()) },
            valueRange = rangeMs,
            modifier = Modifier
                .weight(1f)
                .height(48.dp),
        )
        Text(
            "%.1fs".format(valueMs / 1000f),
            style = MaterialTheme.typography.bodySmall,
            modifier = Modifier.width(44.dp),
        )
    }
}

/** Full-take waveform with the trimmed-out ends dimmed and handle lines drawn. */
@Composable
private fun TrimWaveform(
    bins: FloatArray?,
    startFrac: Float,
    endFrac: Float,
    modifier: Modifier = Modifier,
) {
    val barColor = MaterialTheme.colorScheme.secondary
    val hotColor = MaterialTheme.colorScheme.tertiary
    val dimColor = MaterialTheme.colorScheme.surface.copy(alpha = 0.62f)
    val handleColor = MaterialTheme.colorScheme.primary
    Row(
        modifier
            .fillMaxWidth()
            .height(72.dp)
            .drawBehind {
                val midY = size.height / 2f
                val data = bins ?: FloatArray(0)
                if (data.isNotEmpty()) {
                    val step = size.width / data.size
                    val stroke = step * 0.7f
                    for (i in data.indices) {
                        val v = data[i].coerceIn(0f, 1f)
                        val half = sqrt(v) * midY
                        val x = i * step + step / 2f
                        drawLine(
                            color = lerp(barColor, hotColor, v),
                            start = Offset(x, midY - half),
                            end = Offset(x, midY + half),
                            strokeWidth = stroke,
                            cap = StrokeCap.Round,
                        )
                    }
                } else {
                    drawLine(barColor, Offset(0f, midY), Offset(size.width, midY), 2f)
                }
                // Dim the trimmed-out regions.
                val sx = (startFrac.coerceIn(0f, 1f)) * size.width
                val ex = (endFrac.coerceIn(0f, 1f)) * size.width
                if (sx > 0f) drawRect(dimColor, size = Size(sx, size.height))
                if (ex < size.width) {
                    drawRect(dimColor, topLeft = Offset(ex, 0f), size = Size(size.width - ex, size.height))
                }
                // Handle lines.
                drawLine(handleColor, Offset(sx, 0f), Offset(sx, size.height), 4f)
                drawLine(handleColor, Offset(ex, 0f), Offset(ex, size.height), 4f)
            },
    ) {}
}
