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
  for (int32_t t = 0; t < mConfig.trackCount; ++t) {
    mTracks[t].data.assign(static_cast<size_t>(mMaxLoopFrames) * mConfig.channelCount, 0.0f);
  }

  mMetronome.configure(mConfig.sampleRate, mConfig.channelCount);
  mCalibrator.configure(mConfig.sampleRate, mConfig.channelCount);

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

  oboe::AudioStreamBuilder inBuilder;
  inBuilder.setDirection(oboe::Direction::Input)
      ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
      ->setSharingMode(oboe::SharingMode::Shared)
      ->setFormat(oboe::AudioFormat::Float)
      ->setFormatConversionAllowed(true)
      ->setChannelCount(mConfig.channelCount)
      ->setChannelConversionAllowed(true)
      ->setSampleRate(mConfig.sampleRate)
      ->setSampleRateConversionQuality(oboe::SampleRateConversionQuality::Medium)
      // No AGC / noise suppression / echo cancellation in the capture path —
      // essential for a music source like the Zoom H2n.
      ->setInputPreset(oboe::InputPreset::Unprocessed)
      ->setDeviceId(mConfig.inputDeviceId)
      ->setDataCallback(mInputCallback)
      ->setErrorCallback(mInputCallback);

  result = inBuilder.openStream(mInputStream);
  if (result != oboe::Result::OK) {
    LOGW("failed to open input stream: %s", oboe::convertToText(result));
    return result;
  }

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

    LOGI("restarting duplex pair on current default devices");
    stopLocked();
    result = startLocked();
    attempted = true;
    if (result != oboe::Result::OK) {
      LOGW("restart after disconnect failed: %s", oboe::convertToText(result));
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
void AudioEngine::play() { pushCommand({CommandType::Play, 0}); }
void AudioEngine::stopPlayback() { pushCommand({CommandType::Stop, 0}); }
void AudioEngine::clearAll() { pushCommand({CommandType::ClearAll, 0}); }

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

void AudioEngine::clearTrack(int32_t track) {
  if (track < 0 || track >= mConfig.trackCount) return;
  pushCommand({CommandType::ClearTrack, track});
}

void AudioEngine::selectTrack(int32_t track) {
  if (track < 0 || track >= mConfig.trackCount) return;
  mSelectedTrack.store(track, std::memory_order_relaxed);
}

void AudioEngine::setTrackGain(int32_t track, float gain) {
  if (track < 0 || track >= mConfig.trackCount) return;
  mTracks[track].gain.store(clampf(gain, 0.0f, 4.0f), std::memory_order_relaxed);
}

void AudioEngine::setTrackMuted(int32_t track, bool muted) {
  if (track < 0 || track >= mConfig.trackCount) return;
  mTracks[track].muted.store(muted, std::memory_order_relaxed);
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
  s.muted = t.muted.load(std::memory_order_relaxed);
  s.hasContent = t.hasContent.load(std::memory_order_relaxed);
  s.clearing = t.clearing.load(std::memory_order_relaxed);
  return s;
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
  // Deliberately minimal: one bulk copy into the lock-free ring. All
  // analysis and mixing happens on the output callback so the meters stay
  // time-aligned with what actually reaches the loop and the speakers.
  const int32_t written = mInputRing.writeFrames(audioData, numFrames);
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

  // Hard safety clamp on the final mix (loops + monitor + backing +
  // metronome); a proper look-ahead limiter is a later-phase item.
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

  // While the export worker reads the track buffers, anything that would
  // write them (record-arm, clear) is refused; playback/stop still work.
  if (mExportActive.load(std::memory_order_acquire) &&
      (cmd.type == CommandType::ToggleRecord || cmd.type == CommandType::RecordStart ||
       cmd.type == CommandType::ClearAll || cmd.type == CommandType::ClearTrack)) {
    return;
  }

  switch (cmd.type) {
    case CommandType::ToggleRecord: {
      if (st == EngineState::RecordingMaster) {
        finalizeMasterLoop();
      } else if (st == EngineState::Overdubbing) {
        mState.store(EngineState::Playing, std::memory_order_relaxed);
      } else {
        armRecording(st);
      }
      break;
    }
    case CommandType::RecordStart: {
      // Idempotent: ignored if a recording/overdub pass is already running.
      if (st != EngineState::RecordingMaster && st != EngineState::Overdubbing) {
        armRecording(st);
      }
      break;
    }
    case CommandType::RecordStop: {
      if (st == EngineState::RecordingMaster) {
        finalizeMasterLoop();
      } else if (st == EngineState::Overdubbing) {
        mState.store(EngineState::Playing, std::memory_order_relaxed);
      }
      break;
    }
    case CommandType::Play: {
      if (mLoopLen > 0) {
        setPlayhead(0);
        mState.store(EngineState::Playing, std::memory_order_relaxed);
      }
      break;
    }
    case CommandType::Stop: {
      if (st == EngineState::RecordingMaster) {
        // Cancel the take; nothing usable was committed.
        mMasterRecordPos = 0;
        setPlayhead(0);
        mState.store(EngineState::Idle, std::memory_order_relaxed);
      } else {
        mState.store(mLoopLen > 0 ? EngineState::Stopped : EngineState::Idle,
                     std::memory_order_relaxed);
      }
      break;
    }
    case CommandType::ClearAll: {
      mState.store(EngineState::Idle, std::memory_order_relaxed);
      setLoopLength(0);
      setPlayhead(0);
      mMasterRecordPos = 0;
      for (int32_t t = 0; t < mConfig.trackCount; ++t) {
        mTracks[t].hasContent.store(false, std::memory_order_relaxed);
        beginTrackClear(mTracks[t]);
      }
      break;
    }
    case CommandType::ClearTrack: {
      const int32_t track = clampTrackIndex(cmd.intArg);
      if (st == EngineState::Overdubbing && track == mOverdubTrack) {
        mState.store(EngineState::Playing, std::memory_order_relaxed);
      }
      mTracks[track].hasContent.store(false, std::memory_order_relaxed);
      beginTrackClear(mTracks[track]);
      break;
    }
    case CommandType::CalibrateStart: {
      // Preconditions: transport quiet and no capture running (the ping
      // must not compete with programme audio or land in a take).
      const bool transportQuiet = (st == EngineState::Idle || st == EngineState::Stopped);
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
  }
}

// Idle / Playing / Stopped -> start recording into the selected track: the
// first-ever recording defines the master loop length; afterwards it's an
// overdub pass (from Stopped, playback restarts at the top).
void AudioEngine::armRecording(EngineState current) {
  const int32_t track = clampTrackIndex(mSelectedTrack.load(std::memory_order_relaxed));
  LoopTrack& t = mTracks[track];
  if (t.clearing.load(std::memory_order_relaxed)) return;  // not armable mid-clear
  mOverdubTrack = track;
  if (mLoopLen == 0) {
    // No master loop yet — this recording defines the loop length.
    mMasterRecordPos = 0;
    setPlayhead(0);
    mState.store(EngineState::RecordingMaster, std::memory_order_relaxed);
  } else {
    if (current == EngineState::Stopped) setPlayhead(0);
    t.hasContent.store(true, std::memory_order_relaxed);
    mState.store(EngineState::Overdubbing, std::memory_order_relaxed);
  }
}

void AudioEngine::beginTrackClear(LoopTrack& track) {
  track.clearCursor = 0;
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
  if (mMasterRecordPos > 0) {
    setLoopLength(mMasterRecordPos);
    mTracks[mOverdubTrack].hasContent.store(true, std::memory_order_relaxed);
    setPlayhead(0);
    mState.store(EngineState::Playing, std::memory_order_relaxed);
  } else {
    mState.store(EngineState::Idle, std::memory_order_relaxed);
  }
  mMasterRecordPos = 0;
}

void AudioEngine::renderLooper(float* out, const float* in, int32_t frames) {
  const int32_t ch = mConfig.channelCount;
  const int32_t samples = frames * ch;

  // Base layer: live input monitoring.
  const float monitorGain = mMonitorGain.load(std::memory_order_relaxed);
  for (int32_t i = 0; i < samples; ++i) out[i] = in[i] * monitorGain;

  const EngineState st = mState.load(std::memory_order_relaxed);

  if (st == EngineState::RecordingMaster) {
    LoopTrack& rec = mTracks[mOverdubTrack];
    const int32_t n = std::min(frames, mMaxLoopFrames - mMasterRecordPos);
    std::memcpy(&rec.data[static_cast<size_t>(mMasterRecordPos) * ch], in,
                static_cast<size_t>(n) * ch * sizeof(float));
    mMasterRecordPos += n;
    // Publish record progress so the UI can show elapsed loop time.
    mPlayheadFrames.store(mMasterRecordPos, std::memory_order_relaxed);
    if (mMasterRecordPos >= mMaxLoopFrames) {
      finalizeMasterLoop();  // hit capacity: close the loop automatically
    }
  } else if ((st == EngineState::Playing || st == EngineState::Overdubbing) && mLoopLen > 0) {
    // Snapshot per-track parameters once per block, never per frame.
    const float* srcs[kMaxTracks];
    float gains[kMaxTracks];
    int32_t active = 0;
    for (int32_t t = 0; t < mConfig.trackCount; ++t) {
      LoopTrack& track = mTracks[t];
      if (!track.hasContent.load(std::memory_order_relaxed) ||
          track.muted.load(std::memory_order_relaxed) ||
          track.clearing.load(std::memory_order_relaxed)) {
        continue;
      }
      srcs[active] = track.data.data();
      gains[active] = track.gain.load(std::memory_order_relaxed);
      ++active;
    }

    // Overdub target: write the live input this many frames BEHIND the
    // playhead so it lands where the performer actually heard the loop
    // (round-trip latency measured by the Phase-3 calibration tool).
    float* odData = nullptr;
    int32_t odPos = 0;
    if (st == EngineState::Overdubbing) {
      odData = mTracks[mOverdubTrack].data.data();
      const int32_t offset = mRecordOffset.load(std::memory_order_relaxed) % mLoopLen;
      odPos = mPlayhead - offset;
      if (odPos < 0) odPos += mLoopLen;
    }

    int32_t ph = mPlayhead;
    for (int32_t f = 0; f < frames; ++f) {
      float* o = out + static_cast<size_t>(f) * ch;
      for (int32_t t = 0; t < active; ++t) {
        const float* s = srcs[t] + static_cast<size_t>(ph) * ch;
        for (int32_t c = 0; c < ch; ++c) o[c] += s[c] * gains[t];
      }
      if (odData != nullptr) {
        const float* inF = in + static_cast<size_t>(f) * ch;
        float* d = odData + static_cast<size_t>(odPos) * ch;
        for (int32_t c = 0; c < ch; ++c) d[c] += inF[c];
        if (++odPos >= mLoopLen) odPos = 0;
      }
      if (++ph >= mLoopLen) ph = 0;
    }
    setPlayhead(ph);
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
