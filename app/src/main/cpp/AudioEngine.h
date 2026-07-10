#ifndef LOOPER_AUDIO_ENGINE_H
#define LOOPER_AUDIO_ENGINE_H

/*
 * AudioEngine — Phase 1 core of the low-latency overdub looper.
 *
 * Architecture (see README.md):
 *  - Two independent Oboe streams (input + output), each with its own
 *    realtime callback, instead of a forced full-duplex stream. This stays
 *    stable when the two directions live on different hardware clocks
 *    (e.g. USB interface in, Bluetooth or built-in speaker out).
 *  - A lock-free SPSC ring buffer carries input frames from the input
 *    callback (producer) to the output callback (consumer). The consumer
 *    maintains a look-ahead cushion and performs drift correction by
 *    slipping (dropping/duplicating) a bounded number of frames per block.
 *  - All looper state transitions happen on the audio thread, driven by a
 *    lock-free command queue fed from the control (UI/JNI) thread.
 *  - Per-block RMS/peak meters are published through a lock-free SPSC queue
 *    that the UI polls at its own rate (e.g. 60 Hz).
 *
 * Realtime rules enforced in every audio callback:
 *  NO heap allocation, NO disk I/O, NO JNI, NO mutexes, NO logging,
 *  NO unbounded loops. All buffers are pre-allocated and pre-touched on the
 *  control thread before the streams start.
 *
 * Threading contract:
 *  - Lifecycle + control methods (start/stop/toggleRecord/...) must be
 *    called from ONE non-realtime thread (the JNI/main thread).
 *  - readWaveform() must be called from ONE reader thread (the UI poller).
 *  - Observability getters are safe from any thread.
 */

#include <cstring>  // before Oboe.h: oboe/FullDuplexStream.h uses memset without including it

#include <oboe/Oboe.h>

#include "DiskSpooler.h"
#include "LockFreeRing.h"
#include "Metronome.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace looper {

// ---------------------------------------------------------------------------
// Compile-time limits. Stack arrays inside the audio callback are sized from
// these, so they are constants rather than Config fields.
// ---------------------------------------------------------------------------
constexpr int32_t kMaxTracks = 8;
constexpr int32_t kMaxCallbackFrames = 8192;  // defensive bound per data callback
constexpr int32_t kDeclickFrames = 128;       // ~2.7 ms fade-in after a splice @48k
constexpr int32_t kMaxDriftSlipFrames = 2;    // gentle drift: frames slipped per block
constexpr int32_t kClearChunkFrames = 16384;  // amortized track-clear per callback
constexpr int32_t kMaxCommandsPerBlock = 8;   // bounded command drain per callback

// The SPSC ring/queue primitives live in LockFreeRing.h (shared with the
// disk spooler).

// ---------------------------------------------------------------------------
// Public data types
// ---------------------------------------------------------------------------

enum class EngineState : uint8_t {
  Idle = 0,         // no master loop yet; input is monitored/metered only
  RecordingMaster,  // recording the loop that defines the loop length
  Playing,          // loop playback, input monitored
  Overdubbing,      // loop playback + summing input into the armed track
  Stopped,          // loop retained, transport halted, input still monitored
};

// One meter datum per output callback block. The UI drains these at ~60 Hz
// and downsamples/aggregates however it likes for the waveform display.
struct WaveformPoint {
  float inputRms;   // post-ring input block (exactly what is being recorded)
  float inputPeak;
  float mixRms;     // final output mix block
  float mixPeak;
  int32_t playheadFrames;    // playback position (or record length while recording)
  int32_t loopLengthFrames;  // 0 until the master loop is closed
  uint8_t state;             // EngineState at publish time
};

// UI-facing snapshot of one track's parameters.
struct TrackState {
  float gain = 1.0f;
  bool muted = false;
  bool hasContent = false;
  bool clearing = false;  // true while the amortized clear is still running
};

