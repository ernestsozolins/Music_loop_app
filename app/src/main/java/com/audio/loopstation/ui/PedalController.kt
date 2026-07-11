package com.audio.loopstation.ui

import android.view.KeyEvent

/**
 * Maps hardware key events from a Bluetooth foot pedal (pedals present
 * themselves as HID keyboards and send ordinary keystrokes) onto transport
 * intents:
 *
 *   Space / Play-Pause key -> toggle Play / Stop
 *   Enter                  -> overdub trigger: start a pass; pressing again
 *                             ends it and auto-advances to the next track
 *   Backspace (KEYCODE_DEL)-> undo the last recorded track (cancels the
 *                             take in progress if one is running)
 *   PageDown / PageUp      -> select next / previous track (the default
 *                             keys many page-turner pedals ship with)
 *
 * Interception happens at the ACTIVITY level (Activity.onKeyDown), not via
 * a Compose focus modifier: a stage controller must keep working no matter
 * which composable currently holds focus — a dialog or focus change must
 * never silently disconnect the pedal. Key REPEATS are swallowed: a held
 * pedal switch auto-repeats like a held keyboard key, and "stop, start,
 * stop, start…" mid-performance would be catastrophic.
 */
class PedalController(private val viewModel: MainViewModel) {

    /** Call from Activity.onKeyDown. Returns true when the key was consumed. */
    fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        if (!isPedalKey(keyCode)) return false
        if (event.repeatCount > 0) return true  // swallow auto-repeat

        when (keyCode) {
            KeyEvent.KEYCODE_SPACE,
            KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE,
            -> viewModel.onPlayStopToggle()

            KeyEvent.KEYCODE_ENTER,
            KeyEvent.KEYCODE_NUMPAD_ENTER,
            -> viewModel.onOverdubPedal()

            KeyEvent.KEYCODE_DEL -> viewModel.onUndoPedal()  // Android's backspace

            KeyEvent.KEYCODE_PAGE_DOWN -> viewModel.onSelectNextTrack()
            KeyEvent.KEYCODE_PAGE_UP -> viewModel.onSelectPreviousTrack()
        }
        return true
    }

    private fun isPedalKey(keyCode: Int): Boolean = when (keyCode) {
        KeyEvent.KEYCODE_SPACE,
        KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE,
        KeyEvent.KEYCODE_ENTER,
        KeyEvent.KEYCODE_NUMPAD_ENTER,
        KeyEvent.KEYCODE_DEL,
        KeyEvent.KEYCODE_PAGE_DOWN,
        KeyEvent.KEYCODE_PAGE_UP,
        -> true
        else -> false
    }
}
