# Music Loop App

A low-latency, multi-track overdubbing looper for Android, optimized for
professional USB audio interfaces (e.g. Zoom H2n) — including split-clock
setups such as USB mic in / Bluetooth out.

## Architecture

| Layer | Technology | Notes |
|---|---|---|
| Audio engine | Google Oboe (C++17) via JNI | Synchronized **dual-stream** setup (independent input & output streams), not forced full-duplex, for stability across differing hardware clocks |
| Clock-drift protection | Lock-free SPSC look-ahead ring buffer | Output callback defends a latency cushion; bounded frame slipping absorbs sample-rate drift between clock domains |
| UI | Jetpack Compose (Kotlin) | Polls meter data at 60 Hz; never touches the audio thread |
| Waveform data | Per-block RMS/peak in the C++ callback | Published through a lock-free SPSC queue (`WaveformPoint`) |

### Realtime rules (enforced in `onAudioReady`)

No heap allocation, no disk I/O, no JNI, no mutexes, no logging, no unbounded
work. Every buffer is pre-allocated and pre-touched on the control thread
before the streams start. Even track clearing is amortized across callbacks
in bounded chunks.

### Data flow

```
USB mic ──> input stream callback ──> SpscSampleRing ──┐   (drift-corrected,
                                                       ▼    look-ahead cushion)
                              output stream callback: pull input ─> mix loop
                              tracks ─> record/overdub ─> meters ─> speaker/BT
                                                       │
                                    SpscQueue<WaveformPoint> ──> Compose UI @60Hz
```

## Roadmap

- [x] **Phase 1 — Core engine & ring buffer** (`app/src/main/cpp/AudioEngine.{h,cpp}`):
      dual Oboe streams (Shared / LowLatency / Float), lock-free looper +
      overdub mixer, drift-corrected input path, RMS/peak metering.
- [x] **Phase 2 — Metronome & JNI bridge** (`Metronome.h`,
      `AudioEngine_JNI.cpp`, `AudioEngine.kt`): sample-accurate synthesized
      click (downbeat 1500 Hz / offbeat 800 Hz, output-path only), lock-free
      mid-song tempo changes, JNI control surface, 60 Hz meter-polling
      coroutine, device-class-scaled engine config (tablet vs phone).
- [x] **Phase 3 — Disk spooling & file management** (`DiskSpooler.{h,cpp}`):
      DiskWriter/DiskReader background threads behind lock-free SPSC rings;
      long-form capture appended to float32 .wav while recording; backing
      tracks streamed from .wav (float32/PCM16) into the output mix; the
      audio callback never touches a file. Plus: metronome phase-lock to the
      loop (loop start = bar 1 beat 1, re-anchored every wrap).
- [x] **Phase 3.5 — Engine safety & latency calibration**
      (`LatencyCalibrator.h`): `onError` disconnect protection (transport
      paused, capture finalized, event pushed to Kotlin, automatic device
      restart); explicit same-rate + resampler policy on both streams; and
      ping-and-listen round-trip calibration that feeds the overdub record
      offset automatically.
- [x] **Phase 4 — Track export & file packaging**: `flushAndCloseSession()`
      (synchronous ring flush + RIFF/data size patching + handle close),
      per-track float32 stem export on a worker thread, `StemExporter.kt`
      zipping stems + takes into `Session_Stems.zip`, and the FileProvider +
      ACTION_SEND share flow (`ui/ExportStemsButton.kt`).
- [x] **Phase 4.5 — Compose multi-track UI** (`ui/MainViewModel.kt`,
      `MainScreen.kt`, `TransportBar.kt`, `TrackRow.kt`, `MainActivity.kt`):
      StateFlow-driven Material 3 UI — fixed transport bar (record / play /
      stop / metronome / BPM / export), LazyColumn track grid with
      mute/solo/volume/pan and the waveform area, keep-screen-on wake lock,
      mic-permission flow. Engine gained per-track balance pan and a packed
      track-content mask for one-call polling; solo is a ViewModel-side
      mute matrix.
- [x] **Phase 4.8 — Foreground service & system lifecycles**
      (`MediaRecordingService.kt`): the service owns the engine (and its
      meter polling); the UI binds in onStart/unbinds in onStop and the
      ViewModel attaches/detaches — UI death never touches the audio thread.
      Foreground promotion (microphone type, ongoing notification with Stop
      action) while the transport runs or loops are held in memory; audio
      focus (AUDIOFOCUS_GAIN) with call-safe loss handling: transport
      parked, take finalized on disk, streams released to telephony, and
      playback-only resume on focus return.