// ---------------------------------------------------------------------------
// AudioEngine
// ---------------------------------------------------------------------------
class AudioEngine {
 public:
  // Scale trackCount/maxLoopSeconds by device class — large tablets (e.g.
  // Galaxy Tab S9 Ultra) can afford 8 tracks x 120 s, small phones should
  // stay near the defaults. AudioEngine.kt picks these at create() time.
  struct Config {
    int32_t sampleRate = 48000;   // engine-canonical rate; Oboe SRC pins both streams to it
    int32_t channelCount = 2;     // interleaved channels on both streams (H2n is stereo)
    int32_t trackCount = 4;       // <= kMaxTracks
    int32_t maxLoopSeconds = 30;  // per-track pre-allocation: rate*seconds*channels*4 bytes
    int32_t lookAheadMillis = 15; // input cushion the drift logic defends
    int32_t driftSlackMillis = 5; // tolerated deviation before slipping frames
    int32_t inputDeviceId = oboe::kUnspecified;   // e.g. the USB interface's capture device
    int32_t outputDeviceId = oboe::kUnspecified;
  };

  AudioEngine();  // default Config
  explicit AudioEngine(const Config& config);
  ~AudioEngine();

  AudioEngine(const AudioEngine&) = delete;
  AudioEngine& operator=(const AudioEngine&) = delete;

  // ----- Lifecycle (control thread) -----
  oboe::Result start();
  void stop();
  bool isRunning() const { return mUserRunning.load(std::memory_order_acquire); }

  // ----- Transport (control thread; lock-free hand-off to the audio thread) -----
  // Idle -> record master loop | RecordingMaster -> close loop, play |
  // Playing -> overdub armed track | Overdubbing -> back to playing.
  void toggleRecord();
  void startRecording();  // idempotent: no-op if already recording/overdubbing
  void stopRecording();   // closes the master loop or ends the overdub pass
  void play();            // (re)start playback from the top of the loop
  void stopPlayback();    // halt transport; cancels a master recording in progress
  void clearAll();        // drop the loop and schedule all tracks for clearing
  void clearTrack(int32_t track);

  // ----- Metronome (control thread; single-atomic hand-off, applied at the
  // next beat boundary — see Metronome.h) -----
  void setMetronomeState(bool active, float bpm, int32_t beatsPerMeasure);
  void setMetronomeGain(float gain);
  // Phase-lock the metronome to the loop: while a loop is playing or being
  // overdubbed, the loop start is treated as bar 1 beat 1 and the beat grid
  // re-anchors on every loop wrap (default ON — a free-running click drifts
  // against an unquantized loop). While recording the master loop, or when
  // no loop exists, the metronome free-runs regardless.
  void setMetronomeSyncToLoop(bool enabled);
  bool metronomeActive() const { return mMetronome.isActive(); }
  uint32_t metronomeBeatCount() const { return mMetronome.beatCount(); }
  int32_t metronomeBeatInBar() const { return mMetronome.beatInBar(); }

  // ----- Disk spooling (control thread; async — see DiskSpooler.h) -----
  // Spools the recorded input (or the full mix, pre-metronome, when
  // captureMix is true) to a float32 .wav via the DiskWriter thread.
  void startCapture(const std::string& path, bool captureMix = false);
  void stopCapture();
  bool isCapturing() const { return mSpooler.isCapturing(); }
  int64_t capturedFrames() const { return mSpooler.capturedFrames(); }
  int64_t captureDroppedFrames() const { return mSpooler.captureDroppedFrames(); }
  // Backing tracks stream from disk (DiskReader thread) into the output mix;
  // they are never recorded into loop tracks.
  void openBackingTrack(int32_t slot, const std::string& path, bool loop);
  void playBackingTrack(int32_t slot);
  void pauseBackingTrack(int32_t slot);
  void closeBackingTrack(int32_t slot);
  void setBackingTrackGain(int32_t slot, float gain);
  StreamState backingTrackState(int32_t slot) const { return mSpooler.streamState(slot); }
  int64_t backingTrackPositionFrames(int32_t slot) const {
    return mSpooler.streamPositionFrames(slot);
  }
  int64_t backingTrackLengthFrames(int32_t slot) const {
    return mSpooler.streamLengthFrames(slot);
  }

