#include "AudioEngine.h"

#include <chrono>
#include <cmath>
#include <cstdio>

#include "Log.h"

namespace looper {

namespace {

int32_t millisToFrames(int32_t millis, int32_t sampleRate) {
  return static_cast<int32_t>(static_cast<int64_t>(millis) * sampleRate / 1000);
}

float clampf(float v, float lo, float hi) { return std::max(lo, std::min(hi, v)); }

int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return std::max(lo, std::min(hi, v)); }

}  // namespace

// ---------------------------------------------------------------------------
// StreamCallback — thin routing shims into the engine
// ---------------------------------------------------------------------------

oboe::DataCallbackResult AudioEngine::StreamCallback::onAudioReady(oboe::AudioStream* /*stream*/,
                                                                   void* audioData,
                                                                   int32_t numFrames) {
  if (mDirection == oboe::Direction::Input) {
    return mEngine.onInputReady(static_cast<const float*>(audioData), numFrames);
  }
  return mEngine.onOutputReady(static_cast<float*>(audioData), numFrames);
}

bool AudioEngine::StreamCallback::onError(oboe::AudioStream* /*stream*/, oboe::Result error) {
  return mEngine.onStreamError(mDirection, error);
}

void AudioEngine::StreamCallback::onErrorAfterClose(oboe::AudioStream* stream, oboe::Result error) {
  mEngine.onStreamErrorAfterClose(stream, error);
}

// ---------------------------------------------------------------------------
// Construction / configuration
// ---------------------------------------------------------------------------

AudioEngine::AudioEngine() : AudioEngine(Config()) {}

AudioEngine::AudioEngine(const Config& config) : mConfig(config) {
  mConfig.sampleRate = clampi(mConfig.sampleRate, 8000, 192000);
  mConfig.channelCount = clampi(mConfig.channelCount, 1, 2);
  mConfig.trackCount = clampi(mConfig.trackCount, 1, kMaxTracks);
  // Memory: trackCount * maxLoopSeconds * sampleRate * channels * 4 bytes.
  // Defaults (4 tracks, 30 s, 48 kHz stereo) ~= 46 MB.
  mConfig.maxLoopSeconds = clampi(mConfig.maxLoopSeconds, 1, 600);
  mConfig.lookAheadMillis = clampi(mConfig.lookAheadMillis, 2, 500);
  mConfig.driftSlackMillis = clampi(mConfig.driftSlackMillis, 1, mConfig.lookAheadMillis);

  mMaxLoopFrames = mConfig.sampleRate * mConfig.maxLoopSeconds;
  mLookAheadFrames = millisToFrames(mConfig.lookAheadMillis, mConfig.sampleRate);
  mDriftSlackFrames = millisToFrames(mConfig.driftSlackMillis, mConfig.sampleRate);

  // Everything the audio threads touch is allocated and pre-touched here,
  // on the control thread, before any stream exists.
  mInputRing.allocate(mLookAheadFrames + mDriftSlackFrames + 4 * kMaxCallbackFrames,
                      mConfig.channelCount);
  mInputScratch.assign(static_cast<size_t>(kMaxCallbackFrames) * mConfig.channelCount, 0.0f);
  // Sized for the widest input the callback may hand us before remapping to
  // the engine's channel count (a 4-channel interface into a stereo engine).
  mInputRemapScratch.assign(static_cast<size_t>(kMaxCallbackFrames) * 8, 0.0f);
  for (int32_t t = 0; t < mConfig.trackCount; ++t) {
    mTracks[t].data.assign(static_cast<size_t>(mMaxLoopFrames) * mConfig.channelCount, 0.0f);
  }
  // One spare track's worth of RAM buys single-level per-pass undo.
  mUndoBuffer.assign(static_cast<size_t>(mMaxLoopFrames) * mConfig.channelCount, 0.0f);

  mMetronome.configure(mConfig.sampleRate, mConfig.channelCount);
  mCalibrator.configure(mConfig.sampleRate, mConfig.channelCount);
  mReverb.configure(mConfig.sampleRate, mConfig.channelCount);
  mLimiter.configure(mConfig.sampleRate, mConfig.channelCount);
  mInputMapR.store(mConfig.channelCount > 1 ? 1 : 0, std::memory_order_relaxed);
  mOutputDeviceId.store(mConfig.outputDeviceId, std::memory_order_relaxed);
  mInputDeviceId.store(mConfig.inputDeviceId, std::memory_order_relaxed);

  // The spooler's worker threads live for the engine's whole lifetime; the
  // audio callbacks only ever touch its lock-free rings, so starting the
  // threads here (before any stream exists) is race-free.
  mSpooler.configure(mConfig.sampleRate, mConfig.channelCount, kMaxCallbackFrames);
  mSpooler.start();

  mInputCallback = std::make_shared<StreamCallback>(*this, oboe::Direction::Input);
  mOutputCallback = std::make_shared<StreamCallback>(*this, oboe::Direction::Output);
}

AudioEngine::~AudioEngine() {
  stop();  // streams closed first: no audio callback can run past here
  {
    std::lock_guard<std::mutex> lock(mExportMutex);
    if (mExportThread.joinable()) mExportThread.join();
  }
  {
    std::lock_guard<std::mutex> lock(mRestoreMutex);
    if (mRestoreThread.joinable()) mRestoreThread.join();
  }
  mSpooler.stop();  // then finalize any open files and join the worker threads
}

// ---------------------------------------------------------------------------
// Lifecycle (control thread)
// ---------------------------------------------------------------------------

oboe::Result AudioEngine::start() {
  std::lock_guard<std::mutex> lock(mLifecycleMutex);
  return startLocked();
}

void AudioEngine::stop() {
  std::lock_guard<std::mutex> lock(mLifecycleMutex);
  stopLocked();
}

oboe::Result AudioEngine::startLocked() {
  if (mUserRunning.load(std::memory_order_acquire)) return oboe::Result::OK;

  oboe::Result result = openStreams();
  if (result != oboe::Result::OK) {
    closeStreams();
    return result;
  }

  // Both callbacks are guaranteed stopped here, so resetting the shared
  // plumbing is race-free. Loop content deliberately survives restarts.
  mInputRing.reset();
  mPrimed = false;
  mDeclickRemaining = 0;

  // Input first so the ring starts filling; the output side holds silence
  // until the look-ahead cushion is primed, so start order is not critical.
  result = mInputStream->requestStart();
  if (result != oboe::Result::OK) {
    closeStreams();
    return result;
  }
  result = mOutputStream->requestStart();
  if (result != oboe::Result::OK) {
    mInputStream->requestStop();
    closeStreams();
    return result;
  }

  mUserRunning.store(true, std::memory_order_release);
  LOGI("engine started: rate=%d, inBurst=%d, outBurst=%d", mConfig.sampleRate,
       mInputStream->getFramesPerBurst(), mOutputStream->getFramesPerBurst());
  return oboe::Result::OK;
}

void AudioEngine::stopLocked() {
  mUserRunning.store(false, std::memory_order_release);
  if (mOutputStream) mOutputStream->stop();
  if (mInputStream) mInputStream->stop();
  closeStreams();
}

oboe::Result AudioEngine::setOutputDevice(int32_t deviceId) {
  std::lock_guard<std::mutex> lock(mLifecycleMutex);
  mConfig.outputDeviceId = deviceId;
  mOutputDeviceId.store(deviceId, std::memory_order_relaxed);
  return applyDeviceChangeLocked();
}

oboe::Result AudioEngine::setInputDevice(int32_t deviceId) {
  std::lock_guard<std::mutex> lock(mLifecycleMutex);
  mConfig.inputDeviceId = deviceId;
  mInputDeviceId.store(deviceId, std::memory_order_relaxed);
  return applyDeviceChangeLocked();
}

oboe::Result AudioEngine::applyDeviceChangeLocked() {
  // Not running yet: the new device id is picked up by the next start().
  if (!mUserRunning.load(std::memory_order_acquire)) return oboe::Result::OK;
  // Running: reopen both streams on the new device. Loop content, transport
  // state, and any in-progress recording survive (only the streams + input
  // ring reset). Same machinery as the disconnect-recovery restart.
  stopLocked();
  const oboe::Result result = startLocked();
  if (result != oboe::Result::OK) {
    LOGW("device change reopen failed: %s", oboe::convertToText(result));
  }
  return result;
}

