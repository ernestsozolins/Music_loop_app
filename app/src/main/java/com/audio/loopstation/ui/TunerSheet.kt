package com.audio.loopstation.ui

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import com.audio.loopstation.ui.MainViewModel.TunerUiState
import kotlin.math.abs

/**
 * Tuner: shows the detected note, how many cents sharp/flat, and the frequency.
 * Tune each open string until the needle sits at centre and the readout turns
 * green (within a few cents). Pitch is detected on the live input.
 */
@Composable
fun TunerSheet(
    tuner: TunerUiState,
    onDismiss: () -> Unit,
) {
    val inTune = tuner.hasPitch && abs(tuner.cents) <= 5
    val accent = if (inTune) Color(0xFF3DDC84) else MaterialTheme.colorScheme.primary
    val trackColor = MaterialTheme.colorScheme.surfaceVariant
    val centerColor = MaterialTheme.colorScheme.outline

    AlertDialog(
        onDismissRequest = onDismiss,
        confirmButton = { TextButton(onClick = onDismiss) { Text("Done") } },
        title = { Text("Tuner") },
        text = {
            Column {
                Text(
                    text = if (tuner.hasPitch) tuner.note else "—",
                    style = MaterialTheme.typography.displayMedium,
                    color = if (tuner.hasPitch) accent else MaterialTheme.colorScheme.onSurfaceVariant,
                    textAlign = TextAlign.Center,
                    modifier = Modifier.fillMaxWidth(),
                )
                Text(
                    text = when {
                        !tuner.hasPitch -> "Play a note…"
                        tuner.cents == 0 -> "In tune"
                        tuner.cents > 0 -> "+${tuner.cents} cents (sharp)"
                        else -> "${tuner.cents} cents (flat)"
                    },
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    textAlign = TextAlign.Center,
                    modifier = Modifier.fillMaxWidth(),
                )
                // Cents needle: centre = in tune, ±50 cents across the width.
                Column(
                    Modifier
                        .fillMaxWidth()
                        .height(44.dp)
                        .padding(vertical = 12.dp)
                        .drawBehind {
                            val midY = size.height / 2f
                            drawLine(
                                trackColor,
                                Offset(0f, midY),
                                Offset(size.width, midY),
                                strokeWidth = 6f,
                            )
                            // Centre tick.
                            drawLine(
                                centerColor,
                                Offset(size.width / 2f, 0f),
                                Offset(size.width / 2f, size.height),
                                strokeWidth = 3f,
                            )
                            if (tuner.hasPitch) {
                                val frac = (tuner.cents.coerceIn(-50, 50) / 50f + 1f) / 2f
                                val x = frac * size.width
                                drawRect(
                                    accent,
                                    topLeft = Offset(x - 5f, 0f),
                                    size = Size(10f, size.height),
                                )
                            }
                        },
                ) {}
                Text(
                    text = if (tuner.hasPitch) "%.1f Hz".format(tuner.hz) else "",
                    style = MaterialTheme.typography.labelMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    textAlign = TextAlign.Center,
                    modifier = Modifier.fillMaxWidth(),
                )
            }
        },
    )
}
