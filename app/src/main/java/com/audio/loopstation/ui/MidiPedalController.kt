package com.audio.loopstation.ui

import android.content.Context
import android.media.midi.MidiDevice
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
    // Ports are NOT enough: closing a port leaves the MidiDevice open, and the
    // activity re-binds this controller on every start/stop cycle (which on a
    // tablet means every rotation), so the devices would pile up.
    private val openDevices = mutableListOf<MidiDevice>()
    private val receiver = PedalReceiver()
    // openDevice() is async: its callback can land after stop(). Without this
    // the late port is added to a cleared list (leak) and keeps firing pedal
    // actions into a ViewModel whose engine is already detached.
    private var running = false

    private val deviceCallback = object : MidiManager.DeviceCallback() {
        override fun onDeviceAdded(info: MidiDeviceInfo) {
            attach(info)
        }

        override fun onDeviceRemoved(info: MidiDeviceInfo) {
            // Drop anything belonging to the unplugged device so a re-plug
            // doesn't stack a second receiver on the same pedal.
            val id = info.id
            openDevices.filter { it.info?.id == id }.forEach { device ->
                runCatching { device.close() }
                openDevices.remove(device)
            }
        }
    }

    fun start() {
        if (running) return  // idempotent: onStart can fire repeatedly
        val mm = context.getSystemService(Context.MIDI_SERVICE) as? MidiManager ?: return
        midiManager = mm
        running = true
        mm.registerDeviceCallback(deviceCallback, main)
        mm.devices.forEach(::attach)
    }

    fun stop() {
        running = false
        midiManager?.let { mm -> runCatching { mm.unregisterDeviceCallback(deviceCallback) } }
        openPorts.forEach { port ->
            runCatching { port.disconnect(receiver) }
            runCatching { port.close() }
        }
        openPorts.clear()
        openDevices.forEach { runCatching { it.close() } }
        openDevices.clear()
        midiManager = null
    }

    private fun attach(info: MidiDeviceInfo) {
        val mm = midiManager ?: return
        // Only devices that can SEND to us (have output ports) are pedals.
        if (info.outputPortCount <= 0) return
        if (openDevices.any { it.info?.id == info.id }) return  // already attached
        mm.openDevice(info, { device ->
            if (device == null) return@openDevice
            if (!running) {  // stop() won the race — don't leak the device
                runCatching { device.close() }
                return@openDevice
            }
            val port = device.openOutputPort(0)
            if (port == null) {
                runCatching { device.close() }
                return@openDevice
            }
            port.connect(receiver)
            openPorts.add(port)
            openDevices.add(device)
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
