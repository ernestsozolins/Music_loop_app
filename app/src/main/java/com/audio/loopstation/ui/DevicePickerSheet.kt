package com.audio.loopstation.ui

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Check
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.audio.loopstation.AudioDeviceOption

/**
 * Generic audio-device picker used for both the playback and capture device
 * sheets. Lists the current devices with a checkmark on the active one;
 * tapping routes there by reopening the streams. Split-clock rigs (USB mic
 * in / Bluetooth out) are fully supported — the engine's drift-correcting
 * ring absorbs the two clocks, and latency calibration compensates for
 * Bluetooth delay.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun DevicePickerSheet(
    title: String,
    subtitle: String,
    devices: List<AudioDeviceOption>,
    selectedId: Int,
    onSelect: (Int) -> Unit,
    onDismiss: () -> Unit,
) {
    ModalBottomSheet(onDismissRequest = onDismiss) {
        Text(
            title,
            style = MaterialTheme.typography.titleLarge,
            modifier = Modifier.padding(start = 16.dp, top = 8.dp, bottom = 4.dp),
        )
        Text(
            subtitle,
            style = MaterialTheme.typography.labelMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(start = 16.dp, bottom = 8.dp),
        )
        LazyColumn(Modifier.fillMaxWidth().padding(bottom = 24.dp)) {
            items(devices, key = { it.id }) { device ->
                ListItem(
                    headlineContent = { Text(device.name) },
                    trailingContent = {
                        if (device.id == selectedId) {
                            Icon(Icons.Filled.Check, contentDescription = "Selected")
                        }
                    },
                    modifier = Modifier.clickable { onSelect(device.id) },
                )
            }
        }
    }
}
