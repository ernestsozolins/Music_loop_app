#ifndef LOOPER_FILTER_H
#define LOOPER_FILTER_H

/*
 * Filter — a stereo multimode resonant filter for the monitor/output path.
 *
 * Topology: Andy Simper's topology-preserving-transform (TPT) state-variable
 * filter — unconditionally stable, cheap, and continuously tunable, which is
 * exactly what a looper filter-sweep FX needs. Low-pass / high-pass /
 * band-pass are all tapped from the same core.
 *
 * ROUTING (see AudioEngine.cpp): processed IN PLACE on the master output mix
 * only. Like the reverb, everything recordable is tapped upstream, so takes
 * and exported stems stay dry — this is a performance/monitor FX.
 *
 * Realtime rules: configure() runs on the control thread before the streams
 * start; process() allocates nothing and never locks. Parameters arrive via
 * relaxed atomics. The cutoff and the dry/wet (bypass) amount are both ramped
 * once per block so sweeps and on/off never click or zipper.
 */

#include <atomic>
#include <cmath>
#include <cstdint>

namespace looper {

class Filter {
 public:
  enum class Mode : int32_t { LowPass = 0, HighPass = 1, BandPass = 2 };

  void configure(int32_t sampleRate, int32_t channelCount) {
    mSampleRate = sampleRate > 0 ? sampleRate : 48000;
    mChannels = channelCount > 0 ? channelCount : 2;
    for (int c = 0; c < kMaxCh; ++c) {
      mIc1[c] = 0.0f;
      mIc2[c] = 0.0f;
    }
    mCutoffSmoothed = mCutoff.load(std::memory_order_relaxed);
    mWet = mEnabled.load(std::memory_order_relaxed) ? 1.0f : 0.0f;
  }

  // Control/UI thread — lock-free, safe mid-playback.
  void setCutoff(float hz) {
    const float lo = 30.0f;
    const float hi = static_cast<float>(mSampleRate) * 0.45f;
    mCutoff.store(hz < lo ? lo : (hz > hi ? hi : hz), std::memory_order_relaxed);
  }
  void setResonance(float r) {  // 0..1 -> Q from ~0.7 to ~8
    mResonance.store(r < 0.0f ? 0.0f : (r > 1.0f ? 1.0f : r), std::memory_order_relaxed);
  }
  void setMode(Mode m) { mMode.store(static_cast<int32_t>(m), std::memory_order_relaxed); }
  void setEnabled(bool on) { mEnabled.store(on, std::memory_order_relaxed); }

  float cutoff() const { return mCutoff.load(std::memory_order_relaxed); }
  float resonance() const { return mResonance.load(std::memory_order_relaxed); }
  int32_t mode() const { return mMode.load(std::memory_order_relaxed); }
  bool enabled() const { return mEnabled.load(std::memory_order_relaxed); }

  // AUDIO THREAD ONLY. In-place on the interleaved output mix.
  void process(float* inout, int32_t frames) {
    const bool on = mEnabled.load(std::memory_order_relaxed);
    const float wetTarget = on ? 1.0f : 0.0f;
    // Nothing to do once fully bypassed and settled.
    if (!on && mWet <= 0.0001f) return;

    const int32_t ch = mChannels > kMaxCh ? kMaxCh : mChannels;
    const float target = mCutoff.load(std::memory_order_relaxed);
    const float res = mResonance.load(std::memory_order_relaxed);
    const int32_t mode = mMode.load(std::memory_order_relaxed);
    // Q from 0.707 (no resonance) up to ~8; k = 1/Q is the SVF damping.
    const float q = 0.707f + res * 7.3f;
    const float k = 1.0f / q;

    // Per-block ramps (≈ one-pole toward the target) kill zipper/click.
    const float cutoffStep = (target - mCutoffSmoothed) / static_cast<float>(frames);
    const float wetStep = (wetTarget - mWet) / static_cast<float>(frames);

    for (int32_t f = 0; f < frames; ++f) {
      mCutoffSmoothed += cutoffStep;
      mWet += wetStep;
      const float g = std::tan(3.14159265f * mCutoffSmoothed / static_cast<float>(mSampleRate));
      const float a1 = 1.0f / (1.0f + g * (g + k));
      const float a2 = g * a1;
      const float a3 = g * a2;
      float* frame = inout + static_cast<size_t>(f) * mChannels;
      for (int32_t c = 0; c < ch; ++c) {
        const float v0 = frame[c];
        const float v3 = v0 - mIc2[c];
        const float v1 = a1 * mIc1[c] + a2 * v3;
        const float v2 = mIc2[c] + a2 * mIc1[c] + a3 * v3;
        mIc1[c] = 2.0f * v1 - mIc1[c];
        mIc2[c] = 2.0f * v2 - mIc2[c];
        float wet;
        switch (mode) {
          case 1:  wet = v0 - k * v1 - v2; break;  // high-pass
          case 2:  wet = v1; break;                // band-pass
          default: wet = v2; break;                // low-pass
        }
        frame[c] = v0 + (wet - v0) * mWet;  // dry/wet crossfade for click-free bypass
      }
    }
  }

 private:
  static constexpr int32_t kMaxCh = 2;

  int32_t mSampleRate = 48000;
  int32_t mChannels = 2;
  float mIc1[kMaxCh] = {0.0f, 0.0f};  // SVF integrator states
  float mIc2[kMaxCh] = {0.0f, 0.0f};
  float mCutoffSmoothed = 1200.0f;
  float mWet = 0.0f;

  std::atomic<float> mCutoff{1200.0f};
  std::atomic<float> mResonance{0.2f};
  std::atomic<int32_t> mMode{static_cast<int32_t>(Mode::LowPass)};
  std::atomic<bool> mEnabled{false};
};

}  // namespace looper

#endif  // LOOPER_FILTER_H
