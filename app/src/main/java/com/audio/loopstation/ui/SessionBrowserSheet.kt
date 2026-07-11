package com.audio.loopstation.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.audio.loopstation.data.SessionEntity
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * Bottom sheet listing saved sessions (newest first). Tapping one loads it
 * into the engine; each row also offers delete. The list is backed by a Room
 * Flow, so it refreshes automatically after every save or delete.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SessionBrowserSheet(
    sessions: List<SessionEntity>,
    onLoad: (Long) -> Unit,
    onDelete: (SessionEntity) -> Unit,
    onDismiss: () -> Unit,
) {
    ModalBottomSheet(onDismissRequest = onDismiss) {
        Text(
            "Sessions",
            style = MaterialTheme.typography.titleLarge,
            modifier = Modifier.padding(start = 16.dp, top = 8.dp, bottom = 8.dp),
        )
        if (sessions.isEmpty()) {
            Text(
                "No saved sessions yet — record a loop and tap Save.",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(16.dp),
            )
        } else {
            LazyColumn(Modifier.fillMaxWidth().padding(bottom = 24.dp)) {
                items(sessions, key = { it.id }) { session ->
                    ListItem(
                        headlineContent = { Text(session.name) },
                        supportingContent = {
                            Text(
                                "${session.bpm} BPM · ${session.timeSignature}/4 · " +
                                    dateFormat.format(Date(session.timestamp)),
                            )
                        },
                        trailingContent = {
                            Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                                TextButton(onClick = { onLoad(session.id) }) { Text("Load") }
                                TextButton(onClick = { onDelete(session) }) { Text("Delete") }
                            }
                        },
                    )
                    HorizontalDivider()
                }
            }
        }
    }
}

private val dateFormat = SimpleDateFormat("MMM d, HH:mm", Locale.US)
