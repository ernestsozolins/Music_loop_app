package com.audio.loopstation.ui

import android.content.Context
import android.media.midi.MidiDeviceInfo
import android.media.midi.MidiManager
import android.media.midi.MidiOutputPort
import android.media.midi.MidiReceiver
import android.os.Handler
import android.os.Looper

/**
 * Bridges MIDI foot controllers (BLE-MIDI or USB-MIDI, via the Android MIDI
 * API) to the transport intents — the complement to [PedalController], which
 * handles HID-keyboard pedals. Many worship/looper pedals speak MIDI rather
 * than emulating a keyboard, so both paths are worth having.
 *
 * Default map (a sensible starting point — real pedals vary, so this is the
 * layer to make user-configurable later):
 *   Note-on (velocity > 0) or CC (value >= 64):
 *     note%12 == 0  or CC 20  -> Play / Stop
 *     note%12 == 2  or CC 21  -> Overdub / Next
 *     note%12 == 4  or CC 22  -> Undo
 *     note%12 == 5  or CC 23  -> Metronome toggle
 * Program-change messages 0/1/2/3 map to the same four actions.
 *
 * Lifecycle: [start] attaches to every present MIDI device and to devices
 * added later; [stop] detaches. MIDI callbacks arrive on a binder thread, so
 * actions are marshalled to the main thread before touching the ViewModel.
 */
class MidiPedalController(
    private val context: Context,
    private val viewModel: MainViewModel,
) {
    private val main = Handler(Looper.getMainLooper())
    private var midiManager: MidiManager? = null
    private val openPorts = mutableListOf<MidiOutputPort>()

    private val deviceCallback = object : MidiManager.DeviceCallback() {
        override fun onDeviceAdded(info: MidiDeviceInfo) = attach(info)
    }

    fun start() {
        val mm = context.getSystemService(Context.MIDI_SERVICE) as? MidiManager ?: return
        midiManager = mm
        mm.registerDeviceCallback(deviceCallback, main)
        mm.devices.forEach(::attach)
    }

    fun stop() {
        midiManager?.unregisterDeviceCallback(deviceCallback)
        openPorts.forEach { runCatching { it.close() } }
        openPorts.clear()
        midiManager = null
    }

    private fun attach(info: MidiDeviceInfo) {
        val mm = midiManager ?: return
        // Only devices that can SEND to us (have output ports) are pedals.
        if (info.outputPortCount <= 0) return
        mm.openDevice(info, { device ->
            if (device == null) return@openDevice
            val port = device.openOutputPort(0) ?: return@openDevice
            port.connect(PedalReceiver())
            openPorts.add(port)
        }, main)
    }

    private inner class PedalReceiver : MidiReceiver() {
        override fun onSend(msg: ByteArray, offset: Int, count: Int, timestamp: Long) {
            var i = offset
            val end = offset + count
            while (i < end) {
                val status = msg[i].toInt() and 0xFF
                val type = status and 0xF0
                when (type) {
                    0x90 -> {  // note on
                        if (i + 2 < end) {
                            val note = msg[i + 1].toInt() and 0x7F
                            val velocity = msg[i + 2].toInt() and 0x7F
                            if (velocity > 0) fireForNote(note)
                            i += 3
                        } else return
                    }
                    0x80 -> i += 3  // note off: ignore
                    0xB0 -> {  // control change
                        if (i + 2 < end) {
                            val cc = msg[i + 1].toInt() and 0x7F
                            val value = msg[i + 2].toInt() and 0x7F
                            if (value >= 64) fireForCc(cc)
                            i += 3
                        } else return
                    }
                    0xC0 -> {  // program change
                        if (i + 1 < end) {
                            fireForProgram(msg[i + 1].toInt() and 0x7F)
                            i += 2
                        } else return
                    }
                    else -> i += 1  // unknown/realtime byte
                }
            }
        }
    }

    private fun fireForNote(note: Int) = when (note % 12) {
        0 -> post { viewModel.onPlayStopToggle() }
        2 -> post { viewModel.onOverdubPedal() }
        4 -> post { viewModel.onUndoPedal() }
        5 -> post { viewModel.onMetronomeToggle() }
        else -> {}
    }

    private fun fireForCc(cc: Int) = when (cc) {
        20 -> post { viewModel.onPlayStopToggle() }
        21 -> post { viewModel.onOverdubPedal() }
        22 -> post { viewModel.onUndoPedal() }
        23 -> post { viewModel.onMetronomeToggle() }
        else -> {}
    }

    private fun fireForProgram(program: Int) = when (program) {
        0 -> post { viewModel.onPlayStopToggle() }
        1 -> post { viewModel.onOverdubPedal() }
        2 -> post { viewModel.onUndoPedal() }
        3 -> post { viewModel.onMetronomeToggle() }
        else -> {}
    }

    private inline fun post(crossinline action: () -> Unit) {
        main.post { action() }
    }
}
