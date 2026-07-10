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
- [ ] **Phase 4 — Compose UI**: transport controls, track strip, live
      waveform; tablet-first adaptive layout (window size classes) for
      Tab S9 Ultra-class devices, usable down to small phones.
- [ ] **Phase 5 — Latency calibration** (loopback measurement feeding
      `setRecordOffsetFrames`), persistence, export.

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
