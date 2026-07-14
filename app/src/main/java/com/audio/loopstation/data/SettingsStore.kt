package com.audio.loopstation.data

import android.content.Context

/**
 * Lightweight persistence for the transport / monitoring / FX settings so they
 * survive an app restart (recorded audio is persisted separately by the Room
 * session layer). Backed by SharedPreferences; writes are async (apply()).
 */
class SettingsStore(context: Context) {

    private val prefs = context.getSharedPreferences("loopstation_settings", Context.MODE_PRIVATE)

    data class Snapshot(
        val bpm: Int = 120,
        val beatsPerMeasure: Int = 4,
        val countIn: Boolean = true,
        val countInBars: Int = 1,
        val quantize: Boolean = true,
        val syncToLoop: Boolean = true,
        val singleMode: Boolean = false,
        val rhythmBeat: Boolean = false,
        val monitorLevel: Float = 0f,
        val reverbMix: Float = 0f,
        val reverbRoom: Float = 0.5f,
        val filterEnabled: Boolean = false,
        val filterCutoffNorm: Float = 0.65f,
        val filterResonance: Float = 0.2f,
        val filterMode: Int = 0,
        val droneHz: Float = 220f,
        val droneGain: Float = 0.5f,
        val manualLatencyMs: Int = 0,
    )

    fun load(): Snapshot {
        val d = Snapshot()
        return Snapshot(
            bpm = prefs.getInt("bpm", d.bpm),
            beatsPerMeasure = prefs.getInt("beats", d.beatsPerMeasure),
            countIn = prefs.getBoolean("countIn", d.countIn),
            countInBars = prefs.getInt("countInBars", d.countInBars),
            quantize = prefs.getBoolean("quantize", d.quantize),
            syncToLoop = prefs.getBoolean("syncToLoop", d.syncToLoop),
            singleMode = prefs.getBoolean("singleMode", d.singleMode),
            rhythmBeat = prefs.getBoolean("rhythmBeat", d.rhythmBeat),
            monitorLevel = prefs.getFloat("monitorLevel", d.monitorLevel),
            reverbMix = prefs.getFloat("reverbMix", d.reverbMix),
            reverbRoom = prefs.getFloat("reverbRoom", d.reverbRoom),
            filterEnabled = prefs.getBoolean("filterEnabled", d.filterEnabled),
            filterCutoffNorm = prefs.getFloat("filterCutoffNorm", d.filterCutoffNorm),
            filterResonance = prefs.getFloat("filterResonance", d.filterResonance),
            filterMode = prefs.getInt("filterMode", d.filterMode),
            droneHz = prefs.getFloat("droneHz", d.droneHz),
            droneGain = prefs.getFloat("droneGain", d.droneGain),
            manualLatencyMs = prefs.getInt("manualLatencyMs", d.manualLatencyMs),
        )
    }

    fun save(s: Snapshot) {
        prefs.edit()
            .putInt("bpm", s.bpm)
            .putInt("beats", s.beatsPerMeasure)
            .putBoolean("countIn", s.countIn)
            .putInt("countInBars", s.countInBars)
            .putBoolean("quantize", s.quantize)
            .putBoolean("syncToLoop", s.syncToLoop)
            .putBoolean("singleMode", s.singleMode)
            .putBoolean("rhythmBeat", s.rhythmBeat)
            .putFloat("monitorLevel", s.monitorLevel)
            .putFloat("reverbMix", s.reverbMix)
            .putFloat("reverbRoom", s.reverbRoom)
            .putBoolean("filterEnabled", s.filterEnabled)
            .putFloat("filterCutoffNorm", s.filterCutoffNorm)
            .putFloat("filterResonance", s.filterResonance)
            .putInt("filterMode", s.filterMode)
            .putFloat("droneHz", s.droneHz)
            .putFloat("droneGain", s.droneGain)
            .putInt("manualLatencyMs", s.manualLatencyMs)
            .apply()
    }
}
