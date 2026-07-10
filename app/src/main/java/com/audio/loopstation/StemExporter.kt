package com.audio.loopstation

import android.content.Context
import java.io.BufferedInputStream
import java.io.BufferedOutputStream
import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/**
 * Packages the current session's audio into a single `Session_Stems.zip`
 * ready for a desktop DAW:
 *
 *  1. Asks the engine to flush + finalize any open capture file (ring
 *     buffers drained, RIFF/data sizes patched, handles closed).
 *  2. Asks the engine to write one IEEE-float32 .wav stem per non-empty
 *     loop track into a session directory.
 *  3. Collects every session .wav — the fresh stems plus the long-form
 *     capture takes — and zips them with [ZipOutputStream].
 *
 * The zip lands in the app's cache dir under `exports/`, which is mapped by
 * the FileProvider (res/xml/file_paths.xml) so it can be shared without any
 * storage permission. Everything runs on [Dispatchers.IO].
 */
class StemExporter(private val context: Context) {

    /**
     * Builds the zip and returns it, or null if there was nothing to export
     * or a step failed. Safe to call while loops are playing; not while
     * recording (the engine refuses and this returns null).
     */
    suspend fun exportSessionZip(
        engine: AudioEngine,
        sessionName: String = defaultSessionName(),
    ): File? = withContext(Dispatchers.IO) {
        // 1. Finalize spooled audio so every .wav on disk has valid headers.
        if (!engine.flushAndCloseSession()) return@withContext null

        // 2. One stem per loop track. A null here means the engine refused
        //    (recording in progress) or a disk write failed. Zero stems is
        //    fine — the session may be capture-takes only.
        val stemsDir = File(context.filesDir, "$STEMS_DIR/$sessionName").apply { mkdirs() }
        engine.exportStems(stemsDir.absolutePath) ?: return@withContext null

        // 3. Gather the session's .wav files, namespaced inside the zip.
        val entries = buildList {
            wavsIn(stemsDir).forEach { add("stems/${it.name}" to it) }
            wavsIn(File(context.filesDir, TAKES_DIR)).forEach { add("takes/${it.name}" to it) }
        }
        if (entries.isEmpty()) return@withContext null

        // 4. Zip into the FileProvider-visible cache area.
        val zip = File(File(context.cacheDir, EXPORT_DIR).apply { mkdirs() }, ZIP_NAME)
        zipTo(zip, entries)
        zip
    }

    private fun wavsIn(dir: File): List<File> =
        dir.listFiles { f: File -> f.isFile && f.extension.equals("wav", ignoreCase = true) }
            ?.sortedBy { it.name }
            .orEmpty()

    private fun zipTo(zip: File, entries: List<Pair<String, File>>) {
        ZipOutputStream(BufferedOutputStream(FileOutputStream(zip))).use { out ->
            for ((entryName, file) in entries) {
                out.putNextEntry(ZipEntry(entryName).apply { time = file.lastModified() })
                BufferedInputStream(FileInputStream(file)).use { input ->
                    input.copyTo(out, COPY_BUFFER_BYTES)
                }
                out.closeEntry()
            }
        }
    }

    companion object {
        const val ZIP_NAME = "Session_Stems.zip"
        private const val STEMS_DIR = "stems"
        private const val TAKES_DIR = "takes"  // where AudioEngine.newCaptureFile() writes
        private const val EXPORT_DIR = "exports"
        private const val COPY_BUFFER_BYTES = 64 * 1024

        private fun defaultSessionName(): String =
            "session_" + SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(Date())
    }
}
