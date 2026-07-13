#ifndef LOOPER_DRONE_H
#define LOOPER_DRONE_H

/*
 * Drone — a sustained reference tone for tuning and for improvising over.
 *
 * A solo cellist tunes the open strings to a pitch and often loops/practises
 * over a tonic drone. This generates a warm sustained tone (fundamental plus
 * a couple of quiet harmonics and a slow vibrato so it reads as a bowed note
 * rather than a test sine) at a selectable frequency.
 *
 * ROUTING (see AudioEngine.cpp): mixed into the OUTPUT only, after the capture
 * tees and the loop/overdub path — like the metronome it reaches the
 * headphones/speakers but is never recorded into a loop or a stem.
 *
 * Realtime rules: configure() runs before the streams start; process() never
 * allocates or locks. Parameters are relaxed atomics; the level is ramped per
 * block so toggling it and changing pitch never clicks.
 */

#include <atomic>
#include <cmath>
#include <cstdint>

namespace looper {

class Drone {
 public:
  void configure(int32_t sampleRate, int32_t channelCount) {
    mSampleRate = sampleRate > 0 ? sampleRate : 48000;
    mChannels = channelCount > 0 ? channelCount : 2;
    mPhase = 0.0f;
    mVibPhase = 0.0f;
    mLevel = 0.0f;
  }

  void setFrequency(float hz) {
    mFrequency.store(hz < 20.0f ? 20.0f : (hz > 2000.0f ? 2000.0f : hz),
                     std::memory_order_relaxed);
  }
  void setGain(float g) {
    mGain.store(g < 0.0f ? 0.0f : (g > 1.0f ? 1.0f : g), std::memory_order_relaxed);
  }
  void setEnabled(bool on) { mEnabled.store(on, std::memory_order_relaxed); }

  float frequency() const { return mFrequency.load(std::memory_order_relaxed); }
  float gain() const { return mGain.load(std::memory_order_relaxed); }
  bool enabled() const { return mEnabled.load(std::memory_order_relaxed); }

  // AUDIO THREAD ONLY. Adds the drone into the interleaved output mix.
  void process(float* out, int32_t frames) {
    const bool on = mEnabled.load(std::memory_order_relaxed);
    const float targetLevel = on ? mGain.load(std::memory_order_relaxed) : 0.0f;
    if (!on && mLevel <= 0.00001f) return;  // settled + off: nothing to do

    const float freq = mFrequency.load(std::memory_order_relaxed);
    const float inc = kTwoPi * freq / static_cast<float>(mSampleRate);
    const float vibInc = kTwoPi * 5.0f / static_cast<float>(mSampleRate);  // 5 Hz vibrato
    const float levelStep = (targetLevel - mLevel) / static_cast<float>(frames);

    for (int32_t f = 0; f < frames; ++f) {
      mLevel += levelStep;
      const float vib = 1.0f + 0.003f * std::sin(mVibPhase);  // ±0.3% pitch
      mPhase += inc * vib;
      if (mPhase > kTwoPi) mPhase -= kTwoPi;
      mVibPhase += vibInc;
      if (mVibPhase > kTwoPi) mVibPhase -= kTwoPi;
      // Fundamental + quiet 2nd/3rd harmonics for a warmer, more audible tone.
      float s = std::sin(mPhase) + 0.3f * std::sin(2.0f * mPhase) +
                0.12f * std::sin(3.0f * mPhase);
      s *= 0.28f * mLevel;  // headroom so stacked harmonics never clip
      float* o = out + static_cast<size_t>(f) * mChannels;
      for (int32_t c = 0; c < mChannels; ++c) o[c] += s;
    }
  }

 private:
  static constexpr float kTwoPi = 6.283185307179586f;

  int32_t mSampleRate = 48000;
  int32_t mChannels = 2;
  float mPhase = 0.0f;
  float mVibPhase = 0.0f;
  float mLevel = 0.0f;

  std::atomic<float> mFrequency{220.0f};  // A3 — the cello tuning reference
  std::atomic<float> mGain{0.5f};
  std::atomic<bool> mEnabled{false};
};

}  // namespace looper

#endif  // LOOPER_DRONE_H
