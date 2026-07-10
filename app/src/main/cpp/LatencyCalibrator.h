#ifndef LOOPER_LATENCY_CALIBRATOR_H
#define LOOPER_LATENCY_CALIBRATOR_H

/*
 * LatencyCalibrator — ping-and-listen round-trip latency measurement.
 *
 * Ping: a 2 ms Hann-windowed sine burst at 3 kHz is injected into the
 * output stream (the window keeps the onset click-free and the energy
 * concentrated at the probe frequency).
 *
 * Listen: the drift-corrected input feed — the exact signal the looper
 * records — is scanned for the first sample crossing a peak threshold that
 * was calibrated against the measured noise floor during a 100 ms lead-in.
 *
 * The delta is counted in OUTPUT frames on one timeline (the engine's
 * monotonic output-frame counter), so it captures the full recording round
 * trip: output buffering + DAC + acoustic/cable path + ADC + resampler +
 * input buffering + the engine's look-ahead cushion. That is precisely the
 * offset by which an overdub must be written BEHIND the playhead so it
 * lands where the performer heard the loop.
 *
 * Robustness: kPings measurements are taken, separated by an echo-settle
 * cooldown; the run succeeds only if two pings agree within 1 ms (their
 * average is the result). A noisy room, a missing loopback path, or an
 * unplugged monitor therefore fails cleanly instead of storing garbage.
 *
 * Threading: begin()/cancel()/markFailed()/process() run on the AUDIO
 * thread (the engine drives them via its command queue); process() does no
 * allocation, locking, or I/O. state()/latencyFrames() are atomics, safe
 * from any thread (UI polls them).
 */

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace looper {

class LatencyCalibrator {
 public:
  enum class State : int32_t { Idle = 0, Running = 1, Succeeded = 2, Failed = 3 };

  static constexpr int32_t kPings = 3;
  static constexpr float kPingHz = 3000.0f;
  static constexpr float kPingAmplitude = 0.8f;

  // Control thread, before the streams start.
  void configure(int32_t sampleRate, int32_t channelCount) {
    mSampleRate = sampleRate;
    mChannels = channelCount;
    mPingFrames = sampleRate / 500;        // 2 ms burst
    mLeadInFrames = sampleRate / 10;       // 100 ms noise-floor measurement
    mTimeoutFrames = sampleRate;           // 1 s to hear each ping back
    mCooldownFrames = sampleRate / 4;      // 250 ms echo settle between pings
    mToleranceFrames = sampleRate / 1000;  // pings must agree within 1 ms
  }

  // ----- AUDIO THREAD -----

  void begin(int64_t absFrame) {
    mNoisePeak = 0.0f;
    mPingIndex = 0;
    mPhase = Phase::LeadIn;
    mPhaseMark = absFrame + mLeadInFrames;
    mActive = true;
    mState.store(static_cast<int32_t>(State::Running), std::memory_order_relaxed);
  }

  void cancel() {
    if (!mActive) return;
    mActive = false;
    mState.store(static_cast<int32_t>(State::Idle), std::memory_order_relaxed);
  }

  // For precondition failures (transport running, capture active, ...).
  void markFailed() {
    mActive = false;
    mState.store(static_cast<int32_t>(State::Failed), std::memory_order_relaxed);
  }

  bool active() const { return mActive; }

  // Owns the block while active: overwrites `out` with silence + the ping
  // burst and scans `in` for the return. `absFrame` is the engine's
  // monotonic output-frame counter at the first frame of this block.
  void process(float* out, const float* in, int32_t frames, int64_t absFrame) {
    std::memset(out, 0, static_cast<size_t>(frames) * mChannels * sizeof(float));
    if (!mActive) return;

    switch (mPhase) {
      case Phase::LeadIn: {
        const int32_t samples = frames * mChannels;
        for (int32_t i = 0; i < samples; ++i) {
          const float a = in[i] < 0.0f ? -in[i] : in[i];
          if (a > mNoisePeak) mNoisePeak = a;
        }
        if (absFrame + frames >= mPhaseMark) {
          // 4x the noise floor, but never hair-trigger and never deaf.
          mThreshold = std::min(0.5f, std::max(0.02f, mNoisePeak * 4.0f + 0.01f));
          startPing();
        }
        break;
      }

      case Phase::Emit: {
        if (mPingCursor == 0) mPingEmitFrame = absFrame;
        const int32_t n = std::min(frames, mPingFrames - mPingCursor);
        for (int32_t f = 0; f < n; ++f) {
          const int32_t i = mPingCursor + f;
          const float env =
              0.5f * (1.0f - std::cos(kTwoPi * static_cast<float>(i) /
                                      static_cast<float>(mPingFrames - 1)));
          const float s = std::sin(kTwoPi * kPingHz * static_cast<float>(i) /
                                   static_cast<float>(mSampleRate)) *
                          env * kPingAmplitude;
          float* o = out + static_cast<size_t>(f) * mChannels;
          for (int32_t c = 0; c < mChannels; ++c) o[c] = s;
        }
        mPingCursor += n;
        if (mPingCursor >= mPingFrames) {
          mPhase = Phase::Listen;
          mPhaseMark = mPingEmitFrame + mTimeoutFrames;
        }
        break;
      }

      case Phase::Listen: {
        int32_t hit = -1;
        for (int32_t f = 0; f < frames && hit < 0; ++f) {
          const float* s = in + static_cast<size_t>(f) * mChannels;
          for (int32_t c = 0; c < mChannels; ++c) {
            const float a = s[c] < 0.0f ? -s[c] : s[c];
            if (a >= mThreshold) {
              hit = f;
              break;
            }
          }
        }
        if (hit >= 0) {
          mDeltas[mPingIndex++] = static_cast<int32_t>(absFrame + hit - mPingEmitFrame);
          mPhase = Phase::Cooldown;
          mPhaseMark = absFrame + mCooldownFrames;
        } else if (absFrame + frames >= mPhaseMark) {
          mDeltas[mPingIndex++] = -1;  // this ping was never heard back
          mPhase = Phase::Cooldown;
          mPhaseMark = absFrame + mCooldownFrames;
        }
        break;
      }

      case Phase::Cooldown: {
        if (absFrame >= mPhaseMark) {
          if (mPingIndex >= kPings) {
            finish();
          } else {
            startPing();
          }
        }
        break;
      }
    }
  }

  // ----- Any thread -----
  State state() const {
    return static_cast<State>(mState.load(std::memory_order_relaxed));
  }
  // Last successful measurement (-1 until one succeeds). Survives later
  // failed runs so a bad re-calibration never destroys a good value.
  int32_t latencyFrames() const { return mLatencyFrames.load(std::memory_order_relaxed); }

 private:
  enum class Phase : uint8_t { LeadIn, Emit, Listen, Cooldown };

  static constexpr float kTwoPi = 6.283185307179586f;

  void startPing() {
    mPhase = Phase::Emit;
    mPingCursor = 0;
  }

  void finish() {
    int32_t valid[kPings];
    int32_t n = 0;
    for (int32_t i = 0; i < kPings; ++i) {
      if (mDeltas[i] >= 0) valid[n++] = mDeltas[i];
    }
    // Bounded insertion sort (n <= kPings): trivially realtime-safe.
    for (int32_t i = 1; i < n; ++i) {
      const int32_t v = valid[i];
      int32_t j = i - 1;
      while (j >= 0 && valid[j] > v) {
        valid[j + 1] = valid[j];
        --j;
      }
      valid[j + 1] = v;
    }

    // Success requires two independent measurements that agree within the
    // tolerance; the earliest tight pair wins (later hits are echoes).
    int32_t result = -1;
    for (int32_t i = 0; i + 1 < n; ++i) {
      if (valid[i + 1] - valid[i] <= mToleranceFrames) {
        result = (valid[i] + valid[i + 1]) / 2;
        break;
      }
    }

    mActive = false;
    if (result >= 0) {
      mLatencyFrames.store(result, std::memory_order_relaxed);
      mState.store(static_cast<int32_t>(State::Succeeded), std::memory_order_relaxed);
    } else {
      mState.store(static_cast<int32_t>(State::Failed), std::memory_order_relaxed);
    }
  }

  // Configuration
  int32_t mSampleRate = 48000;
  int32_t mChannels = 2;
  int32_t mPingFrames = 96;
  int32_t mLeadInFrames = 4800;
  int32_t mTimeoutFrames = 48000;
  int32_t mCooldownFrames = 12000;
  int32_t mToleranceFrames = 48;

  // Audio-thread state machine
  bool mActive = false;
  Phase mPhase = Phase::LeadIn;
  int64_t mPhaseMark = 0;
  int64_t mPingEmitFrame = 0;
  int32_t mPingCursor = 0;
  int32_t mPingIndex = 0;
  int32_t mDeltas[kPings] = {0};
  float mNoisePeak = 0.0f;
  float mThreshold = 0.02f;

  // Published to UI/control threads
  std::atomic<int32_t> mState{static_cast<int32_t>(State::Idle)};
  std::atomic<int32_t> mLatencyFrames{-1};
};

}  // namespace looper

#endif  // LOOPER_LATENCY_CALIBRATOR_H