- [x] **Phase 5 — Session persistence (Room)** (`data/`): SessionEntity +
      TrackEntity (FK, cascade) with Flow-based DAO queries and a
      SessionRepository owning the two-step save protocol (row first for the
      stem directory, then stems, then an atomic metadata+tracks commit).
      Engine gained the inverse of stem export — `restoreSession()` loads
      saved .wavs back into the loop tracks on a worker thread and commits
      loop length/content flags on the audio thread. MainViewModel restores
      the last active session on startup (mix params via the JNI setters,
      audio via the restore worker) and exposes `saveSession()`.
- [x] **Phase 6 — Hardware control & UI polish** (`ui/PedalController.kt`,
      `ui/WaveformVisualizer.kt`): Bluetooth foot-pedal support via
      activity-level key interception (Space = play/stop, Enter =
      overdub + auto-advance, Backspace = undo last track / cancel take,
      PageUp/Down = track select; auto-repeat swallowed) with a ViewModel
      undo stack; 60 fps waveform + playhead rendering with
      draw-phase-only invalidation (produceState + frame clock + meter
      ballistics) — zero recompositions while audio runs.
- [x] **Phase 7 — Output DSP (monitoring reverb)** (`Reverb.{h,cpp}`):
      Freeverb-style Schroeder tank (8 damped combs + 4 allpasses per
      channel, detuned right tank, denormal-flushed, block-ramped dry/wet)
      applied to the headphone mix only — capture tees, the overdub path,
      and stem export are all upstream, so recordings stay 100% dry.
      Lock-free RoomSize / DryWetMix / Damping / Enabled controls via JNI.
- [x] **Phase 8 — Build wiring & polish**: full Gradle project (AGP 8.7,
      Kotlin 2.0 + Compose plugin, Room via KSP, Oboe via Prefab, NDK/CMake,
      pinned Gradle 8.10.2 wrapper); reverb controls in the UI
      (`MonitorFxPanel`); per-track offline waveforms (native peak-bin
      scan + `StaticWaveform`); per-pass undo via native track snapshots
      (Backspace restores an overdub's previous layers instead of wiping
      the track); window-size-class two-pane grid on expanded widths.

## Building & installing

