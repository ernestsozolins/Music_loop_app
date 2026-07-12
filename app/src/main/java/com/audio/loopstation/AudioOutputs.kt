package com.audio.loopstation

import android.content.Context
import android.media.AudioDeviceInfo
import android.media.AudioManager

/** A selectable playback destination for the output-device picker. */
data class OutputDevice(
    val id: Int,       // AudioDeviceInfo id; 0 = system default
    val name: String,
) {
    companion object {
        /** The always-present "follow the system default" choice. */
        val SYSTEM_DEFAULT = OutputDevice(0, "System default")
    }
}

/**
 * Enumerates the current audio OUTPUT devices (speaker, wired headset,
 * Bluetooth, USB, …) for the picker, newest routing reflected each call.
 * Always includes [OutputDevice.SYSTEM_DEFAULT] first. Duplicate/among-type
 * entries are de-duplicated by id.
 */
object AudioOutputs {

    fun list(context: Context): List<OutputDevice> {
        val am = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
        val devices = am.getDevices(AudioManager.GET_DEVICES_OUTPUTS)
        val out = mutableListOf(OutputDevice.SYSTEM_DEFAULT)
        for (d in devices) {
            if (!d.isSink) continue
            // Skip telephony/aux types that aren't meaningful playback picks.
            val label = typeLabel(d.type) ?: continue
            val name = buildString {
                append(label)
                val product = d.productName?.toString()?.trim()
                if (!product.isNullOrEmpty() && !label.contains(product, ignoreCase = true)) {
                    append(" · ").append(product)
                }
            }
            out.add(OutputDevice(d.id, name))
        }
        return out.distinctBy { it.id }
    }

    private fun typeLabel(type: Int): String? = when (type) {
        AudioDeviceInfo.TYPE_BUILTIN_SPEAKER -> "Speaker"
        AudioDeviceInfo.TYPE_BUILTIN_EARPIECE -> "Earpiece"
        AudioDeviceInfo.TYPE_WIRED_HEADPHONES -> "Wired headphones"
        AudioDeviceInfo.TYPE_WIRED_HEADSET -> "Wired headset"
        AudioDeviceInfo.TYPE_BLUETOOTH_A2DP -> "Bluetooth"
        AudioDeviceInfo.TYPE_BLUETOOTH_SCO -> "Bluetooth (call)"
        AudioDeviceInfo.TYPE_USB_DEVICE -> "USB audio"
        AudioDeviceInfo.TYPE_USB_HEADSET -> "USB headset"
        AudioDeviceInfo.TYPE_USB_ACCESSORY -> "USB accessory"
        AudioDeviceInfo.TYPE_HDMI -> "HDMI"
        AudioDeviceInfo.TYPE_HEARING_AID -> "Hearing aid"
        AudioDeviceInfo.TYPE_DOCK -> "Dock"
        AudioDeviceInfo.TYPE_LINE_ANALOG, AudioDeviceInfo.TYPE_LINE_DIGITAL -> "Line out"
        else -> null  // telephony, remote-submix, unknown → not a user pick
    }
}