  // ----- Parameters (control thread; plain atomic stores) -----
  void selectTrack(int32_t track);              // target for the next record/overdub
  void setTrackGain(int32_t track, float gain); // 0..4
  void setTrackMuted(int32_t track, bool muted);
  void setMonitorGain(float gain);              // live input passthrough level, 0..2
  // Round-trip compensation measured by the Phase-3 loopback calibration
  // tool: overdubs are written this many frames behind the playhead.
  void setRecordOffsetFrames(int32_t frames);

  // ----- Waveform / metering (single UI reader thread) -----
  // Drains up to maxPoints meter blocks; returns the count written.
  int32_t readWaveform(WaveformPoint* dest, int32_t maxPoints);

  // ----- Observability (any thread) -----
  EngineState state() const { return mState.load(std::memory_order_relaxed); }
  int32_t sampleRate() const { return mConfig.sampleRate; }
  int32_t channelCount() const { return mConfig.channelCount; }
  int32_t trackCount() const { return mConfig.trackCount; }
  int32_t maxLoopFrames() const { return mMaxLoopFrames; }
  int32_t loopLengthFrames() const { return mLoopLengthFrames.load(std::memory_order_relaxed); }
  int32_t playheadFrames() const { return mPlayheadFrames.load(std::memory_order_relaxed); }
  int32_t inputRingFillFrames() const { return mInputRing.framesReadable(); }
  TrackState trackState(int32_t track) const;

  // Drift/xrun forensics — a healthy same-clock setup keeps all of these ~0.
  int64_t driftFramesDropped() const { return mDriftDroppedFrames.load(std::memory_order_relaxed); }
  int64_t driftFramesInserted() const { return mDriftInsertedFrames.load(std::memory_order_relaxed); }
  int64_t inputUnderrunFrames() const { return mInputUnderrunFrames.load(std::memory_order_relaxed); }
  int64_t inputOverflowFrames() const { return mInputOverflowFrames.load(std::memory_order_relaxed); }
  int64_t meterDropCount() const { return mMeterDropCount.load(std::memory_order_relaxed); }

  // Control thread only (touches the streams).
  double outputLatencyMillis() const;
  int32_t framesPerBurst() const;

 private:
  // ----- Control -> audio commands (state transitions only; parameter
  // tweaks go through plain atomics instead) -----
  enum class CommandType : uint8_t {
    ToggleRecord,
    RecordStart,
    RecordStop,
    Play,
    Stop,
    ClearAll,
    ClearTrack,
  };
  struct Command {
    CommandType type;
    int32_t intArg;
  };

  struct LoopTrack {
    std::vector<float> data;  // maxLoopFrames * channels, pre-touched
    std::atomic<float> gain{1.0f};
    std::atomic<bool> muted{false};
    std::atomic<bool> hasContent{false};
    std::atomic<bool> clearing{false};
    int32_t clearCursor = 0;  // audio thread only
  };

  // One callback object per stream; routes into the engine by direction.
  // Oboe invokes onAudioReady on the realtime thread it owns for that stream,
  // and the error callbacks on a separate non-realtime thread.
  class StreamCallback : public oboe::AudioStreamDataCallback,
                         public oboe::AudioStreamErrorCallback {
   public:
    StreamCallback(AudioEngine& engine, oboe::Direction direction)
        : mEngine(engine), mDirection(direction) {}

    oboe::DataCallbackResult onAudioReady(oboe::AudioStream* stream, void* audioData,
                                          int32_t numFrames) override;
    void onErrorAfterClose(oboe::AudioStream* stream, oboe::Result error) override;

   private:
    AudioEngine& mEngine;
    const oboe::Direction mDirection;
  };

  // ----- Audio-thread entry points -----
  oboe::DataCallbackResult onInputReady(const float* audioData, int32_t numFrames);
  oboe::DataCallbackResult onOutputReady(float* audioData, int32_t numFrames);
  void onStreamErrorAfterClose(oboe::AudioStream* stream, oboe::Result error);

