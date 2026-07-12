package com.audio.loopstation.ui

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.FilledIconToggleButton
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.FilledTonalIconButton
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedCard
import androidx.compose.material3.Slider
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.audio.loopstation.AudioEngine.BackingTrackState
import com.audio.loopstation.ui.MainViewModel.BackingTrackUiState

/**
 * Backing-track rack: streams pre-recorded material (a click loop, a drum
 * bed, a full song to jam over) from disk into the output mix. These are
 * never captured into loop tracks and stay out of exported stems.
 *
 * The whole rack is built for live use — one big Play/Pause per slot, a loop
 * toggle, and a level fader, all reachable without leaving the performance
 * screen. Loading uses the system file picker (WAV only); the copy + decode
 * happens off-thread so a tap never blocks audio.
 */
@Composable
fun BackingTrackPanel(
    slots: List<BackingTrackUiState>,
    onLoad: (Int, android.net.Uri) -> Unit,
    onTogglePlay: (Int) -> Unit,
    onToggleLoop: (Int) -> Unit,
    onGainChange: (Int, Float) -> Unit,
    onRemove: (Int) -> Unit,
    modifier: Modifier = Modifier,
) {
    // One picker for the whole rack; the target slot is stashed before launch.
    var pendingSlot by remember { mutableIntStateOf(-1) }
    val picker = rememberLauncherForActivityResult(
        ActivityResultContracts.OpenDocument(),
    ) { uri ->
        val slot = pendingSlot
        pendingSlot = -1
        if (uri != null && slot >= 0) onLoad(slot, uri)
    }

    OutlinedCard(modifier = modifier.fillMaxWidth()) {
        Column(Modifier.padding(horizontal = 16.dp, vertical = 12.dp)) {
            Text("Backing tracks", style = MaterialTheme.typography.titleMedium)
            Text(
                "Jam over a click, a drum bed, or a full song (WAV). Streamed to " +
                    "the output only — never recorded into your loops or stems.",
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            slots.forEachIndexed { i, slot ->
                if (i > 0) HorizontalDivider(Modifier.padding(vertical = 10.dp))
                BackingTrackRow(
                    slot = slot,
                    onPick = {
                        pendingSlot = slot.slot
                        picker.launch(arrayOf("audio/*"))
                    },
                    onTogglePlay = { onTogglePlay(slot.slot) },
                    onToggleLoop = { onToggleLoop(slot.slot) },
                    onGainChange = { onGainChange(slot.slot, it) },
                    onRemove = { onRemove(slot.slot) },
                )
            }
        }
    }
}

@Composable
private fun BackingTrackRow(
    slot: BackingTrackUiState,
    onPick: () -> Unit,
    onTogglePlay: () -> Unit,
    onToggleLoop: () -> Unit,
    onGainChange: (Float) -> Unit,
    onRemove: () -> Unit,
) {
    Column(Modifier.padding(top = 8.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text(
                    text = slot.name.ifBlank { "Slot ${slot.slot + 1}" },
                    style = MaterialTheme.typography.titleSmall,
                    maxLines = 1,
                )
                Text(
                    text = statusLine(slot),
                    style = MaterialTheme.typography.labelMedium,
                    color = if (slot.isError) {
                        MaterialTheme.colorScheme.error
                    } else {
                        MaterialTheme.colorScheme.onSurfaceVariant
                    },
                )
            }

            if (slot.isBusy) {
                CircularProgressIndicator(Modifier.size(28.dp))
            } else if (slot.isLoaded) {
                // Loop toggle.
                FilledIconToggleButton(
                    checked = slot.loop,
                    onCheckedChange = { onToggleLoop() },
                    modifier = Modifier.size(48.dp),
                ) { Text("↻", style = MaterialTheme.typography.titleMedium) }
                // Big Play/Pause — the one control you hit mid-performance.
                FilledTonalIconButton(
                    onClick = onTogglePlay,
                    modifier = Modifier
                        .padding(start = 8.dp)
                        .size(56.dp),
                ) {
                    Text(
                        if (slot.isPlaying) "❚❚" else "▶",
                        style = MaterialTheme.typography.titleLarge,
                    )
                }
            } else {
                FilledTonalButton(onClick = onPick) { Text("Load") }
            }
        }

        if (slot.isLoaded) {
            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                Text(
                    "Level",
                    style = MaterialTheme.typography.labelLarge,
                    modifier = Modifier.width(44.dp),
                )
                Slider(
                    value = slot.gain,
                    onValueChange = onGainChange,
                    valueRange = 0f..1f,
                    modifier = Modifier
                        .weight(1f)
                        .height(48.dp),
                )
            }
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                TextButton(onClick = onPick) { Text("Replace") }
                TextButton(onClick = onRemove) { Text("Remove") }
            }
        } else if (slot.isError) {
            TextButton(onClick = onPick) { Text("Try another file") }
        }
    }
}

private fun statusLine(slot: BackingTrackUiState): String = when (slot.state) {
    BackingTrackState.EMPTY -> "Empty — tap Load to add a WAV"
    BackingTrackState.LOADING -> "Loading…"
    BackingTrackState.READY -> lengthLabel(slot.lengthMs, "Ready")
    BackingTrackState.PLAYING -> lengthLabel(slot.lengthMs, "Playing")
    BackingTrackState.ENDED -> "Finished — tap play to restart"
    BackingTrackState.ERROR -> "Couldn't load — WAV (PCM16 / float32) only"
}

private fun lengthLabel(lengthMs: Int, prefix: String): String {
    if (lengthMs <= 0) return prefix
    val totalSec = lengthMs / 1000
    return "%s · %d:%02d".format(prefix, totalSec / 60, totalSec % 60)
}
