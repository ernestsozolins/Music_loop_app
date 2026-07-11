#ifndef LOOPER_LIMITER_H
#define LOOPER_LIMITER_H

/*
 * Limiter — feed-forward look-ahead peak limiter for the MONITORING mix.
 *
 * The signal is delayed by a 5 ms look-ahead line while a peak-follower
 * envelope tracks the *incoming* (future) samples: the envelope jumps to any
 * new peak instantly and decays slowly (~200 ms), so the reciprocal gain is
 * already reduced by the time that same peak reaches the output. Loud stacked
 * loops therefore compress transparently instead of hard-clipping. Because
 * the envelope only decays a fraction of a percent across the look-ahead
 * window, output peaks land at the ceiling with sub-0.1 dB overshoot, which
 * the engine's final safety clamp absorbs.
 *
 * Placement (AudioEngine.cpp): after the metronome, on the output mix only.
 * Both capture tees and the stem/overdub paths are upstream, so recordings
 * remain unprocessed. The 5 ms of added output latency is measured by the
 * calibration ping automatically (the ping passes through this limiter).
 *
 * Realtime rules: configure()/reset() on the control thread while streams
 * are stopped; process() is allocation/lock/IO-free.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace looper {

class Limiter {
 public:
  void configure(int32_t sampleRate, int32_t channelCount) {
    mChannels = channelCount;
    mDelayFrames = std::max(1, sampleRate / 200);  // 5 ms look-ahead
    mDelay.assign(static_cast<size_t>(mDelayFrames) * channelCount, 0.0f);
    // Per-sample envelope decay for a ~200 ms release. Chosen so the envelope
    // drops well under 1% across the 5 ms look-ahead window, which bounds the
    // output overshoot.
    mReleaseCoef = std::exp(-1.0f / (0.200f * static_cast<float>(sampleRate)));
    reset();
  }

  // Only while both streams are stopped.
  void reset() {
    std::fill(mDelay.begin(), mDelay.end(), 0.0f);
    mIndex = 0;
    mEnv = kCeiling;
  }

  int32_t latencyFrames() const { return mDelayFrames; }

  // AUDIO THREAD ONLY. In place on the interleaved output mix.
  void process(float* inout, int32_t frames) {
    const int32_t ch = mChannels;
    for (int32_t f = 0; f < frames; ++f) {
      float* s = inout + static_cast<size_t>(f) * ch;
      float peak = 0.0f;
      for (int32_t c = 0; c < ch; ++c) {
        const float a = s[c] < 0.0f ? -s[c] : s[c];
        if (a > peak) peak = a;
      }
      // Peak follower: instant attack, slow release. Driven by the FUTURE
      // (undelayed) sample, so the gain is already down when the peak exits
      // the delay line.
      mEnv *= mReleaseCoef;
      if (peak > mEnv) mEnv = peak;
      const float gain = mEnv > kCeiling ? kCeiling / mEnv : 1.0f;
      float* d = mDelay.data() + static_cast<size_t>(mIndex) * ch;
      for (int32_t c = 0; c < ch; ++c) {
        const float delayed = d[c];
        d[c] = s[c];
        s[c] = delayed * gain;
      }
      if (++mIndex >= mDelayFrames) mIndex = 0;
    }
  }

 private:
  static constexpr float kCeiling = 0.98f;

  int32_t mChannels = 2;
  int32_t mDelayFrames = 240;
  std::vector<float> mDelay;
  int32_t mIndex = 0;
  float mEnv = kCeiling;
  float mReleaseCoef = 0.9999f;
};

}  // namespace looper

#endif  // LOOPER_LIMITER_H