oboe::Result AudioEngine::openStreams() {
  // Two independent streams, deliberately NOT a forced full-duplex pair.
  //
  // SAMPLE-RATE UNIFICATION: both builders explicitly request the same
  // engine rate (setSampleRate below) AND enable Oboe's internal resampler
  // (setSampleRateConversionQuality(Medium)). If the external USB hardware
  // only supports a different native rate (e.g. a 44.1 kHz-only interface),
  // Oboe resamples to the requested rate instead of delivering mismatched
  // frames — without this, audio crossing the input->output ring would be
  // pitch-shifted by the rate ratio. Residual clock drift between the two
  // hardware clocks is then handled by pullInput().
  oboe::AudioStreamBuilder outBuilder;
  outBuilder.setDirection(oboe::Direction::Output)
      ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
      ->setSharingMode(oboe::SharingMode::Shared)
      ->setFormat(oboe::AudioFormat::Float)
      ->setFormatConversionAllowed(true)
      ->setChannelCount(mConfig.channelCount)
      ->setChannelConversionAllowed(true)
      ->setSampleRate(mConfig.sampleRate)
      ->setSampleRateConversionQuality(oboe::SampleRateConversionQuality::Medium)
      ->setUsage(oboe::Usage::Media)
      ->setContentType(oboe::ContentType::Music)
      ->setDeviceId(mConfig.outputDeviceId)
      ->setDataCallback(mOutputCallback)
      ->setErrorCallback(mOutputCallback);

  oboe::Result result = outBuilder.openStream(mOutputStream);
  if (result != oboe::Result::OK) {
    LOGW("failed to open output stream: %s", oboe::convertToText(result));
    return result;
  }
  // Double-buffer the output for the standard latency/stability trade-off.
  mOutputStream->setBufferSizeInFrames(mOutputStream->getFramesPerBurst() * 2);

  // Multichannel request: when the user picks a wider capture (e.g. a
  // 4-channel H2n mode) we open that many channels and remap to the engine's
  // stereo path ourselves in onInputReady, so channel conversion must be OFF.
  const int32_t requestedInCh =
      mInputRequestChannels > 0 ? mInputRequestChannels : mConfig.channelCount;
  const bool customChannelMap = requestedInCh != mConfig.channelCount;

  oboe::AudioStreamBuilder inBuilder;
  inBuilder.setDirection(oboe::Direction::Input)
      ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
      ->setSharingMode(oboe::SharingMode::Shared)
      ->setFormat(oboe::AudioFormat::Float)
      ->setFormatConversionAllowed(true)
      ->setChannelCount(requestedInCh)
      ->setChannelConversionAllowed(!customChannelMap)
      ->setSampleRate(mConfig.sampleRate)
      ->setSampleRateConversionQuality(oboe::SampleRateConversionQuality::Medium)
      // No AGC / noise suppression / echo cancellation in the capture path —
      // essential for a music source like the Zoom H2n.
      ->setInputPreset(oboe::InputPreset::Unprocessed)
      ->setDeviceId(mConfig.inputDeviceId)
      ->setDataCallback(mInputCallback)
      ->setErrorCallback(mInputCallback);

  result = inBuilder.openStream(mInputStream);
  if (result != oboe::Result::OK && customChannelMap) {
    // The device could not supply the requested channel count: fall back to
    // the engine channel count with conversion on.
    LOGW("multichannel input (%d ch) unavailable, falling back to %d", requestedInCh,
         mConfig.channelCount);
    inBuilder.setChannelCount(mConfig.channelCount)->setChannelConversionAllowed(true);
    result = inBuilder.openStream(mInputStream);
  }
  if (result != oboe::Result::OK) {
    LOGW("failed to open input stream: %s", oboe::convertToText(result));
    return result;
  }
  // What the input callback will actually receive (used by the remap).
  mInputOpenedChannels = mInputStream->getChannelCount();

  // With SRC pinned above, both streams must present the engine rate; if a
  // platform ever refuses, fail loudly rather than run misaligned frame math.
  if (mInputStream->getSampleRate() != mConfig.sampleRate ||
      mOutputStream->getSampleRate() != mConfig.sampleRate) {
    LOGW("stream rate mismatch: in=%d out=%d want=%d", mInputStream->getSampleRate(),
         mOutputStream->getSampleRate(), mConfig.sampleRate);
    return oboe::Result::ErrorInvalidRate;
  }
  return oboe::Result::OK;
}

void AudioEngine::closeStreams() {
  if (mOutputStream) {
    mOutputStream->close();
    mOutputStream.reset();
  }
  if (mInputStream) {
    mInputStream->close();
    mInputStream.reset();
  }
}

bool AudioEngine::onStreamError(oboe::Direction direction, oboe::Result error) {
  // First line of disconnect protection. Runs on Oboe's non-realtime error
  // thread BEFORE the stream is closed: the moment a USB mic or Bluetooth
  // sink vanishes we park the transport (Stop command), abort any calibration
  // in flight, stop disk writing (which finalizes the .wav header so the take
  // on disk stays valid), and push a thread-safe event up to Kotlin. Returning
  // false lets Oboe run its normal close sequence; the recovery restart then
  // happens in onStreamErrorAfterClose().
  if (error == oboe::Result::ErrorDisconnected) {
    LOGW("%s stream disconnected", direction == oboe::Direction::Input ? "input" : "output");
    pushCommand({CommandType::CalibrateCancel, 0});
    pushCommand({CommandType::Stop, 0});
    mSpooler.stopCapture();
    fireEvent(direction == oboe::Direction::Input ? EventType::InputDisconnected
                                                  : EventType::OutputDisconnected,
              static_cast<int32_t>(error));
  }
  return false;
}

void AudioEngine::onStreamErrorAfterClose(oboe::AudioStream* stream, oboe::Result error) {
  // Runs on a non-realtime thread owned by Oboe/AAudio, so locking and
  // reopening streams here is legal (this is the pattern Oboe's own duplex
  // samples use). The immediate protective actions already ran in
  // onStreamError(); this stage attempts the recovery restart.
  if (error != oboe::Result::ErrorDisconnected) return;

  bool attempted = false;
  oboe::Result result = oboe::Result::OK;
  {
    std::lock_guard<std::mutex> lock(mLifecycleMutex);
    if (!mUserRunning.load(std::memory_order_acquire)) return;
    // Ignore stale events from a stream generation we already replaced.
    if (stream != mInputStream.get() && stream != mOutputStream.get()) return;

    LOGI("restarting duplex pair after disconnect");
    stopLocked();
    result = startLocked();
    attempted = true;
    if (result != oboe::Result::OK) {
      LOGW("restart after disconnect failed: %s", oboe::convertToText(result));
      // The explicitly-selected device (e.g. the Bluetooth sink the user
      // picked) is likely the one that vanished — fall back to the system
      // default so audio isn't lost.
      if (mConfig.outputDeviceId != 0 || mConfig.inputDeviceId != 0) {
        mConfig.outputDeviceId = 0;
        mConfig.inputDeviceId = 0;
        mOutputDeviceId.store(0, std::memory_order_relaxed);
        mInputDeviceId.store(0, std::memory_order_relaxed);
        result = startLocked();
        LOGI("fell back to system default devices: %s", oboe::convertToText(result));
      }
    }
  }
  // Fire outside the lifecycle lock so a listener reacting to the event can
  // safely call back into the engine.
  if (attempted) {
    fireEvent(result == oboe::Result::OK ? EventType::RestartSucceeded : EventType::RestartFailed,
              static_cast<int32_t>(result));
  }
}

void AudioEngine::setEventCallback(EventCallback callback) {
  std::lock_guard<std::mutex> lock(mEventMutex);
  mEventCallback = std::move(callback);
}

void AudioEngine::fireEvent(EventType type, int32_t arg) {
  EventCallback callback;
  {
    std::lock_guard<std::mutex> lock(mEventMutex);
    callback = mEventCallback;
  }
  if (callback) callback(static_cast<int32_t>(type), arg);
}

// ---------------------------------------------------------------------------
// Control surface (control thread)
// ---------------------------------------------------------------------------

void AudioEngine::pushCommand(const Command& cmd) {
  // The mutex only serializes control-side producers (keeps the queue SPSC);
  // the audio-thread consumer never takes it.
  std::lock_guard<std::mutex> lock(mCommandMutex);
  if (!mCommands.push(cmd)) {
    LOGW("command queue full; dropped command %d", static_cast<int>(cmd.type));
  }
}

void AudioEngine::toggleRecord() { pushCommand({CommandType::ToggleRecord, 0}); }
void AudioEngine::startRecording() { pushCommand({CommandType::RecordStart, 0}); }
void AudioEngine::stopRecording() { pushCommand({CommandType::RecordStop, 0}); }
void AudioEngine::play() { pushCommand({CommandType::PlayAll, 0}); }
void AudioEngine::stopPlayback() { pushCommand({CommandType::StopAll, 0}); }
void AudioEngine::clearAll() { pushCommand({CommandType::ClearAll, 0}); }
void AudioEngine::resetLoopForRestore() { pushCommand({CommandType::ResetForRestore, 0}); }

// ----- Per-track transport (control thread -> audio thread) -----
void AudioEngine::recordTrack(int32_t track) {
  pushCommand({CommandType::RecordTrack, clampTrackIndex(track)});
}
void AudioEngine::playTrack(int32_t track) {
  pushCommand({CommandType::PlayTrack, clampTrackIndex(track)});
}
void AudioEngine::stopTrack(int32_t track) {
  pushCommand({CommandType::StopTrack, clampTrackIndex(track)});
}
void AudioEngine::playAll() { pushCommand({CommandType::PlayAll, 0}); }
void AudioEngine::stopAll() { pushCommand({CommandType::StopAll, 0}); }

TrackTransport AudioEngine::trackTransport(int32_t track) const {
  return static_cast<TrackTransport>(
      mTracks[clampTrackIndex(track)].transport.load(std::memory_order_relaxed));
}
int32_t AudioEngine::trackPositionFrames(int32_t track) const {
  return mTracks[clampTrackIndex(track)].posFrames.load(std::memory_order_relaxed);
}

void AudioEngine::setMetronomeState(bool active, float bpm, int32_t beatsPerMeasure) {
  mMetronome.setState(active, bpm, beatsPerMeasure);
}

void AudioEngine::setMetronomeGain(float gain) { mMetronome.setGain(gain); }

void AudioEngine::setMetronomeSyncToLoop(bool enabled) {
  mMetronomeSyncToLoop.store(enabled, std::memory_order_relaxed);
}

void AudioEngine::calibrateLatency() {
  if (!isRunning()) {
    // No streams -> the command queue is not draining; fail immediately
    // rather than leaving a stale command to fire on some future start().
    mCalibrator.markFailed();
    return;
  }
  pushCommand({CommandType::CalibrateStart, 0});
}

void AudioEngine::cancelCalibration() { pushCommand({CommandType::CalibrateCancel, 0}); }

void AudioEngine::startCapture(const std::string& path, bool captureMix) {
  mCaptureMixSource.store(captureMix, std::memory_order_relaxed);
  mSpooler.startCapture(path);
}

void AudioEngine::stopCapture() { mSpooler.stopCapture(); }

void AudioEngine::openBackingTrack(int32_t slot, const std::string& path, bool loop) {
  mSpooler.openStream(slot, path, loop);
}

void AudioEngine::playBackingTrack(int32_t slot) { mSpooler.playStream(slot); }
void AudioEngine::pauseBackingTrack(int32_t slot) { mSpooler.pauseStream(slot); }
void AudioEngine::closeBackingTrack(int32_t slot) { mSpooler.closeStream(slot); }

void AudioEngine::setBackingTrackGain(int32_t slot, float gain) {
  mSpooler.setStreamGain(slot, gain);
}

void AudioEngine::setBackingTrackLoop(int32_t slot, bool loop) {
  mSpooler.setStreamLoop(slot, loop);
}

// ---------------------------------------------------------------------------
// Session finalization & stem export (control / worker threads)
// ---------------------------------------------------------------------------

bool AudioEngine::flushAndCloseSession(int32_t timeoutMillis) {
  // Ask the DiskWriter to drain the capture ring to disk, patch the RIFF and
  // data chunk sizes, and close the handle — then wait until it has.
  mSpooler.stopCapture();
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMillis);
  while (mSpooler.isCapturing()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      LOGW("flushAndCloseSession timed out after %d ms", timeoutMillis);
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return true;
}

