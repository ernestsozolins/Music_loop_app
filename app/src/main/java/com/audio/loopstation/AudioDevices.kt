package com.audio.loopstation

import android.content.Context
import android.media.AudioDeviceInfo
import android.media.AudioManager

/** A selectable audio device (input or output) for the device pickers. */
data class AudioDeviceOption(
    val id: Int,       // AudioDeviceInfo id; 0 = system default
    val name: String,
) {
    companion object {
        /** The always-present "follow the system default" choice. */
        val SYSTEM_DEFAULT = AudioDeviceOption(0, "System default")
    }
}

/**
 * Enumerates the current audio OUTPUT devices (speaker, wired, Bluetooth,
 * USB, …) and INPUT devices (built-in mic, wired/USB/Bluetooth mics, the
 * Zoom H2n interface, …) for the pickers. Each list always starts with
 * [AudioDeviceOption.SYSTEM_DEFAULT] and is de-duplicated by id.
 */
object AudioDevices {

    fun outputs(context: Context): List<AudioDeviceOption> =
        enumerate(context, AudioManager.GET_DEVICES_OUTPUTS, sinks = true)

    fun inputs(context: Context): List<AudioDeviceOption> =
        enumerate(context, AudioManager.GET_DEVICES_INPUTS, sinks = false)

    private fun enumerate(
        context: Context,
        flag: Int,
        sinks: Boolean,
    ): List<AudioDeviceOption> {
        val am = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
        val out = mutableListOf(AudioDeviceOption.SYSTEM_DEFAULT)
        for (d in am.getDevices(flag)) {
            if (sinks && !d.isSink) continue
            if (!sinks && !d.isSource) continue
            val label = typeLabel(d.type) ?: continue
            val name = buildString {
                append(label)
                val product = d.productName?.toString()?.trim()
                if (!product.isNullOrEmpty() && !label.contains(product, ignoreCase = true)) {
                    append(" · ").append(product)
                }
            }
            out.add(AudioDeviceOption(d.id, name))
        }
        return out.distinctBy { it.id }
    }

    private fun typeLabel(type: Int): String? = when (type) {
        AudioDeviceInfo.TYPE_BUILTIN_SPEAKER -> "Speaker"
        AudioDeviceInfo.TYPE_BUILTIN_EARPIECE -> "Earpiece"
        AudioDeviceInfo.TYPE_BUILTIN_MIC -> "Built-in mic"
        AudioDeviceInfo.TYPE_WIRED_HEADPHONES -> "Wired headphones"
        AudioDeviceInfo.TYPE_WIRED_HEADSET -> "Wired headset"
        AudioDeviceInfo.TYPE_BLUETOOTH_A2DP -> "Bluetooth"
        AudioDeviceInfo.TYPE_BLUETOOTH_SCO -> "Bluetooth (call)"
        // Bluetooth LE Audio (API 31/33) — many current earbuds/speakers use
        // this instead of A2DP; without these they never show in the picker.
        AudioDeviceInfo.TYPE_BLE_HEADSET -> "Bluetooth LE"
        AudioDeviceInfo.TYPE_BLE_SPEAKER -> "Bluetooth LE speaker"
        AudioDeviceInfo.TYPE_BLE_BROADCAST -> "Bluetooth LE broadcast"
        AudioDeviceInfo.TYPE_USB_DEVICE -> "USB audio"
        AudioDeviceInfo.TYPE_USB_HEADSET -> "USB headset"
        AudioDeviceInfo.TYPE_USB_ACCESSORY -> "USB accessory"
        AudioDeviceInfo.TYPE_HDMI -> "HDMI"
        AudioDeviceInfo.TYPE_HEARING_AID -> "Hearing aid"
        AudioDeviceInfo.TYPE_DOCK -> "Dock"
        AudioDeviceInfo.TYPE_LINE_ANALOG, AudioDeviceInfo.TYPE_LINE_DIGITAL -> "Line"
        else -> null  // telephony, remote-submix, unknown → not a user pick
    }
}
