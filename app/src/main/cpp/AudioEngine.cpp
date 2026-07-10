#include "AudioEngine.h"

#include <cmath>

#if defined(__ANDROID__)
#include <android/log.h>
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "LooperEngine", __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, "LooperEngine", __VA_ARGS__)
#else
// Host builds (unit tests / static analysis) have no liblog.
#define LOGI(...) ((void)0)
#define LOGW(...) ((void)0)
#endif
// NOTE: LOGI/LOGW are control-plane only. Nothing in the onAudioReady path logs.

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

  mInputCallback = std::make_shared<StreamCallback>(*this, oboe::Direction::Input);
  mOutputCallback = std::make_shared<StreamCallback>(*this, oboe::Direction::Output);
}

AudioEngine::~AudioEngine() { stop(); }

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
  // Both are pinned to the same nominal sample rate via Oboe's sample-rate
  // conversion, so the ring-buffer frame math is consistent even when the
  // devices run at different native rates. Residual clock drift between the
  // two hardware clocks is handled by pullInput().
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
  // samples use). Fired when a device disappears — e.g. the USB interface
  // or Bluetooth headset disconnects.
  if (error != oboe::Result::ErrorDisconnected) return;

  std::lock_guard<std::mutex> lock(mLifecycleMutex);
  if (!mUserRunning.load(std::memory_order_acquire)) return;
  // Ignore stale events from a stream generation we already replaced.
  if (stream != mInputStream.get() && stream != mOutputStream.get()) return;

  LOGI("stream disconnected; restarting duplex pair on current default devices");
  stopLocked();
  const oboe::Result result = startLocked();
  if (result != oboe::Result::OK) {
    LOGW("restart after disconnect failed: %s", oboe::convertToText(result));
  }
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
    return oboe::DataCallbackResult::Continue;
  }

  drainCommands();
  processPendingClears();
  pullInput(mInputScratch.data(), numFrames);
  renderLooper(audioData, mInputScratch.data(), numFrames);

  // ROUTING INVARIANT: the metronome is mixed into the OUTPUT buffer only,
  // strictly after renderLooper() has finished reading the input scratch and
  // writing the loop tracks. It can reach headphones/speakers but never the
  // input ring, the input scratch, or any track buffer — a click can never
  // be recorded.
  mMetronome.render(audioData, numFrames);

  // Hard safety clamp on the final mix (loops + monitor + metronome); a
  // proper look-ahead limiter is a later-phase item.
  const int32_t samples = numFrames * mConfig.channelCount;
  for (int32_t i = 0; i < samples; ++i) audioData[i] = clampf(audioData[i], -1.0f, 1.0f);

  publishMeters(mInputScratch.data(), audioData, numFrames);
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