Prerequisites: [Android Studio](https://developer.android.com/studio)
(Ladybug or newer). It installs the matching SDK; accept the prompts for
**NDK (Side by side)** and **CMake 3.22.1** on first sync (or add them via
Tools → SDK Manager → SDK Tools).

1. `git clone` this repository and check out this branch.
2. **File → Open** the repository root in Android Studio and let Gradle
   sync (first sync downloads Compose/Room/Oboe and builds
   `liblooperengine.so` via CMake).
3. On the tablet: **Settings → About device → Software information → tap
   "Build number" seven times** to unlock Developer options, then enable
   **Settings → Developer options → USB debugging**.
4. Connect the tablet by USB (or pair via Developer options → Wireless
   debugging), accept the RSA fingerprint dialog, pick the device in the
   toolbar, press **Run ▶**.
5. First launch: grant **microphone** (and notifications). Plug the USB
   audio interface into the tablet's USB-C port; Android routes audio to it
   automatically. Run latency calibration once with headphones near the mic
   (or a loopback cable), pair the Bluetooth pedal, and play.

CLI alternative: `./gradlew installDebug` with the tablet connected (set
`sdk.dir` in `local.properties` if `ANDROID_HOME` is unset).

## Engine configuration

`AudioEngine.kt#create()` scales the config by device class:

| Knob | Phone | Tablet | Tab S9 Ultra class | Notes |
|---|---|---|---|---|
| Tracks | 4 | 6 | 8 | Compile-time max 8 |
| Max loop length | 30 s | 60 s | 120 s | Pre-allocated: `tracks × seconds × rate × ch × 4 B` |
| Sample rate | 48 kHz | ← | ← | Engine-canonical; Oboe SRC pins both streams to it |
| Channels | 2 | ← | ← | Interleaved float; H2n presents a stereo capture device |
| Look-ahead cushion | 15 ms | ← | ← | Lower (e.g. 5 ms) for same-clock USB duplex rigs |
| Drift slack | 5 ms | ← | ← | Deviation tolerated before frame slipping kicks in |
| Monitor gain | 0 | ← | ← | Hardware monitoring through the interface assumed; raise for software monitoring |

### Metronome

Fully synthesized in the callback (no sample assets): a sine burst with an
exponential decay envelope (~45 ms to −80 dB), downbeat at 1500 Hz, offbeats
at 800 Hz, onset click-free because the sine starts at phase 0. Tempo and
time signature are packed into a single 64-bit atomic; changes land at the
**next beat boundary**, so mid-bar BPM edits never glitch the phase. The
click is mixed exclusively into the output buffer, after the record path has
consumed the input — it can never end up on a recorded track or in a
captured file.

**Phase lock (default ON):** while a loop plays or is overdubbed, the loop
start is treated as bar 1 beat 1 and the beat grid re-anchors at every loop
wrap, so the click never drifts against an unquantized loop. Disable via
`setMetronomeSyncToLoop(false)` for a free-running click.

### Disk spooling

Long-form material never lives in RAM. Two background threads sit behind
lock-free SPSC rings; the audio callback only ever touches the rings:

- **DiskWriter** — `startCapture(path)` tees the drift-corrected input (or
  the full pre-metronome mix) into a ~2.7 s ring; the writer appends it to a
  float32 `.wav` and patches the RIFF header on `stopCapture()`. Interrupted
  takes (unpatched headers) are still readable back.
- **DiskReader** — up to 2 backing-track slots stream `.wav` files
  (float32/PCM16, mono/stereo, 48 kHz) through ~1.4 s read-ahead rings into
  the output mix. Open/play/pause/close/loop/gain per slot; backing audio is
  mixed after the overdub path, so it is never recorded into loop tracks.

Capture files live in app-private storage (`AudioEngine.newCaptureFile(context)`
— no storage permission required).

### Export

All engine audio is written as **IEEE 32-bit float WAV (format tag 3, with a
`fact` chunk)** — no 16-bit truncation, so hot stems can be mixed down in a
DAW without digital clipping. `flushAndCloseSession()` drains the lock-free
rings to disk, patches the RIFF and `data` chunk sizes, and closes handles
(blocking, bounded by a timeout). `exportStems(dir)` writes one .wav per
non-empty loop track on a worker thread; record-arm and clear commands are
frozen while it reads the track buffers (playback stays live). On the Kotlin
side, `StemExporter` zips stems + capture takes into `Session_Stems.zip`
(cache dir, FileProvider-mapped) and `ExportStemsButton` hands it to the
system share sheet via `ACTION_SEND`.

### Monitoring reverb

Dry headphone monitoring feels sterile, so a lightweight Freeverb-style
Schroeder reverberator (8 parallel damped comb filters + 4 series allpass
diffusers per channel, right tank detuned for stereo, no convolution, no
third-party code) runs on the **output mix only**, between the mix-capture
tee and the metronome. Everything recordable is tapped upstream — takes,
loop tracks, and exported stems are 100% dry by construction, and the click
stays un-reverberated. RoomSize / DryWetMix / Damping / Enabled are
lock-free atomics (dry/wet ramped per block, so slider moves never click);
the default mix is 0 — fully dry until the user opts in.

### Engine safety

Both streams implement `onError`: on device disconnect (USB interface or
Bluetooth sink unplugged) the engine immediately pauses the transport,
finalizes any capture file so the take on disk stays valid, pushes a
thread-safe event to Kotlin (`AudioEngine.events` SharedFlow), then attempts
an automatic restart on the current default devices and reports the outcome.
Both stream builders request the engine rate explicitly with Oboe's internal
resampler enabled (`SampleRateConversionQuality::Medium`), so hardware that
only runs at another native rate is resampled instead of pitch-shifted.

### Latency calibration

`calibrateLatency()` (suspend fun in Kotlin) runs ping-and-listen: three
2 ms Hann-windowed 3 kHz sine bursts are injected into the output; the input
feed is scanned for each return against a noise-floor-calibrated threshold.
Measured on one output-frame timeline, the delta captures the full recording
round trip (output buffer → DAC → air/cable → ADC → resampler → input ring
cushion). Two pings must agree within 1 ms; the result is applied as the
overdub record offset, so overdubs land exactly where the performer heard
the loop. Failure (noisy room, no loopback path) is reported cleanly and
never overwrites the last good value.