bool AudioEngine::exportStems(const std::string& directory) {
  std::lock_guard<std::mutex> lock(mExportMutex);
  if (mExportState.load(std::memory_order_acquire) == kExportRunning) return false;
  if (mRestoreState.load(std::memory_order_acquire) == kRestoreRunning) return false;
  if (mExportThread.joinable()) mExportThread.join();  // reap the previous run
  mExportDir = directory;
  mExportedStems.store(0, std::memory_order_relaxed);
  // Freeze track-mutating commands FIRST, then verify preconditions on the
  // worker after a grace window, so a record-arm racing this call either
  // lands before the check (export fails) or is refused by the freeze.
  mExportActive.store(true, std::memory_order_release);
  mExportState.store(kExportRunning, std::memory_order_release);
  mExportThread = std::thread([this] { exportThreadMain(); });
  return true;
}

void AudioEngine::exportThreadMain() {
  // Longer than one audio callback: any command already past the freeze is
  // applied by now, so the state we read below is settled.
  std::this_thread::sleep_for(std::chrono::milliseconds(25));

  const EngineState st = mState.load(std::memory_order_acquire);
  bool clearing = false;
  for (int32_t t = 0; t < mConfig.trackCount; ++t) {
    clearing = clearing || mTracks[t].clearing.load(std::memory_order_acquire);
  }
  if (st == EngineState::RecordingMaster || st == EngineState::Overdubbing || clearing) {
    LOGW("stem export refused: tracks are being written");
    mExportActive.store(false, std::memory_order_release);
    mExportState.store(kExportFailed, std::memory_order_release);
    return;
  }

  const int32_t loopLen = mLoopLengthFrames.load(std::memory_order_acquire);
  bool ok = true;
  int32_t written = 0;
  if (loopLen > 0) {
    for (int32_t t = 0; t < mConfig.trackCount; ++t) {
      if (!mTracks[t].hasContent.load(std::memory_order_acquire)) continue;
      char name[32];
      std::snprintf(name, sizeof(name), "/track_%02d.wav", t + 1);
      const int64_t n = mSpooler.writeWavFile(mExportDir + name, mTracks[t].data.data(), loopLen);
      if (n == loopLen) {
        mExportedStems.store(++written, std::memory_order_relaxed);
      } else {
        LOGW("stem export: track %d wrote %lld of %d frames", t + 1,
             static_cast<long long>(n), loopLen);
        ok = false;
      }
    }
  }
  LOGI("stem export finished: %d stem(s), loopLen=%d", written, loopLen);
  mExportActive.store(false, std::memory_order_release);
  mExportState.store(ok ? kExportDone : kExportFailed, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Per-pass undo (control thread)
// ---------------------------------------------------------------------------

void AudioEngine::snapshotTrackForUndo(int32_t track) {
  std::lock_guard<std::mutex> lock(mUndoMutex);
  if (track < 0 || track >= mConfig.trackCount) return;
  const int32_t loopLen = mLoopLengthFrames.load(std::memory_order_acquire);
  const bool hadContent = mTracks[track].hasContent.load(std::memory_order_acquire) && loopLen > 0;
  if (hadContent) {
    // Concurrent with audio-thread READS at most (the pass has not started);
    // reader/reader is race-free.
    std::memcpy(mUndoBuffer.data(), mTracks[track].data.data(),
                static_cast<size_t>(loopLen) * mConfig.channelCount * sizeof(float));
  }
  mUndoHadContent = hadContent;
  mUndoLoopLen = loopLen;
  mUndoTrack.store(track, std::memory_order_release);
}

bool AudioEngine::undoLastPass() {
  std::lock_guard<std::mutex> lock(mUndoMutex);
  const int32_t track = mUndoTrack.load(std::memory_order_acquire);
  if (track < 0) return false;
  if (mExportState.load(std::memory_order_acquire) == kExportRunning) return false;
  mUndoTrack.store(-1, std::memory_order_release);  // one-shot

  // 1. Freeze arming/clearing, silence the track (mixer stops reading it).
  mUndoActive.store(true, std::memory_order_release);
  pushCommand({CommandType::UndoMute, track});
  if (isRunning()) std::this_thread::sleep_for(std::chrono::milliseconds(30));

  if (mUndoHadContent && mUndoLoopLen == mLoopLengthFrames.load(std::memory_order_acquire)) {
    // 2. Rewrite the buffer off the audio thread, then recommit.
    std::memcpy(mTracks[track].data.data(), mUndoBuffer.data(),
                static_cast<size_t>(mUndoLoopLen) * mConfig.channelCount * sizeof(float));
    mUndoActive.store(false, std::memory_order_release);
    pushCommand({CommandType::UndoCommit, track});
  } else {
    // The pass started from an empty track (or the loop changed):
    // undo = wipe the take.
    mUndoActive.store(false, std::memory_order_release);
    pushCommand({CommandType::ClearTrack, track});
  }
  LOGI("undo: restored pre-pass state of track %d", track);
  return true;
}

bool AudioEngine::trimTrackStart(int32_t track, int32_t frames) {
  std::lock_guard<std::mutex> lock(mUndoMutex);
  if (track < 0 || track >= mConfig.trackCount) return false;
  const int32_t loopLen = mLoopLengthFrames.load(std::memory_order_acquire);
  if (loopLen <= 0 || !mTracks[track].hasContent.load(std::memory_order_acquire)) return false;
  if (mExportState.load(std::memory_order_acquire) == kExportRunning) return false;
  const int32_t n = std::max(0, std::min(frames, loopLen));
  if (n == 0) return false;
  const int32_t ch = mConfig.channelCount;

  // Snapshot the whole track first so undo/Backspace restores the original.
  std::memcpy(mUndoBuffer.data(), mTracks[track].data.data(),
              static_cast<size_t>(loopLen) * ch * sizeof(float));
  mUndoHadContent = true;
  mUndoLoopLen = loopLen;
  mUndoTrack.store(track, std::memory_order_release);

  // Freeze the mixer's read of this track, zero the first n frames off the
  // audio thread, then recommit — same handshake as undo.
  mUndoActive.store(true, std::memory_order_release);
  pushCommand({CommandType::UndoMute, track});
  if (isRunning()) std::this_thread::sleep_for(std::chrono::milliseconds(30));
  std::memset(mTracks[track].data.data(), 0, static_cast<size_t>(n) * ch * sizeof(float));
  mUndoActive.store(false, std::memory_order_release);
  pushCommand({CommandType::UndoCommit, track});
  LOGI("trim: silenced first %d frames of track %d", n, track);
  return true;
}

int32_t AudioEngine::trackWaveform(int32_t track, float* bins, int32_t maxBins) const {
  if (track < 0 || track >= mConfig.trackCount || bins == nullptr || maxBins <= 0) return 0;
  const int32_t loopLen = mLoopLengthFrames.load(std::memory_order_acquire);
  if (loopLen <= 0 || !mTracks[track].hasContent.load(std::memory_order_acquire)) return 0;
  const int32_t ch = mConfig.channelCount;
  const float* data = mTracks[track].data.data();
  const int32_t binCount = std::min(maxBins, loopLen);
  for (int32_t b = 0; b < binCount; ++b) {
    const int64_t from = static_cast<int64_t>(loopLen) * b / binCount;
    const int64_t to = static_cast<int64_t>(loopLen) * (b + 1) / binCount;
    float peak = 0.0f;
    for (int64_t f = from; f < to; ++f) {
      for (int32_t c = 0; c < ch; ++c) {
        const float v = data[f * ch + c];
        const float a = v < 0.0f ? -v : v;
        if (a > peak) peak = a;
      }
    }
    bins[b] = peak;
  }
  return binCount;
}

bool AudioEngine::restoreSession(std::vector<RestoreFile> files) {
  std::lock_guard<std::mutex> lock(mRestoreMutex);
  if (files.empty()) return false;
  if (mRestoreState.load(std::memory_order_acquire) == kRestoreRunning) return false;
  if (mExportState.load(std::memory_order_acquire) == kExportRunning) return false;
  if (mRestoreThread.joinable()) mRestoreThread.join();  // reap the previous run
  mRestoreFiles = std::move(files);
  // Freeze the whole transport before the worker touches any track buffer;
  // preconditions are re-checked on the worker after a grace window.
  mRestoreActive.store(true, std::memory_order_release);
  mRestoreState.store(kRestoreRunning, std::memory_order_release);
  mRestoreThread = std::thread([this] { restoreThreadMain(); });
  return true;
}

void AudioEngine::restoreThreadMain() {
  // Longer than one audio callback: any command already past the freeze has
  // been applied, so the state we read below is settled.
  std::this_thread::sleep_for(std::chrono::milliseconds(25));

  const EngineState st = mState.load(std::memory_order_acquire);
  bool blocked = (st != EngineState::Idle && st != EngineState::Stopped) ||
                 mLoopLengthFrames.load(std::memory_order_acquire) != 0 ||  // never clobber a live loop
                 mExportActive.load(std::memory_order_acquire) || mCalibrator.active();
  for (const RestoreFile& f : mRestoreFiles) {
    if (f.track >= 0 && f.track < mConfig.trackCount) {
      blocked = blocked || mTracks[f.track].clearing.load(std::memory_order_acquire) ||
                mTracks[f.track].hasContent.load(std::memory_order_acquire);
    }
  }
  if (blocked) {
    LOGW("session restore refused: engine is not in a fresh, quiet state");
    mRestoreActive.store(false, std::memory_order_release);
    mRestoreState.store(kRestoreFailed, std::memory_order_release);
    return;
  }

  // Load each stem into its (content-free, unreadable-by-the-mixer) buffer.
  int32_t loopLen = 0;
  uint32_t mask = 0;
  for (const RestoreFile& f : mRestoreFiles) {
    if (f.track < 0 || f.track >= mConfig.trackCount) continue;
    const int64_t frames =
        mSpooler.readWavFile(f.path, mTracks[f.track].data.data(), mMaxLoopFrames);
    if (frames <= 0) {
      LOGW("restore: skipping track %d (%s)", f.track, f.path.c_str());
      continue;
    }
    // Stems of one session share a length; tolerate small mismatches by
    // taking the shortest so every playing track has valid data.
    loopLen = (loopLen == 0) ? static_cast<int32_t>(frames)
                             : std::min(loopLen, static_cast<int32_t>(frames));
    mask |= 1u << f.track;
  }

  if (mask == 0 || loopLen <= 0) {
    mRestoreActive.store(false, std::memory_order_release);
    mRestoreState.store(kRestoreFailed, std::memory_order_release);
    return;
  }

  // Hand the result to the audio thread: it adopts loop length + content
  // flags atomically with respect to the state machine. The freeze lifts
  // there, and Done is reported there.
  mRestoreMask = mask;
  mRestoreLoopLen = loopLen;
  pushCommand({CommandType::RestoreCommit, 0});
  LOGI("restore: %d frames staged across mask 0x%x, awaiting audio-thread commit",
       loopLen, mask);
}

void AudioEngine::clearTrack(int32_t track) {
  if (track < 0 || track >= mConfig.trackCount) return;
  pushCommand({CommandType::ClearTrack, track});
}

void AudioEngine::selectTrack(int32_t track) {
  if (track < 0 || track >= mConfig.trackCount) return;
  mSelectedTrack.store(track, std::memory_order_relaxed);
}

void AudioEngine::setInputChannels(int32_t channels, int32_t mapLeft, int32_t mapRight) {
  // The map indices are lock-free and apply live; the channel COUNT needs a
  // stream reopen, so latch it under the lifecycle lock for the next start().
  mInputMapL.store(std::max(0, mapLeft), std::memory_order_relaxed);
  mInputMapR.store(std::max(0, mapRight), std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(mLifecycleMutex);
    mInputRequestChannels = std::max(0, std::min(channels, 8));
  }
}

void AudioEngine::setTrackGain(int32_t track, float gain) {
  if (track < 0 || track >= mConfig.trackCount) return;
  mTracks[track].gain.store(clampf(gain, 0.0f, 4.0f), std::memory_order_relaxed);
}

void AudioEngine::setTrackPan(int32_t track, float pan) {
  if (track < 0 || track >= mConfig.trackCount) return;
  mTracks[track].pan.store(clampf(pan, -1.0f, 1.0f), std::memory_order_relaxed);
}

void AudioEngine::setTrackShiftFrames(int32_t track, int32_t frames) {
  if (track < 0 || track >= mConfig.trackCount) return;
  mTracks[track].playOffset.store(frames, std::memory_order_relaxed);
}

int32_t AudioEngine::trackShiftFrames(int32_t track) const {
  if (track < 0 || track >= mConfig.trackCount) return 0;
  return mTracks[track].playOffset.load(std::memory_order_relaxed);
}

void AudioEngine::setTrackMuted(int32_t track, bool muted) {
  if (track < 0 || track >= mConfig.trackCount) return;
  mTracks[track].muted.store(muted, std::memory_order_relaxed);
}

void AudioEngine::setTrackTrim(int32_t track, int32_t startFrames, int32_t endFrames) {
  if (track < 0 || track >= mConfig.trackCount) return;
  const int32_t len = mLoopLengthFrames.load(std::memory_order_relaxed);
  int32_t s = startFrames < 0 ? 0 : startFrames;
  int32_t e = endFrames;
  if (len > 0) {
    s = std::min(s, len);
    if (e > len) e = len;
  }
  if (e > 0 && e <= s) e = s + 1;  // keep a non-empty window
  mTracks[track].trimStart.store(s, std::memory_order_relaxed);
  mTracks[track].trimEnd.store(e <= 0 ? 0 : e, std::memory_order_relaxed);
}

int32_t AudioEngine::trackTrimStart(int32_t track) const {
  if (track < 0 || track >= mConfig.trackCount) return 0;
  return mTracks[track].trimStart.load(std::memory_order_relaxed);
}

int32_t AudioEngine::trackTrimEnd(int32_t track) const {
  if (track < 0 || track >= mConfig.trackCount) return 0;
  const int32_t e = mTracks[track].trimEnd.load(std::memory_order_relaxed);
  const int32_t len = mLoopLengthFrames.load(std::memory_order_relaxed);
  return (e <= 0 || e > len) ? len : e;  // resolve the sentinel to loop length
}

void AudioEngine::setTrackOneShot(int32_t track, bool oneShot) {
  if (track < 0 || track >= mConfig.trackCount) return;
  mTracks[track].oneShot.store(oneShot, std::memory_order_relaxed);
}

bool AudioEngine::trackOneShot(int32_t track) const {
  if (track < 0 || track >= mConfig.trackCount) return false;
  return mTracks[track].oneShot.load(std::memory_order_relaxed);
}

void AudioEngine::autoTrimTrack(int32_t track, float thresholdDb) {
  if (track < 0 || track >= mConfig.trackCount) return;
  const int32_t len = mLoopLengthFrames.load(std::memory_order_relaxed);
  const LoopTrack& t = mTracks[track];
  if (len <= 0 || !t.hasContent.load(std::memory_order_relaxed)) return;
  const int32_t ch = mConfig.channelCount;
  const float threshold = std::pow(10.0f, thresholdDb / 20.0f);  // linear peak
  const float* data = t.data.data();  // read-only scan; concurrent playback is fine

  int32_t first = -1;
  int32_t last = -1;
  for (int32_t f = 0; f < len; ++f) {
    float peak = 0.0f;
    const float* s = data + static_cast<size_t>(f) * ch;
    for (int32_t c = 0; c < ch; ++c) {
      const float a = s[c] < 0.0f ? -s[c] : s[c];
      if (a > peak) peak = a;
    }
    if (peak > threshold) {
      if (first < 0) first = f;
      last = f;
    }
  }
  if (first < 0) return;  // silent track: leave the window untouched

  // Keep a short margin so transients aren't clipped (~10 ms).
  const int32_t margin = mConfig.sampleRate / 100;
  int32_t s = std::max(0, first - margin);
  int32_t e = std::min(len, last + 1 + margin);
  mTracks[track].trimStart.store(s, std::memory_order_relaxed);
  mTracks[track].trimEnd.store(e >= len ? 0 : e, std::memory_order_relaxed);
}

void AudioEngine::setMonitorGain(float gain) {
  mMonitorGain.store(clampf(gain, 0.0f, 2.0f), std::memory_order_relaxed);
}

void AudioEngine::setRecordOffsetFrames(int32_t frames) {
  mRecordOffset.store(clampi(frames, 0, mConfig.sampleRate), std::memory_order_relaxed);
}

int32_t AudioEngine::readWaveform(WaveformPoint* dest, int32_t maxPoints) {
  int32_t n = 0;
  while (n < maxPoints && mWaveform.pop(dest[n])) ++n;
  return n;
}

TrackState AudioEngine::trackState(int32_t track) const {
  TrackState s;
  if (track < 0 || track >= mConfig.trackCount) return s;
  const LoopTrack& t = mTracks[track];
  s.gain = t.gain.load(std::memory_order_relaxed);
  s.pan = t.pan.load(std::memory_order_relaxed);
  s.muted = t.muted.load(std::memory_order_relaxed);
  s.hasContent = t.hasContent.load(std::memory_order_relaxed);
  s.clearing = t.clearing.load(std::memory_order_relaxed);
  s.transport = t.transport.load(std::memory_order_relaxed);
  return s;
}

uint32_t AudioEngine::trackContentMask() const {
  uint32_t mask = 0;
  for (int32_t t = 0; t < mConfig.trackCount; ++t) {
    if (mTracks[t].hasContent.load(std::memory_order_relaxed)) mask |= 1u << t;
    if (mTracks[t].clearing.load(std::memory_order_relaxed)) mask |= 1u << (t + 16);
  }
  return mask;
}

double AudioEngine::outputLatencyMillis() const {
  std::lock_guard<std::mutex> lock(mLifecycleMutex);
  if (!mOutputStream) return -1.0;
  const auto result = mOutputStream->calculateLatencyMillis();
  return result ? result.value() : -1.0;
}

int32_t AudioEngine::framesPerBurst() const {
  std::lock_guard<std::mutex> lock(mLifecycleMutex);
  return mOutputStream ? mOutputStream->getFramesPerBurst() : 0;
}

// ---------------------------------------------------------------------------
// Input callback (realtime thread A — producer side of the ring)
// ---------------------------------------------------------------------------

oboe::DataCallbackResult AudioEngine::onInputReady(const float* audioData, int32_t numFrames) {
  // Deliberately minimal: (optional) channel remap, then one bulk copy into
  // the lock-free ring. All analysis and mixing happens on the output
  // callback so the meters stay time-aligned with what actually reaches the
  // loop and the speakers.
  const float* frames = audioData;
  const int32_t srcCh = mInputOpenedChannels;
  if (srcCh != mConfig.channelCount && numFrames <= kMaxCallbackFrames && srcCh > 0) {
    // Pull the engine's L/R from the chosen source channels (indices clamped
    // to what the device actually opened).
    const int32_t dstCh = mConfig.channelCount;
    int32_t mapL = mInputMapL.load(std::memory_order_relaxed);
    int32_t mapR = mInputMapR.load(std::memory_order_relaxed);
    if (mapL >= srcCh) mapL = 0;
    if (mapR >= srcCh) mapR = srcCh - 1;
    float* dst = mInputRemapScratch.data();
    for (int32_t f = 0; f < numFrames; ++f) {
      const float* s = audioData + static_cast<size_t>(f) * srcCh;
      float* d = dst + static_cast<size_t>(f) * dstCh;
      d[0] = s[mapL];
      if (dstCh == 2) d[1] = s[mapR];
    }
    frames = dst;
  }
  const int32_t written = mInputRing.writeFrames(frames, numFrames);
  if (written < numFrames) {
    mInputOverflowFrames.fetch_add(numFrames - written, std::memory_order_relaxed);
  }
  return oboe::DataCallbackResult::Continue;
}

// ---------------------------------------------------------------------------
// Output callback (realtime thread B — consumer side of the ring, mixer)
// ---------------------------------------------------------------------------

oboe::DataCallbackResult AudioEngine::onOutputReady(float* audioData, int32_t numFrames) {
  if (numFrames > kMaxCallbackFrames) {
    // Defensive: never overrun the pre-allocated scratch. Real burst sizes
    // are far below this bound.
    std::memset(audioData, 0,
                static_cast<size_t>(numFrames) * mConfig.channelCount * sizeof(float));
    mOversizeCallbackCount.fetch_add(1, std::memory_order_relaxed);
    mAbsOutFrame += numFrames;  // keep the calibration timeline honest
    return oboe::DataCallbackResult::Continue;
  }

  drainCommands();
  processPendingClears();
  pullInput(mInputScratch.data(), numFrames);

  if (mCalibrator.active()) {
    // Calibration owns the block: silence + ping burst out, threshold scan
    // on the same drift-corrected input the looper records, one timeline
    // (mAbsOutFrame) for both — so the measured delta is exactly the offset
    // an overdub needs. Looper/monitor/backing/metronome are all suppressed
    // so nothing competes with the ping.
    mCalibrator.process(audioData, mInputScratch.data(), numFrames, mAbsOutFrame);
    // Route the ping through the limiter too, so its look-ahead delay is part
    // of the measured round trip (overdubs pass through the same limiter).
    mLimiter.process(audioData, numFrames);
    if (!mCalibrator.active() &&
        mCalibrator.state() == LatencyCalibrator::State::Succeeded) {
      // Apply immediately: overdubs are now written this many frames behind
      // the playhead (same clamp as setRecordOffsetFrames).
      mRecordOffset.store(std::min(mCalibrator.latencyFrames(), mConfig.sampleRate),
                          std::memory_order_relaxed);
    }
    publishMeters(mInputScratch.data(), audioData, numFrames);
    mAbsOutFrame += numFrames;
    return oboe::DataCallbackResult::Continue;
  }

  // Disk tee #1 (input source): hand the drift-corrected input — exactly
  // what the looper records — to the DiskWriter through its lock-free ring.
  // No file I/O happens on this thread; writeCaptureFrames self-gates.
  if (!mCaptureMixSource.load(std::memory_order_relaxed)) {
    mSpooler.writeCaptureFrames(mInputScratch.data(), numFrames);
  }

  // Snapshots for the metronome phase-lock: renderLooper() advances the
  // playhead, but the click for THIS block must be computed against the
  // block's starting position.
  const EngineState stateAtBlockStart = mState.load(std::memory_order_relaxed);
  const int32_t playheadAtBlockStart = mPlayhead;
  const int32_t loopLenAtBlockStart = mLoopLen;

  renderLooper(audioData, mInputScratch.data(), numFrames);

  // Backing tracks stream from disk into the output mix, under the loops.
  // They are mixed AFTER the overdub path consumed the input, so like the
  // metronome they can never be recorded into a loop track.
  mSpooler.mixStreams(audioData, numFrames);

  // Disk tee #2 (mix source): loops + monitor + backing tracks, deliberately
  // BEFORE the metronome so the click never lands in the captured file.
  if (mCaptureMixSource.load(std::memory_order_relaxed)) {
    mSpooler.writeCaptureFrames(audioData, numFrames);
  }

  // MONITORING-ONLY DSP: the reverb wets what the musician HEARS — loops +
  // monitor + backing tracks — and nothing that is ever written down. Both
  // capture tees are upstream (dry), the overdub path reads the input
  // scratch (dry), and stem export reads the raw track buffers (dry), so
  // takes and exported stems stay 100% dry by construction. The metronome
  // is added AFTER, keeping the click precise and un-reverberated.
  mReverb.process(audioData, numFrames);

  // ROUTING INVARIANT: the metronome is mixed into the OUTPUT buffer only,
  // strictly after renderLooper() has finished reading the input scratch and
  // writing the loop tracks, and after both capture tees. It can reach
  // headphones/speakers but never the input ring, the input scratch, any
  // track buffer, or the capture file.
  const bool locked = mMetronomeSyncToLoop.load(std::memory_order_relaxed) &&
                      loopLenAtBlockStart > 0 &&
                      (stateAtBlockStart == EngineState::Playing ||
                       stateAtBlockStart == EngineState::Overdubbing);
  mMetronome.render(audioData, numFrames, locked ? playheadAtBlockStart : -1,
                    locked ? loopLenAtBlockStart : 0);

  // Look-ahead master limiter on the monitoring mix (loops + monitor +
  // backing + metronome). Transparent gain reduction replaces hard clipping
  // when stacked loops get loud; still output-path only, so recordings stay
  // dry. Its 5 ms latency is captured by the calibration ping (which also
  // passes through here).
  mLimiter.process(audioData, numFrames);

  // Final safety clamp catches the limiter's sub-dB overshoot only.
  const int32_t samples = numFrames * mConfig.channelCount;
  for (int32_t i = 0; i < samples; ++i) audioData[i] = clampf(audioData[i], -1.0f, 1.0f);

  publishMeters(mInputScratch.data(), audioData, numFrames);
  mAbsOutFrame += numFrames;
  return oboe::DataCallbackResult::Continue;
}

// ---------------------------------------------------------------------------
// Ring consumption with look-ahead priming and drift correction
// ---------------------------------------------------------------------------

void AudioEngine::pullInput(float* dst, int32_t frames) {
  const int32_t ch = mConfig.channelCount;
  const int32_t fill = mInputRing.framesReadable();

  if (!mPrimed) {
    // Hold silence until the cushion exists, so a slower input clock can't
    // starve the very next callback.
    if (fill >= mLookAheadFrames + frames) {
      mPrimed = true;
      mDeclickRemaining = kDeclickFrames;
    } else {
      std::memset(dst, 0, static_cast<size_t>(frames) * ch * sizeof(float));
      return;
    }
  }

  // Cushion remaining after a nominal read of this block. The controller
  // defends the band [lookAhead - slack, lookAhead + slack]:
  //   above the band  -> input clock is fast: drop oldest frames
  //   below the band  -> input clock is slow: duplicate the last frame
  // Gentle corrections are bounded to kMaxDriftSlipFrames per block (sub-
  // audible); only a gross backlog (device stall/resume) takes the one big
  // splice, masked by the de-click ramp.
  const int32_t projected = fill - frames;
  const int32_t excess = projected - (mLookAheadFrames + mDriftSlackFrames);
  const int32_t deficit = (mLookAheadFrames - mDriftSlackFrames) - projected;

  int32_t toRead = frames;
  int32_t duplicate = 0;

  if (excess > mDriftSlackFrames) {
    mInputRing.discardFrames(excess);
    mDriftDroppedFrames.fetch_add(excess, std::memory_order_relaxed);
    mDeclickRemaining = kDeclickFrames;
  } else if (excess > 0) {
    const int32_t drop = std::min(excess, kMaxDriftSlipFrames);
    mInputRing.discardFrames(drop);
    mDriftDroppedFrames.fetch_add(drop, std::memory_order_relaxed);
  } else if (deficit > 0 && frames > kMaxDriftSlipFrames) {
    duplicate = std::min(deficit, kMaxDriftSlipFrames);
    toRead = frames - duplicate;
  }

  const int32_t got = mInputRing.readFrames(dst, toRead);
  if (got < toRead) {
    // Input stalled or xrun: pad with silence and rebuild the cushion before
    // consuming again.
    std::memset(dst + static_cast<size_t>(got) * ch, 0,
                static_cast<size_t>(frames - got) * ch * sizeof(float));
    mInputUnderrunFrames.fetch_add(toRead - got, std::memory_order_relaxed);
    mPrimed = false;
    mDeclickRemaining = kDeclickFrames;
  } else if (duplicate > 0) {
    // Hold time by repeating the last real frame.
    const float* last = dst + static_cast<size_t>(toRead - 1) * ch;
    for (int32_t f = toRead; f < frames; ++f) {
      std::memcpy(dst + static_cast<size_t>(f) * ch, last, static_cast<size_t>(ch) * sizeof(float));
    }
    mDriftInsertedFrames.fetch_add(duplicate, std::memory_order_relaxed);
  }

  if (mDeclickRemaining > 0) {
    // Short linear fade-in masking the waveform discontinuity after a splice.
    for (int32_t f = 0; f < frames && mDeclickRemaining > 0; ++f, --mDeclickRemaining) {
      const float g = 1.0f - static_cast<float>(mDeclickRemaining) / kDeclickFrames;
      for (int32_t c = 0; c < ch; ++c) dst[f * ch + c] *= g;
    }
  }
}

// ---------------------------------------------------------------------------
// Looper state machine + mixer (audio thread)
// ---------------------------------------------------------------------------

void AudioEngine::drainCommands() {
  Command cmd{};
  for (int32_t i = 0; i < kMaxCommandsPerBlock && mCommands.pop(cmd); ++i) {
    applyCommand(cmd);
  }
}

void AudioEngine::applyCommand(const Command& cmd) {
  const EngineState st = mState.load(std::memory_order_relaxed);

  // While calibrating, the transport is frozen: only a cancel gets through.
  if (mCalibrator.active() && cmd.type != CommandType::CalibrateCancel) return;

  // While the restore worker fills track buffers, EVERYTHING except its own
  // commit is frozen — no arming, clearing, or playback over half-loaded data.
  if (mRestoreActive.load(std::memory_order_acquire) &&
      cmd.type != CommandType::RestoreCommit) {
    return;
  }

  // While the export worker reads the track buffers — or the undo restore
  // rewrites one — anything that would write them (record-arm, clear) is
  // refused; playback/stop still work.
  const bool writesTracks =
      cmd.type == CommandType::ToggleRecord || cmd.type == CommandType::RecordStart ||
      cmd.type == CommandType::RecordTrack || cmd.type == CommandType::ClearAll ||
      cmd.type == CommandType::ClearTrack;
  if ((mExportActive.load(std::memory_order_acquire) ||
       mUndoActive.load(std::memory_order_acquire)) &&
      writesTracks) {
    return;
  }

  switch (cmd.type) {
    // ---- Legacy transport (operate on the selected track / all tracks) ----
    case CommandType::ToggleRecord:
      recordToggleTrack(clampTrackIndex(mSelectedTrack.load(std::memory_order_relaxed)));
      break;
    case CommandType::RecordStart: {
      const int32_t t = clampTrackIndex(mSelectedTrack.load(std::memory_order_relaxed));
      const uint8_t tp = mTracks[t].transport.load(std::memory_order_relaxed);
      const bool recording = tp == static_cast<uint8_t>(TrackTransport::Recording) ||
          tp == static_cast<uint8_t>(TrackTransport::Overdubbing) ||
          (st == EngineState::CountIn && mCountInTrack == t);
      if (!recording) armRecordTrack(t);
      break;
    }
    case CommandType::RecordStop:
      recordStopTrack(clampTrackIndex(mSelectedTrack.load(std::memory_order_relaxed)));
      break;

    // ---- Per-track transport (RC-505 model) ----
    case CommandType::RecordTrack:
      recordToggleTrack(clampTrackIndex(cmd.intArg));
      break;
    case CommandType::PlayTrack:
      startTrackPlaying(clampTrackIndex(cmd.intArg));
      break;
    case CommandType::StopTrack:
      stopTrackInternal(clampTrackIndex(cmd.intArg));
      break;
    case CommandType::Play:      // legacy alias
    case CommandType::PlayAll: {
      // All Start: rewind the shared clock and start every track from the top.
      setPlayhead(0);
      for (int32_t t = 0; t < mConfig.trackCount; ++t) {
        LoopTrack& tr = mTracks[t];
        if (tr.hasContent.load(std::memory_order_relaxed) &&
            !tr.clearing.load(std::memory_order_relaxed)) {
          tr.posFrames.store(0, std::memory_order_relaxed);
          tr.transport.store(static_cast<uint8_t>(TrackTransport::Playing),
                             std::memory_order_relaxed);
        }
      }
      break;
    }
    case CommandType::Stop:      // legacy alias
    case CommandType::StopAll: {
      if (st == EngineState::RecordingMaster || st == EngineState::CountIn) {
        mMasterRecordPos = 0;
        mMasterStopAt = 0;
        mTracks[mCountInTrack].transport.store(
            static_cast<uint8_t>(TrackTransport::Empty), std::memory_order_relaxed);
        mTracks[mOverdubTrack].transport.store(
            static_cast<uint8_t>(TrackTransport::Empty), std::memory_order_relaxed);
        mState.store(EngineState::Idle, std::memory_order_relaxed);
      }
      for (int32_t t = 0; t < mConfig.trackCount; ++t) stopTrackInternal(t);
      setPlayhead(0);  // rewind so the next All Start begins at the top
      break;
    }

    case CommandType::ClearAll: {
      mState.store(EngineState::Idle, std::memory_order_relaxed);
      setLoopLength(0);
      setPlayhead(0);
      mMasterRecordPos = 0;
      mMasterStopAt = 0;
      for (int32_t t = 0; t < mConfig.trackCount; ++t) {
        mTracks[t].hasContent.store(false, std::memory_order_relaxed);
        mTracks[t].transport.store(static_cast<uint8_t>(TrackTransport::Empty),
                                   std::memory_order_relaxed);
        mTracks[t].pos = 0;
        mTracks[t].recPos = 0;
        mTracks[t].posFrames.store(0, std::memory_order_relaxed);
        beginTrackClear(mTracks[t]);
      }
      break;
    }
    case CommandType::ClearTrack: {
      const int32_t track = clampTrackIndex(cmd.intArg);
      LoopTrack& tr = mTracks[track];
      tr.hasContent.store(false, std::memory_order_relaxed);
      tr.transport.store(static_cast<uint8_t>(TrackTransport::Empty),
                         std::memory_order_relaxed);
      tr.pos = 0;
      tr.recPos = 0;
      tr.posFrames.store(0, std::memory_order_relaxed);
      beginTrackClear(tr);
      // If nothing holds content any more, drop the master loop so the next
      // recording redefines the length.
      bool anyContent = false;
      for (int32_t t = 0; t < mConfig.trackCount; ++t) {
        if (mTracks[t].hasContent.load(std::memory_order_relaxed)) { anyContent = true; break; }
      }
      if (!anyContent) { setLoopLength(0); setPlayhead(0); }
      updateGlobalState();
      break;
    }
    case CommandType::CalibrateStart: {
      // Preconditions: transport quiet and no capture running (the ping
      // must not compete with programme audio or land in a take).
      const bool transportQuiet = !anyTrackBusy() && st != EngineState::RecordingMaster &&
          st != EngineState::CountIn;
      if (!transportQuiet || mSpooler.isCapturing()) {
        mCalibrator.markFailed();
        break;
      }
      mCalibrator.begin(mAbsOutFrame);
      break;
    }
    case CommandType::CalibrateCancel: {
      mCalibrator.cancel();
      break;
    }
    case CommandType::UndoMute: {
      const int32_t track = clampTrackIndex(cmd.intArg);
      const uint8_t tp = mTracks[track].transport.load(std::memory_order_relaxed);
      if (st == EngineState::CountIn && mCountInTrack == track) {
        mMasterRecordPos = 0;
        setPlayhead(0);
        mState.store(EngineState::Idle, std::memory_order_relaxed);
      } else if (tp == static_cast<uint8_t>(TrackTransport::Recording) && mLoopLen == 0) {
        mMasterRecordPos = 0;
        setPlayhead(0);
        mState.store(EngineState::Idle, std::memory_order_relaxed);
      }
      if (tp == static_cast<uint8_t>(TrackTransport::Overdubbing)) {
        mTracks[track].transport.store(static_cast<uint8_t>(TrackTransport::Playing),
                                       std::memory_order_relaxed);
      }
      mTracks[track].hasContent.store(false, std::memory_order_relaxed);  // mixer stops reading
      break;
    }
    case CommandType::UndoCommit: {
      mTracks[clampTrackIndex(cmd.intArg)].hasContent.store(true, std::memory_order_relaxed);
      break;
    }
    case CommandType::ResetForRestore: {
      // Drop loop + content immediately, no amortized wipe (restore
      // overwrites the buffers). Only from a quiet transport.
      mState.store(EngineState::Idle, std::memory_order_relaxed);
      setLoopLength(0);
      setPlayhead(0);
      mMasterRecordPos = 0;
      mMasterStopAt = 0;
      for (int32_t t = 0; t < mConfig.trackCount; ++t) {
        mTracks[t].hasContent.store(false, std::memory_order_relaxed);
        mTracks[t].transport.store(static_cast<uint8_t>(TrackTransport::Empty),
                                   std::memory_order_relaxed);
        mTracks[t].pos = 0;
        mTracks[t].recPos = 0;
        mTracks[t].posFrames.store(0, std::memory_order_relaxed);
        mTracks[t].trimStart.store(0, std::memory_order_relaxed);
        mTracks[t].trimEnd.store(0, std::memory_order_relaxed);
      }
      break;
    }
    case CommandType::RestoreCommit: {
      // Adopt the loop the restore worker staged (mRestoreMask/mRestoreLoopLen
      // are ordered by the command queue's release/acquire pair).
      setLoopLength(mRestoreLoopLen);
      setPlayhead(0);
      int32_t firstContent = -1;
      for (int32_t t = 0; t < mConfig.trackCount; ++t) {
        if (mRestoreMask & (1u << t)) {
          mTracks[t].hasContent.store(true, std::memory_order_relaxed);
          mTracks[t].pos = 0;
          mTracks[t].posFrames.store(0, std::memory_order_relaxed);
          mTracks[t].trimStart.store(0, std::memory_order_relaxed);  // full window
          mTracks[t].trimEnd.store(0, std::memory_order_relaxed);
          mTracks[t].transport.store(static_cast<uint8_t>(TrackTransport::Stopped),
                                     std::memory_order_relaxed);  // ready; user presses play
          if (firstContent < 0) firstContent = t;
        }
      }
      if (firstContent >= 0) mMasterTrack = firstContent;
      mState.store(mRestoreLoopLen > 0 ? EngineState::Stopped : EngineState::Idle,
                   std::memory_order_relaxed);
      mRestoreActive.store(false, std::memory_order_release);
      mRestoreState.store(kRestoreDone, std::memory_order_release);
      break;
    }
  }

  // Refresh the coarse global state (used by the metronome sync + gates) from
  // the per-track states, except while a master take / count-in is pending.
  updateGlobalState();
}

// ----- Per-track transport helpers (audio thread) --------------------------

// RC-505 one-button behaviour: a fresh track records; a recording track
// closes; an overdubbing track drops back to plain playback; a stopped/playing
// track with content starts an overdub pass.
void AudioEngine::recordToggleTrack(int32_t track) {
  const EngineState st = mState.load(std::memory_order_relaxed);
  if (st == EngineState::CountIn && mCountInTrack == track) {
    mState.store(EngineState::Idle, std::memory_order_relaxed);  // cancel count-in
    mTracks[track].transport.store(static_cast<uint8_t>(TrackTransport::Empty),
                                   std::memory_order_relaxed);
    return;
  }
  const uint8_t tp = mTracks[track].transport.load(std::memory_order_relaxed);
  if (tp == static_cast<uint8_t>(TrackTransport::Recording)) {
    if (mLoopLen == 0) finalizeMasterOrQuantize(); else finalizeTrackRecord(track);
  } else if (tp == static_cast<uint8_t>(TrackTransport::Overdubbing)) {
    mTracks[track].transport.store(static_cast<uint8_t>(TrackTransport::Playing),
                                   std::memory_order_relaxed);
  } else {
    armRecordTrack(track);
  }
}

// Stop-recording semantics only (never starts a pass): closes a record/overdub.
void AudioEngine::recordStopTrack(int32_t track) {
  const EngineState st = mState.load(std::memory_order_relaxed);
  if (st == EngineState::CountIn && mCountInTrack == track) {
    mState.store(EngineState::Idle, std::memory_order_relaxed);
    mTracks[track].transport.store(static_cast<uint8_t>(TrackTransport::Empty),
                                   std::memory_order_relaxed);
    return;
  }
  const uint8_t tp = mTracks[track].transport.load(std::memory_order_relaxed);
  if (tp == static_cast<uint8_t>(TrackTransport::Recording)) {
    if (mLoopLen == 0) finalizeMasterOrQuantize(); else finalizeTrackRecord(track);
  } else if (tp == static_cast<uint8_t>(TrackTransport::Overdubbing)) {
    mTracks[track].transport.store(static_cast<uint8_t>(TrackTransport::Playing),
                                   std::memory_order_relaxed);
  }
}

// (Re)start a track from its beginning; no-op on an empty/clearing track.
// If nothing else is running, the shared loop clock rewinds to the top so the
// track truly starts from its start; if other tracks are already going, this
// one joins them locked to the current phase (they must stay in time).
void AudioEngine::startTrackPlaying(int32_t track) {
  LoopTrack& tr = mTracks[track];
  if (!tr.hasContent.load(std::memory_order_relaxed) ||
      tr.clearing.load(std::memory_order_relaxed)) {
    return;
  }
  // Single mode: starting a track stops the others (verse/chorus switching).
  const bool single = mPlayMode.load(std::memory_order_relaxed) ==
                      static_cast<int32_t>(PlayMode::Single);
  bool othersBusy = false;
  for (int32_t o = 0; o < mConfig.trackCount; ++o) {
    if (o == track) continue;
    const uint8_t tp = mTracks[o].transport.load(std::memory_order_relaxed);
    const bool busy = tp == static_cast<uint8_t>(TrackTransport::Playing) ||
                      tp == static_cast<uint8_t>(TrackTransport::Overdubbing) ||
                      tp == static_cast<uint8_t>(TrackTransport::Recording);
    if (busy) {
      if (single && tp != static_cast<uint8_t>(TrackTransport::Recording)) {
        stopTrackInternal(o);
      } else {
        othersBusy = true;
      }
    }
  }
  if (tr.oneShot.load(std::memory_order_relaxed)) {
    tr.pos = tr.trimStart.load(std::memory_order_relaxed);  // 1-shot: from window start
    tr.posFrames.store(tr.pos, std::memory_order_relaxed);
  } else {
    if (!othersBusy) setPlayhead(0);  // lone loop track: rewind to the top
    tr.posFrames.store(mPlayhead, std::memory_order_relaxed);
  }
  tr.transport.store(static_cast<uint8_t>(TrackTransport::Playing), std::memory_order_relaxed);
}

// Stop just this track. A fresh record pass is cancelled; content is retained.
void AudioEngine::stopTrackInternal(int32_t track) {
  LoopTrack& tr = mTracks[track];
  const uint8_t tp = tr.transport.load(std::memory_order_relaxed);
  if (tp == static_cast<uint8_t>(TrackTransport::Recording)) {
    if (mLoopLen == 0) {  // cancel the master take in progress
      mMasterRecordPos = 0;
      mMasterStopAt = 0;
      setPlayhead(0);
    } else {
      tr.recPos = 0;  // discard the partial fixed-length take
    }
  }
  tr.transport.store(tr.hasContent.load(std::memory_order_relaxed)
                         ? static_cast<uint8_t>(TrackTransport::Stopped)
                         : static_cast<uint8_t>(TrackTransport::Empty),
                     std::memory_order_relaxed);
}

bool AudioEngine::anyTrackBusy() const {
  for (int32_t t = 0; t < mConfig.trackCount; ++t) {
    const uint8_t tp = mTracks[t].transport.load(std::memory_order_relaxed);
    if (tp != static_cast<uint8_t>(TrackTransport::Empty) &&
        tp != static_cast<uint8_t>(TrackTransport::Stopped)) {
      return true;
    }
  }
  return false;
}

// Coarse mState for the metronome sync + gates. Leaves the transient master-
// take / count-in states alone (they are driven explicitly).
void AudioEngine::updateGlobalState() {
  const EngineState cur = mState.load(std::memory_order_relaxed);
  if (cur == EngineState::RecordingMaster || cur == EngineState::CountIn) return;
  bool anyOverdub = false, anyPlaying = false;
  for (int32_t t = 0; t < mConfig.trackCount; ++t) {
    const uint8_t tp = mTracks[t].transport.load(std::memory_order_relaxed);
    if (tp == static_cast<uint8_t>(TrackTransport::Overdubbing)) anyOverdub = true;
    else if (tp == static_cast<uint8_t>(TrackTransport::Playing)) anyPlaying = true;
  }
  EngineState ns;
  if (anyOverdub) ns = EngineState::Overdubbing;
  else if (anyPlaying) ns = EngineState::Playing;
  else if (mLoopLen > 0) ns = EngineState::Stopped;
  else ns = EngineState::Idle;
  mState.store(ns, std::memory_order_relaxed);
}

// Start record/overdub on a specific track. The first-ever recording defines
// the shared master loop length; while a master exists, an empty track records
// a fresh one-loop take and a track with content starts an overdub pass.
void AudioEngine::armRecordTrack(int32_t track) {
  LoopTrack& t = mTracks[track];
  if (t.clearing.load(std::memory_order_relaxed)) return;  // not armable mid-clear
  mOverdubTrack = track;
  if (mLoopLen == 0) {
    // No master loop yet — this recording defines the loop length.
    mMasterTrack = track;
    mMasterRecordPos = 0;
    mMasterStopAt = 0;
    t.recPos = 0;
    t.pos = 0;
    setPlayhead(0);
    if (mCountInEnabled.load(std::memory_order_relaxed) && mMetronome.isActive()) {
      // Wait for the next downbeat before capturing, so the loop starts on
      // the bar. The intervening clicks are the count-in.
      mCountInTrack = track;
      mCountInStartBeat = mMetronome.beatCount();
      mState.store(EngineState::CountIn, std::memory_order_relaxed);
    } else {
      t.transport.store(static_cast<uint8_t>(TrackTransport::Recording),
                        std::memory_order_relaxed);
      mState.store(EngineState::RecordingMaster, std::memory_order_relaxed);
    }
  } else if (t.hasContent.load(std::memory_order_relaxed)) {
    // Overdub existing content: make sure it is playing, then sum input.
    const uint8_t tp = t.transport.load(std::memory_order_relaxed);
    if (tp == static_cast<uint8_t>(TrackTransport::Stopped) ||
        tp == static_cast<uint8_t>(TrackTransport::Empty)) {
      t.pos = 0;  // start from the top
    }
    t.transport.store(static_cast<uint8_t>(TrackTransport::Overdubbing),
                      std::memory_order_relaxed);
  } else {
    // Fresh fixed-length take: capture exactly one master loop, then play.
    t.recPos = 0;
    t.pos = 0;
    t.transport.store(static_cast<uint8_t>(TrackTransport::Recording),
                      std::memory_order_relaxed);
  }
}

// Close a fresh FIXED-length take (a track recording one master loop while a
// master already exists). The master-defining take goes through
// finalizeMasterLoop() instead.
void AudioEngine::finalizeTrackRecord(int32_t track) {
  LoopTrack& t = mTracks[track];
  t.hasContent.store(true, std::memory_order_relaxed);
  t.recPos = 0;
  t.pos = 0;
  t.posFrames.store(0, std::memory_order_relaxed);
  t.trimStart.store(0, std::memory_order_relaxed);  // fresh take: full window
  t.trimEnd.store(0, std::memory_order_relaxed);
  t.transport.store(static_cast<uint8_t>(TrackTransport::Playing), std::memory_order_relaxed);
}

// Whole-bar length in frames at the current tempo, or 0 if the click is off.
int32_t AudioEngine::barFramesOrZero() const {
  if (!mMetronome.isActive()) return 0;
  float bpm = 120.0f;
  int32_t beats = 4;
  mMetronome.tempo(bpm, beats);
  const double beatFrames = static_cast<double>(mConfig.sampleRate) * 60.0 / bpm;
  return static_cast<int32_t>(beatFrames * beats + 0.5);
}

// Master-recording stop: bar-round the length when quantization is on.
// Trims a late stop back to the nearest bar immediately; a short stop keeps
// recording until the bar line (mMasterStopAt) and finalizes there.
void AudioEngine::finalizeMasterOrQuantize() {
  const int32_t barFrames =
      mQuantizeBars.load(std::memory_order_relaxed) ? barFramesOrZero() : 0;
  if (barFrames > 0 && mMasterRecordPos > 0) {
    int32_t bars = (mMasterRecordPos + barFrames / 2) / barFrames;
    if (bars < 1) bars = 1;
    const int32_t target = std::min(bars * barFrames, mMaxLoopFrames);
    if (target > mMasterRecordPos) {
      mMasterStopAt = target;  // keep recording to the bar line
      return;
    }
    mMasterRecordPos = target;  // trim a late stop back to the bar
  }
  finalizeMasterLoop();
}

void AudioEngine::beginTrackClear(LoopTrack& track) {
  track.clearCursor = 0;
  track.playOffset.store(0, std::memory_order_relaxed);  // reset start-shift
  track.trimStart.store(0, std::memory_order_relaxed);   // reset trim window
  track.trimEnd.store(0, std::memory_order_relaxed);
  track.clearing.store(true, std::memory_order_relaxed);
}

void AudioEngine::processPendingClears() {
  // Zeroing a whole track (tens of MB) in one callback would blow the
  // deadline, and doing it from the control thread would race the mixer.
  // Instead the audio thread clears one bounded chunk per callback; a
  // clearing track is excluded from mixing and cannot be armed.
  for (int32_t t = 0; t < mConfig.trackCount; ++t) {
    LoopTrack& track = mTracks[t];
    if (!track.clearing.load(std::memory_order_relaxed)) continue;
    const int32_t n = std::min(kClearChunkFrames, mMaxLoopFrames - track.clearCursor);
    std::memset(&track.data[static_cast<size_t>(track.clearCursor) * mConfig.channelCount], 0,
                static_cast<size_t>(n) * mConfig.channelCount * sizeof(float));
    track.clearCursor += n;
    if (track.clearCursor >= mMaxLoopFrames) {
      track.clearing.store(false, std::memory_order_relaxed);
    }
    break;  // amortize: at most one chunk per callback
  }
}

void AudioEngine::finalizeMasterLoop() {
  LoopTrack& t = mTracks[mOverdubTrack];
  if (mMasterRecordPos > 0) {
    setLoopLength(mMasterRecordPos);
    t.hasContent.store(true, std::memory_order_relaxed);
    t.pos = 0;
    t.posFrames.store(0, std::memory_order_relaxed);
    t.trimStart.store(0, std::memory_order_relaxed);  // fresh take: full window
    t.trimEnd.store(0, std::memory_order_relaxed);
    t.transport.store(static_cast<uint8_t>(TrackTransport::Playing), std::memory_order_relaxed);
    mMasterTrack = mOverdubTrack;
    setPlayhead(0);
    mState.store(EngineState::Playing, std::memory_order_relaxed);
  } else {
    t.transport.store(static_cast<uint8_t>(TrackTransport::Empty), std::memory_order_relaxed);
    mState.store(EngineState::Idle, std::memory_order_relaxed);
  }
  mMasterRecordPos = 0;
  mMasterStopAt = 0;
}

void AudioEngine::renderLooper(float* out, const float* in, int32_t frames) {
  const int32_t ch = mConfig.channelCount;
  const int32_t samples = frames * ch;
  using TT = TrackTransport;

  // Base layer: live input monitoring.
  const float monitorGain = mMonitorGain.load(std::memory_order_relaxed);
  for (int32_t i = 0; i < samples; ++i) out[i] = in[i] * monitorGain;

  const EngineState st = mState.load(std::memory_order_relaxed);

  // ---- Count-in: hold until the next downbeat, then begin the master take.
  if (st == EngineState::CountIn) {
    // The metronome renders after this function; its beat state here is from
    // the previous callback, so recording begins within one callback of the
    // downbeat. If the click was switched off mid-count-in the beat counter
    // freezes, so fall through to recording rather than soft-locking.
    const bool downbeat =
        mMetronome.beatCount() > mCountInStartBeat && mMetronome.beatInBar() == 0;
    if (downbeat || !mMetronome.isActive()) {
      mOverdubTrack = mCountInTrack;
      mMasterTrack = mCountInTrack;
      mMasterRecordPos = 0;
      setPlayhead(0);
      mTracks[mCountInTrack].pos = 0;
      mTracks[mCountInTrack].transport.store(static_cast<uint8_t>(TT::Recording),
                                             std::memory_order_relaxed);
      mState.store(EngineState::RecordingMaster, std::memory_order_relaxed);
    }
  }

  // ---- Master take: the first recording defines the shared loop length. It
  // captures into mOverdubTrack and grows mMasterRecordPos; it does not play.
  if (mState.load(std::memory_order_relaxed) == EngineState::RecordingMaster && mLoopLen == 0) {
    LoopTrack& rec = mTracks[mOverdubTrack];
    int32_t cap = mMaxLoopFrames;
    if (mMasterStopAt > 0) cap = std::min(cap, mMasterStopAt);  // scheduled bar-line stop
    const int32_t n = std::min(frames, cap - mMasterRecordPos);
    if (n > 0) {
      std::memcpy(&rec.data[static_cast<size_t>(mMasterRecordPos) * ch], in,
                  static_cast<size_t>(n) * ch * sizeof(float));
      mMasterRecordPos += n;
    }
    mPlayheadFrames.store(mMasterRecordPos, std::memory_order_relaxed);
    if (mMasterStopAt > 0 && mMasterRecordPos >= mMasterStopAt) {
      mMasterStopAt = 0;
      finalizeMasterLoop();  // reached the quantized bar line
    } else if (mMasterRecordPos >= mMaxLoopFrames) {
      finalizeMasterLoop();  // hit capacity: close the loop automatically
    }
  }

  // ---- With a master loop present: every track shares one loop clock
  // (mPlayhead = the global phase), so layers stay locked. Playing tracks are
  // read at the phase; fixed-length takes and overdubs are written at the
  // phase (minus the calibrated round-trip), so new material lands beat-locked
  // to what the performer heard. Tracks that are stopped are simply silent.
  if (mLoopLen > 0) {
    const int32_t phaseStart = mPlayhead;
    int32_t recOffset = mRecordOffset.load(std::memory_order_relaxed) % mLoopLen;
    if (recOffset < 0) recOffset += mLoopLen;

    // Read (playback) targets.
    const float* srcs[kMaxTracks];
    float gainL[kMaxTracks];
    float gainR[kMaxTracks];
    int32_t rd[kMaxTracks];
    int32_t rdTrack[kMaxTracks];
    int32_t trS[kMaxTracks];  // audible-window start (buffer frames)
    int32_t trE[kMaxTracks];  // audible-window end
    bool rdOneShot[kMaxTracks];   // 1-shot: independent playhead, stops at trE
    int32_t rdPosStart[kMaxTracks];
    int32_t nRead = 0;
    // Write targets: fresh fixed-length takes (overwrite, bounded to one loop)
    // and overdubs (sum, continuous).
    float* wData[kMaxTracks];
    int32_t wPos[kMaxTracks];
    int32_t wCount[kMaxTracks];  // frames to write THIS block
    bool wSum[kMaxTracks];       // true: overdub (+=); false: fresh take (=)
    int32_t wTrack[kMaxTracks];
    int32_t nWrite = 0;
    bool active = false;  // does the loop clock advance this block?

    for (int32_t t = 0; t < mConfig.trackCount; ++t) {
      LoopTrack& track = mTracks[t];
      const uint8_t tp = track.transport.load(std::memory_order_relaxed);
      const bool playing = tp == static_cast<uint8_t>(TT::Playing) ||
                           tp == static_cast<uint8_t>(TT::Overdubbing);
      const bool recording = tp == static_cast<uint8_t>(TT::Recording);
      if (playing && track.hasContent.load(std::memory_order_relaxed) &&
          !track.muted.load(std::memory_order_relaxed) &&
          !track.clearing.load(std::memory_order_relaxed)) {
        srcs[nRead] = track.data.data();
        const float g = track.gain.load(std::memory_order_relaxed);
        const float pan = (ch == 2) ? track.pan.load(std::memory_order_relaxed) : 0.0f;
        gainL[nRead] = g * (pan > 0.0f ? 1.0f - pan : 1.0f);
        gainR[nRead] = g * (pan < 0.0f ? 1.0f + pan : 1.0f);
        trS[nRead] = track.trimStart.load(std::memory_order_relaxed);
        const int32_t te = track.trimEnd.load(std::memory_order_relaxed);
        trE[nRead] = (te <= 0 || te > mLoopLen) ? mLoopLen : te;
        const bool os = track.oneShot.load(std::memory_order_relaxed);
        rdOneShot[nRead] = os;
        if (os) {
          // 1-shot: play the window once from its own playhead (set to the
          // window start on trigger), independent of the shared loop clock.
          int32_t p = track.pos;
          if (p < 0) p = 0;
          if (p >= mLoopLen) p = mLoopLen - 1;
          rd[nRead] = p;
          rdPosStart[nRead] = p;
        } else {
          int32_t off = track.playOffset.load(std::memory_order_relaxed) % mLoopLen;
          if (off < 0) off += mLoopLen;
          rd[nRead] = (phaseStart + off) % mLoopLen;
          rdPosStart[nRead] = 0;
        }
        rdTrack[nRead] = t;
        ++nRead;
      }
      if (recording || tp == static_cast<uint8_t>(TT::Overdubbing)) {
        wData[nWrite] = track.data.data();
        int32_t wp = phaseStart - recOffset;
        if (wp < 0) wp += mLoopLen;
        wPos[nWrite] = wp;
        wSum[nWrite] = (tp == static_cast<uint8_t>(TT::Overdubbing));
        // A fresh take captures exactly one loop; an overdub runs continuously.
        wCount[nWrite] = wSum[nWrite] ? frames : std::min(frames, mLoopLen - track.recPos);
        wTrack[nWrite] = t;
        ++nWrite;
      }
      if (playing || recording) active = true;
    }

    for (int32_t f = 0; f < frames; ++f) {
      float* o = out + static_cast<size_t>(f) * ch;
      const float* inF = in + static_cast<size_t>(f) * ch;
      for (int32_t a = 0; a < nRead; ++a) {
        // Non-destructive trim: only the [trimStart, trimEnd) window sounds.
        if (rd[a] >= trS[a] && rd[a] < trE[a]) {
          const float* s = srcs[a] + static_cast<size_t>(rd[a]) * ch;
          o[0] += s[0] * gainL[a];
          if (ch == 2) o[1] += s[1] * gainR[a];
        }
        if (++rd[a] >= mLoopLen) rd[a] = 0;
      }
      for (int32_t w = 0; w < nWrite; ++w) {
        if (f >= wCount[w]) continue;
        float* d = wData[w] + static_cast<size_t>(wPos[w]) * ch;
        if (wSum[w]) {
          for (int32_t c = 0; c < ch; ++c) d[c] += inF[c];
        } else {
          for (int32_t c = 0; c < ch; ++c) d[c] = inF[c];
        }
        if (++wPos[w] >= mLoopLen) wPos[w] = 0;
      }
    }

    // Grow fresh-take cursors and close a take once a full loop is captured.
    for (int32_t w = 0; w < nWrite; ++w) {
      if (wSum[w]) continue;  // overdub: no fixed length
      LoopTrack& tr = mTracks[wTrack[w]];
      tr.recPos += wCount[w];
      tr.posFrames.store(tr.recPos, std::memory_order_relaxed);
      if (tr.recPos >= mLoopLen) finalizeTrackRecord(wTrack[w]);
    }

    // Advance the shared loop clock and publish it as each playing track's
    // position (they are all locked to the same phase).
    if (active) {
      int32_t np = phaseStart + frames;
      np %= mLoopLen;
      setPlayhead(np);
    }
    for (int32_t a = 0; a < nRead; ++a) {
      if (rdOneShot[a]) {
        // Advance the 1-shot's own playhead; stop it once the window is done.
        const int32_t np = rdPosStart[a] + frames;
        mTracks[rdTrack[a]].pos = np;
        mTracks[rdTrack[a]].posFrames.store(std::min(np, trE[a]), std::memory_order_relaxed);
        if (np >= trE[a]) {
          mTracks[rdTrack[a]].transport.store(static_cast<uint8_t>(TT::Stopped),
                                              std::memory_order_relaxed);
        }
      } else {
        mTracks[rdTrack[a]].posFrames.store(mPlayhead, std::memory_order_relaxed);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Metering (audio thread -> UI)
// ---------------------------------------------------------------------------

void AudioEngine::publishMeters(const float* in, const float* out, int32_t frames) {
  WaveformPoint p{};
  computeMeter(in, frames * mConfig.channelCount, p.inputRms, p.inputPeak);
  computeMeter(out, frames * mConfig.channelCount, p.mixRms, p.mixPeak);
  p.playheadFrames = mPlayheadFrames.load(std::memory_order_relaxed);
  p.loopLengthFrames = mLoopLengthFrames.load(std::memory_order_relaxed);
  p.state = static_cast<uint8_t>(mState.load(std::memory_order_relaxed));
  if (!mWaveform.push(p)) {
    // UI fell behind; dropping the newest point is harmless for a meter.
    mMeterDropCount.fetch_add(1, std::memory_order_relaxed);
  }
}

void AudioEngine::computeMeter(const float* samples, int32_t count, float& rms, float& peak) {
  float sumSq = 0.0f;
  float pk = 0.0f;
  for (int32_t i = 0; i < count; ++i) {
    const float v = samples[i];
    sumSq += v * v;
    const float a = v < 0.0f ? -v : v;
    if (a > pk) pk = a;
  }
  rms = count > 0 ? std::sqrt(sumSq / static_cast<float>(count)) : 0.0f;
  peak = pk;
}

// ---------------------------------------------------------------------------
// Small audio-thread helpers
// ---------------------------------------------------------------------------

void AudioEngine::setLoopLength(int32_t frames) {
  mLoopLen = frames;
  mLoopLengthFrames.store(frames, std::memory_order_relaxed);
}

void AudioEngine::setPlayhead(int32_t frames) {
  mPlayhead = frames;
  mPlayheadFrames.store(frames, std::memory_order_relaxed);
}

int32_t AudioEngine::clampTrackIndex(int32_t track) const {
  return clampi(track, 0, mConfig.trackCount - 1);
}

}  // namespace looper
