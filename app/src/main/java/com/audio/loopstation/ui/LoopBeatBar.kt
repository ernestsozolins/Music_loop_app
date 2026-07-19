package com.audio.loopstation.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.background
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.unit.dp
import com.audio.loopstation.ui.MainViewModel.TransportUiState

/**
 * A thin bar under the loop button: the loop's structure (bars + length) on
 * the left, and an on-screen beat pulse on the right (one dot per beat, the
 * current beat lit) so a player knows exactly when to come in — the count-in
 * turns the whole thing red and prompts "Get ready".
 */
@Composable
fun LoopBeatBar(
    transport: TransportUiState,
    loopMs: Int,
    modifier: Modifier = Modifier,
) {
    val counting = transport.isCountingIn
    Row(
        modifier = modifier
            .fillMaxWidth()
            .padding(horizontal = 16.dp, vertical = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            text = when {
                counting -> {
                    val n = transport.countInBeatsRemaining
                    if (n > 0) "Get ready… $n" else "Here we go!"
                }
                transport.hasLoop -> "${transport.loopBars} bar${if (transport.loopBars == 1) "" else "s"} · " +
                    "%.1fs".format(loopMs / 1000f)
                else -> ""
            },
            style = MaterialTheme.typography.labelLarge,
            color = if (counting) {
                MaterialTheme.colorScheme.error
            } else {
                MaterialTheme.colorScheme.onSurfaceVariant
            },
        )
        Spacer(Modifier.weight(1f))
        if (transport.metronomeOn || counting) {
            Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                val beats = transport.beatsPerMeasure.coerceIn(1, 12)
                for (b in 0 until beats) {
                    val active = b == transport.beatInBar
                    val color = when {
                        active && counting -> MaterialTheme.colorScheme.error
                        active && b == 0 -> MaterialTheme.colorScheme.primary
                        active -> MaterialTheme.colorScheme.tertiary
                        else -> MaterialTheme.colorScheme.surfaceVariant
                    }
                    Box(
                        Modifier
                            .size(if (active) 16.dp else 12.dp)
                            .clip(CircleShape)
                            .background(color),
                    )
                }
            }
        }
    }
}
