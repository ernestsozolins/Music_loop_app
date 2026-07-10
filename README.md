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
- [ ] **Phase 2 — JNI bridge + Kotlin/Compose UI** (transport controls, track
      strip, live waveform).
- [ ] **Phase 3 — Latency calibration** (loopback measurement feeding
      `setRecordOffsetFrames`), persistence, export.

## Engine configuration (Phase 1 defaults)

| Knob | Default | Notes |
|---|---|---|
| Sample rate | 48 kHz | Engine-canonical; Oboe SRC pins both streams to it |
| Channels | 2 | Interleaved float; H2n presents a stereo capture device |
| Tracks | 4 | Compile-time max 8 |
| Max loop length | 30 s | Pre-allocated: `tracks × seconds × rate × ch × 4 B` ≈ 46 MB at defaults |
| Look-ahead cushion | 15 ms | Lower (e.g. 5 ms) for same-clock USB duplex rigs |
| Drift slack | 5 ms | Deviation tolerated before frame slipping kicks in |
