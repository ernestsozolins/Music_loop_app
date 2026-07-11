#include "Reverb.h"

#include <algorithm>

namespace looper {

namespace {

// Canonical Freeverb tunings, in samples at 44100 Hz.
constexpr int32_t kCombTunings[] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
constexpr int32_t kAllpassTunings[] = {556, 441, 341, 225};
constexpr int32_t kStereoSpread = 23;  // right-channel detune, samples @44.1k

int32_t scaledLength(int32_t samplesAt44k, int32_t sampleRate) {
  const int64_t scaled = (static_cast<int64_t>(samplesAt44k) * sampleRate + 22050) / 44100;
  return std::max<int32_t>(1, static_cast<int32_t>(scaled));
}

float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

}  // namespace

void Reverb::configure(int32_t sampleRate, int32_t channelCount) {
  mChannels = channelCount;
  const int32_t spread = scaledLength(kStereoSpread, sampleRate);
  for (int32_t ch = 0; ch < 2; ++ch) {
    const int32_t detune = (ch == 1) ? spread : 0;
    for (int32_t i = 0; i < kNumCombs; ++i) {
      mCombs[ch][i].buffer.assign(
          static_cast<size_t>(scaledLength(kCombTunings[i], sampleRate) + detune), 0.0f);
      mCombs[ch][i].index = 0;
      mCombs[ch][i].filterStore = 0.0f;
    }
    for (int32_t i = 0; i < kNumAllpasses; ++i) {
      mAllpasses[ch][i].buffer.assign(
          static_cast<size_t>(scaledLength(kAllpassTunings[i], sampleRate) + detune), 0.0f);
      mAllpasses[ch][i].index = 0;
    }
  }
  mCurrentMix = mMix.load(std::memory_order_relaxed);
  mWasEnabled = false;
}

void Reverb::setRoomSize(float size) {
  mRoomSize.store(clamp01(size), std::memory_order_relaxed);
}

void Reverb::setDamping(float damping) {
  mDamping.store(clamp01(damping), std::memory_order_relaxed);
}

void Reverb::setMix(float mix) { mMix.store(clamp01(mix), std::memory_order_relaxed); }

void Reverb::setEnabled(bool enabled) {
  mEnabled.store(enabled, std::memory_order_relaxed);
}

void Reverb::clearBuffers() {
  // Total tank memory is ~100 KB of already-resident floats; zeroing it on
  // the audio thread at a bypass edge is a bounded ~microseconds memset.
  for (int32_t ch = 0; ch < 2; ++ch) {
    for (Comb& comb : mCombs[ch]) {
      std::fill(comb.buffer.begin(), comb.buffer.end(), 0.0f);
      comb.filterStore = 0.0f;
      comb.index = 0;
    }
    for (Allpass& allpass : mAllpasses[ch]) {
      std::fill(allpass.buffer.begin(), allpass.buffer.end(), 0.0f);
      allpass.index = 0;
    }
  }
}

void Reverb::process(float* inout, int32_t frames) {
  if (frames <= 0) return;
  if (!mEnabled.load(std::memory_order_relaxed)) {
    mWasEnabled = false;  // tank is stale from here on
    return;               // hard bypass: output untouched
  }
  if (!mWasEnabled) {
    // Re-enabled: drop the frozen stale tail instead of replaying it.
    clearBuffers();
    mCurrentMix = mMix.load(std::memory_order_relaxed);
    mWasEnabled = true;
  }

  const float room = clamp01(mRoomSize.load(std::memory_order_relaxed));
  const float dampAmount = clamp01(mDamping.load(std::memory_order_relaxed));
  const float targetMix = clamp01(mMix.load(std::memory_order_relaxed));
  const float feedback = kOffsetRoom + room * kScaleRoom;
  const float damp = dampAmount * kScaleDamp;
  // Linear ramp across the block: slider moves never step the gain.
  const float mixStep = (targetMix - mCurrentMix) / static_cast<float>(frames);

  if (mChannels == 2) {
    for (int32_t f = 0; f < frames; ++f) {
      mCurrentMix += mixStep;
      const float dryGain = 1.0f - mCurrentMix;
      const float wetGain = mCurrentMix * kWetScale;
      float* s = inout + static_cast<size_t>(f) * 2;
      const float inL = s[0];
      const float inR = s[1];
      // Freeverb feeds a mono sum into both (detuned) tanks.
      const float tankIn = (inL + inR) * kFixedGain;
      float wetL = 0.0f;
      float wetR = 0.0f;
      for (int32_t c = 0; c < kNumCombs; ++c) {
        wetL += mCombs[0][c].process(tankIn, feedback, damp);
        wetR += mCombs[1][c].process(tankIn, feedback, damp);
      }
      for (int32_t a = 0; a < kNumAllpasses; ++a) {
        wetL = mAllpasses[0][a].process(wetL);
        wetR = mAllpasses[1][a].process(wetR);
      }
      s[0] = inL * dryGain + wetL * wetGain;
      s[1] = inR * dryGain + wetR * wetGain;
    }
  } else {
    for (int32_t f = 0; f < frames; ++f) {
      mCurrentMix += mixStep;
      const float dryGain = 1.0f - mCurrentMix;
      const float wetGain = mCurrentMix * kWetScale;
      const float in = inout[f];
      const float tankIn = in * 2.0f * kFixedGain;
      float wet = 0.0f;
      for (int32_t c = 0; c < kNumCombs; ++c) {
        wet += mCombs[0][c].process(tankIn, feedback, damp);
      }
      for (int32_t a = 0; a < kNumAllpasses; ++a) {
        wet = mAllpasses[0][a].process(wet);
      }
      inout[f] = in * dryGain + wet * wetGain;
    }
  }
  mCurrentMix = targetMix;  // land exactly; no float-drift accumulation
}

}  // namespace looper
