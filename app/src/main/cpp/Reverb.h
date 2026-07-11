#ifndef LOOPER_REVERB_H
#define LOOPER_REVERB_H

/*
 * Reverb — lightweight Freeverb-style Schroeder reverberator.
 *
 * Topology (the classic Freeverb tank, per channel):
 *   input -> 8 parallel feedback-comb filters (one-pole lowpass damping in
 *   the feedback path) -> 4 series allpass diffusers. The right channel's
 *   delay lines are detuned by a fixed stereo spread so the tail
 *   decorrelates. No convolution, no third-party code; delay lengths are
 *   the canonical 44.1 kHz tunings scaled to the engine rate.
 *
 * ROUTING (see AudioEngine.cpp): processed IN PLACE on the master output
 * mix only — the audio sent to headphones/speakers. Everything recordable
 * is tapped upstream of this call (input-capture tee, mix-capture tee,
 * overdub path, stem export), so recordings and exported stems stay 100%
 * dry by construction.
 *
 * Realtime rules: configure() pre-allocates and pre-touches every delay
 * line on the control thread; process() performs no allocation, locking,
 * or I/O. Parameters arrive through relaxed atomics; the dry/wet mix is
 * ramped across each block so slider moves never click. Denormal tails are
 * flushed manually so the combs can't trigger subnormal-math stalls.
 */

#include <atomic>
#include <cstdint>
#include <vector>

namespace looper {

class Reverb {
 public:
  // Control thread, before the streams start.
  void configure(int32_t sampleRate, int32_t channelCount);

  // Control/UI thread: lock-free, safe mid-playback.
  void setRoomSize(float size);    // 0..1 (tail length)
  void setDamping(float damping);  // 0..1 (high-frequency absorption)
  void setMix(float mix);          // 0 = fully dry .. 1 = fully wet
  void setEnabled(bool enabled);   // hard bypass (tank cleared on re-enable)

  float roomSize() const { return mRoomSize.load(std::memory_order_relaxed); }
  float damping() const { return mDamping.load(std::memory_order_relaxed); }
  float mix() const { return mMix.load(std::memory_order_relaxed); }
  bool enabled() const { return mEnabled.load(std::memory_order_relaxed); }

  // AUDIO THREAD ONLY. In-place on the interleaved output mix.
  void process(float* inout, int32_t frames);

 private:
  static constexpr int32_t kNumCombs = 8;
  static constexpr int32_t kNumAllpasses = 4;
  static constexpr float kFixedGain = 0.015f;  // tank input attenuation
  static constexpr float kWetScale = 3.0f;     // restores tank level at mix = 1
  static constexpr float kScaleRoom = 0.28f;   // feedback = kOffsetRoom + size * kScaleRoom
  static constexpr float kOffsetRoom = 0.7f;
  static constexpr float kScaleDamp = 0.4f;

  struct Comb {
    std::vector<float> buffer;
    int32_t index = 0;
    float filterStore = 0.0f;

    float process(float input, float feedback, float damp) {
      const float output = buffer[static_cast<size_t>(index)];
      filterStore = output * (1.0f - damp) + filterStore * damp;
      // Flush denormals: a decaying tail otherwise ends in subnormal
      // arithmetic, which is catastrophically slow on some cores.
      if (filterStore < 1.0e-15f && filterStore > -1.0e-15f) filterStore = 0.0f;
      buffer[static_cast<size_t>(index)] = input + filterStore * feedback;
      if (++index >= static_cast<int32_t>(buffer.size())) index = 0;
      return output;
    }
  };

  struct Allpass {
    std::vector<float> buffer;
    int32_t index = 0;

    float process(float input) {
      const float delayed = buffer[static_cast<size_t>(index)];
      buffer[static_cast<size_t>(index)] = input + delayed * 0.5f;
      if (++index >= static_cast<int32_t>(buffer.size())) index = 0;
      return delayed - input;
    }
  };

  void clearBuffers();  // audio thread (bypass edge) or pre-start

  int32_t mChannels = 2;
  Comb mCombs[2][kNumCombs];
  Allpass mAllpasses[2][kNumAllpasses];

  // Control -> audio parameters.
  std::atomic<float> mRoomSize{0.5f};
  std::atomic<float> mDamping{0.5f};
  std::atomic<float> mMix{0.0f};  // dry by default: opt-in sweetening
  std::atomic<bool> mEnabled{true};

  // Audio-thread state.
  float mCurrentMix = 0.0f;  // block-ramped toward mMix (click-free)
  bool mWasEnabled = false;
};

}  // namespace looper

#endif  // LOOPER_REVERB_H
