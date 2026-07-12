package com.audio.loopstation.ui

import androidx.compose.foundation.layout.Column
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Slider
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import kotlin.math.min

/**
 * Cut a bad start off a recorded track. The slider chooses how many
 * milliseconds to silence from the beginning; the range is capped at the
 * loop length (or 5 s, whichever is smaller). The edit is undoable, so an
 * over-cut is easy to walk back.
 */
@Composable
fun TrimStartDialog(
    loopMs: Int,
    onApply: (Int) -> Unit,
    onDismiss: () -> Unit,
) {
    val maxMs = min(if (loopMs > 0) loopMs else 5000, 5000).toFloat()
    var value by remember { mutableFloatStateOf(0f) }

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Trim start") },
        text = {
            Column {
                Text(
                    "Silence the first ${value.toInt()} ms of this track.",
                    style = MaterialTheme.typography.bodyMedium,
                )
                Slider(
                    value = value,
                    onValueChange = { value = it },
                    valueRange = 0f..maxMs,
                )
                Text(
                    "You can undo this from the transport bar.",
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        },
        confirmButton = {
            TextButton(
                onClick = { onApply(value.toInt()) },
                enabled = value >= 1f,
            ) { Text("Trim") }
        },
        dismissButton = {
            TextButton(onClick = onDismiss) { Text("Cancel") }
        },
    )
}
