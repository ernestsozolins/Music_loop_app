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
#include "LatencyCalibrator.h"
#include "LockFreeRing.h"
#include "Metronome.h"
#include "Reverb.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
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
  float pan = 0.0f;  // -1 (left) .. 0 (center, unity) .. +1 (right)
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

  // ----- Engine events -----
  // Fired from Oboe's non-realtime error thread (never from the audio
  // callbacks) when a device disconnects (e.g. USB interface unplugged) and
  // after the automatic restart attempt. The JNI layer forwards these to
  // Kotlin. Keep the callback quick; it must not destroy the engine.
  enum class EventType : int32_t {
    InputDisconnected = 0,
    OutputDisconnected = 1,
    RestartSucceeded = 2,
    RestartFailed = 3,
  };
  using EventCallback = std::function<void(int32_t type, int32_t arg)>;
  void setEventCallback(EventCallback callback);  // control thread

  // ----- Latency calibration (ping-and-listen; see LatencyCalibrator.h) -----
  // Requires: engine running, transport Idle/Stopped, capture off. Injects
  // 2 ms / 3 kHz bursts into the output and times their return on the input.
  // On success the measured round trip is applied as the overdub record
  // offset automatically. Asynchronous: poll calibrationState().
  void calibrateLatency();
  void cancelCalibration();
  int32_t calibrationState() const {  // LatencyCalibrator::State as int
    return static_cast<int32_t>(mCalibrator.state());
  }
  int32_t calibratedLatencyFrames() const { return mCalibrator.latencyFrames(); }

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

  // ----- Output monitoring DSP (control thread; lock-free atomics) -----
  // Freeverb-style reverb on the MONITORING mix only. Every recordable
  // signal is tapped upstream of it (input/mix capture tees, overdub path,
  // stem export), so takes and exported stems stay 100% dry.
  void setReverbRoomSize(float size) { mReverb.setRoomSize(size); }
  void setReverbDamping(float damping) { mReverb.setDamping(damping); }
  void setReverbMix(float mix) { mReverb.setMix(mix); }
  void setReverbEnabled(bool enabled) { mReverb.setEnabled(enabled); }
  float reverbRoomSize() const { return mReverb.roomSize(); }
  float reverbMix() const { return mReverb.mix(); }

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

  // ----- Session finalization & stem export (control thread) -----
  // Finalizes any active capture: the DiskWriter drains the remaining audio
  // out of the lock-free ring to disk, patches the RIFF/fact/data chunk
  // sizes, and closes the file handle. BLOCKS (bounded by timeoutMillis)
  // until the file is safe to read/zip — call it off the main thread.
  bool flushAndCloseSession(int32_t timeoutMillis = 5000);

  // Writes every loop track that has content to `directory/track_NN.wav`
  // (IEEE float32) on a dedicated worker thread — one stem per track, ready
  // for a desktop DAW. Requires the transport to be quiet (not recording or
  // overdubbing, no clear in progress); while the export runs, record-arm
  // and clear commands are frozen so the worker reads stable track data
  // (playback is still allowed — it only reads). Asynchronous: poll
  // exportState(). Returns false if an export is already running.
  static constexpr int32_t kExportIdle = 0;
  static constexpr int32_t kExportRunning = 1;
  static constexpr int32_t kExportDone = 2;
  static constexpr int32_t kExportFailed = 3;
  bool exportStems(const std::string& directory);
  int32_t exportState() const { return mExportState.load(std::memory_order_acquire); }
  int32_t exportedStemCount() const { return mExportedStems.load(std::memory_order_relaxed); }

  // ----- Session restore (control thread; asynchronous) -----
  // Loads saved stem .wav files back into the loop tracks — the inverse of
  // exportStems(), used by the persistence layer at startup. A worker
  // thread reads each file into its track buffer (safe: the tracks carry no
  // content yet and every transport command is frozen meanwhile); the loop
  // length + hasContent flags are then committed ON THE AUDIO THREAD via a
  // RestoreCommit command, so the looper state machine never races the
  // loader. The commit lands with the first audio callback, so completion
  // requires the engine to be started. Preconditions: no existing loop,
  // transport Idle/Stopped, no export/calibration running. Poll
  // restoreState().
  struct RestoreFile {
    int32_t track = 0;
    std::string path;
  };
  static constexpr int32_t kRestoreIdle = 0;
  static constexpr int32_t kRestoreRunning = 1;
  static constexpr int32_t kRestoreDone = 2;
  static constexpr int32_t kRestoreFailed = 3;
  bool restoreSession(std::vector<RestoreFile> files);
  int32_t restoreState() const { return mRestoreState.load(std::memory_order_acquire); }

  // ----- Parameters (control thread; plain atomic stores) -----
  void selectTrack(int32_t track);              // target for the next record/overdub
  void setTrackGain(int32_t track, float gain); // 0..4
  // Balance-law pan for stereo output: center is unity on both channels,
  // panning attenuates the opposite side only (no level jump for existing
  // material). Ignored when the engine runs mono.
  void setTrackPan(int32_t track, float pan);   // -1..1
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
  // Packed per-track flags for one-call UI polling: bit t = track t has
  // content, bit (t+16) = track t is still clearing.
  uint32_t trackContentMask() const;

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
    CalibrateStart,
    CalibrateCancel,
    RestoreCommit,  // audio thread adopts the loop the restore worker loaded
  };
  struct Command {
    CommandType type;
    int32_t intArg;
  };

  struct LoopTrack {
    std::vector<float> data;  // maxLoopFrames * channels, pre-touched
    std::atomic<float> gain{1.0f};
    std::atomic<float> pan{0.0f};
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
    bool onError(oboe::AudioStream* stream, oboe::Result error) override;
    void onErrorAfterClose(oboe::AudioStream* stream, oboe::Result error) override;

   private:
    AudioEngine& mEngine;
    const oboe::Direction mDirection;
  };

  // ----- Audio-thread entry points -----
  oboe::DataCallbackResult onInputReady(const float* audioData, int32_t numFrames);
  oboe::DataCallbackResult onOutputReady(float* audioData, int32_t numFrames);
  // Error-thread entry points (non-realtime thread owned by Oboe/AAudio)
  bool onStreamError(oboe::Direction direction, oboe::Result error);
  void onStreamErrorAfterClose(oboe::AudioStream* stream, oboe::Result error);
  void fireEvent(EventType type, int32_t arg);

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

  // Output-path-only monitoring reverb (never reaches the record path).
  Reverb mReverb;

  // Disk spooling: capture tee + backing-track streaming (worker threads
  // owned by the spooler; the audio thread only touches its rings).
  DiskSpooler mSpooler;
  std::atomic<bool> mCaptureMixSource{false};  // false: input, true: mix

  // Ping-and-listen round-trip measurement (audio-thread state machine).
  LatencyCalibrator mCalibrator;
  int64_t mAbsOutFrame = 0;  // monotonic output-frame counter (audio thread)

  // Stem export worker (reads loop-track buffers; audio thread freezes
  // track-mutating commands while mExportActive).
  void exportThreadMain();
  std::mutex mExportMutex;  // guards mExportThread spawn/join
  std::thread mExportThread;
  std::string mExportDir;  // set before spawn, read by the worker only
  std::atomic<bool> mExportActive{false};
  std::atomic<int32_t> mExportState{kExportIdle};
  std::atomic<int32_t> mExportedStems{0};

  // Session-restore worker (writes loop-track buffers; audio thread freezes
  // ALL transport commands while mRestoreActive, then adopts the result via
  // RestoreCommit). mRestoreMask/mRestoreLoopLen are written by the worker
  // before the commit command is pushed and read by the audio thread after
  // popping it — ordered by the command queue's release/acquire pair.
  void restoreThreadMain();
  std::mutex mRestoreMutex;  // guards mRestoreThread spawn/join
  std::thread mRestoreThread;
  std::vector<RestoreFile> mRestoreFiles;  // set before spawn, worker-only after
  std::atomic<bool> mRestoreActive{false};
  std::atomic<int32_t> mRestoreState{kRestoreIdle};
  uint32_t mRestoreMask = 0;
  int32_t mRestoreLoopLen = 0;

  // Event push to the JNI layer (error thread -> Kotlin).
  std::mutex mEventMutex;
  EventCallback mEventCallback;

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