  // ----- Control-plane helpers (mLifecycleMutex held) -----
  oboe::Result startLocked();
  void stopLocked();
  oboe::Result openStreams();
  void closeStreams();
  void pushCommand(const Command& cmd);

  // ----- Audio-thread helpers (realtime-safe) -----
  void drainCommands();
  void applyCommand(const Command& cmd);
  void armRecording(EngineState current);
  void processPendingClears();
  void pullInput(float* dst, int32_t frames);
  void renderLooper(float* out, const float* in, int32_t frames);
  void finalizeMasterLoop();
  void beginTrackClear(LoopTrack& track);
  void publishMeters(const float* in, const float* out, int32_t frames);
  void setLoopLength(int32_t frames);
  void setPlayhead(int32_t frames);
  int32_t clampTrackIndex(int32_t track) const;
  static void computeMeter(const float* samples, int32_t count, float& rms, float& peak);

  // ----- Configuration (immutable after construction) -----
  Config mConfig;
  int32_t mMaxLoopFrames = 0;
  int32_t mLookAheadFrames = 0;
  int32_t mDriftSlackFrames = 0;

  // ----- Streams & callbacks (guarded by mLifecycleMutex) -----
  mutable std::mutex mLifecycleMutex;
  std::shared_ptr<oboe::AudioStream> mInputStream;
  std::shared_ptr<oboe::AudioStream> mOutputStream;
  std::shared_ptr<StreamCallback> mInputCallback;
  std::shared_ptr<StreamCallback> mOutputCallback;
  std::atomic<bool> mUserRunning{false};

  // ----- Lock-free plumbing -----
  SpscSampleRing mInputRing;
  std::vector<float> mInputScratch;  // kMaxCallbackFrames * channels, pre-touched
  SpscQueue<Command, 64> mCommands;
  std::mutex mCommandMutex;  // serializes control-side producers only; the
                             // audio-thread consumer never touches it
  SpscQueue<WaveformPoint, 512> mWaveform;

  // ----- Looper state: owned by the audio thread -----
  std::array<LoopTrack, kMaxTracks> mTracks;
  int32_t mLoopLen = 0;         // audio-thread master copy
  int32_t mPlayhead = 0;        // audio-thread master copy
  int32_t mMasterRecordPos = 0;
  int32_t mOverdubTrack = 0;
  bool mPrimed = false;         // look-ahead cushion established
  int32_t mDeclickRemaining = 0;

  // ----- Atomics shared with the control/UI threads -----
  std::atomic<EngineState> mState{EngineState::Idle};
  std::atomic<int32_t> mLoopLengthFrames{0};  // published mirror of mLoopLen
  std::atomic<int32_t> mPlayheadFrames{0};    // published mirror of mPlayhead
  std::atomic<int32_t> mSelectedTrack{0};
  // Default 0: hardware monitoring through the USB interface (e.g. the H2n)
  // is assumed. Raise via setMonitorGain() for software monitoring.
  std::atomic<float> mMonitorGain{0.0f};
  std::atomic<int32_t> mRecordOffset{0};

  // Output-path-only click generator (never reaches the record path).
  Metronome mMetronome;
  std::atomic<bool> mMetronomeSyncToLoop{true};

  // Disk spooling: capture tee + backing-track streaming (worker threads
  // owned by the spooler; the audio thread only touches its rings).
  DiskSpooler mSpooler;
  std::atomic<bool> mCaptureMixSource{false};  // false: input, true: mix

  // ----- Counters (relaxed; forensics only) -----
  std::atomic<int64_t> mDriftDroppedFrames{0};
  std::atomic<int64_t> mDriftInsertedFrames{0};
  std::atomic<int64_t> mInputUnderrunFrames{0};
  std::atomic<int64_t> mInputOverflowFrames{0};
  std::atomic<int64_t> mMeterDropCount{0};
  std::atomic<int64_t> mOversizeCallbackCount{0};
};

}  // namespace looper

#endif  // LOOPER_AUDIO_ENGINE_H
