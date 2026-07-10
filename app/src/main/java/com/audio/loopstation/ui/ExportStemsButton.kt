package com.audio.loopstation.ui

import android.content.Context
import android.content.Intent
import androidx.compose.material3.Button
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.core.content.FileProvider
import com.audio.loopstation.AudioEngine
import com.audio.loopstation.StemExporter
import java.io.File
import kotlinx.coroutines.launch

/**
 * Export flow: finalize the session's .wav files, zip them, and hand the
 * zip to the Android share sheet (Nearby Share / Drive / email / ...).
 * The zip is exposed through FileProvider — see res/xml/file_paths.xml and
 * the <provider> entry in AndroidManifest.xml — so receiving apps get a
 * temporary content:// read grant instead of a raw file path.
 */
@Composable
fun ExportStemsButton(
    engine: AudioEngine,
    modifier: Modifier = Modifier,
    onFailure: () -> Unit = {},  // hook a snackbar here in the full UI
) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var exporting by remember { mutableStateOf(false) }

    Button(
        modifier = modifier,
        enabled = !exporting,
        onClick = {
            exporting = true
            scope.launch {
                val zip = StemExporter(context).exportSessionZip(engine)
                exporting = false
                if (zip != null) shareSessionZip(context, zip) else onFailure()
            }
        },
    ) {
        Text(if (exporting) "Exporting…" else "Export stems")
    }
}

/** Fires ACTION_SEND for the zip via FileProvider. */
fun shareSessionZip(context: Context, zip: File) {
    val uri = FileProvider.getUriForFile(
        context,
        "${context.packageName}.fileprovider",  // matches android:authorities in the manifest
        zip,
    )
    val send = Intent(Intent.ACTION_SEND).apply {
        type = "application/zip"
        putExtra(Intent.EXTRA_STREAM, uri)
        putExtra(Intent.EXTRA_SUBJECT, zip.name)
        // Grants the receiving app temporary read access to the content Uri.
        addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
    }
    context.startActivity(Intent.createChooser(send, "Share session stems"))
}
