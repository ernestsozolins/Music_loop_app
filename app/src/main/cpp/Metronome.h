#ifndef LOOPER_METRONOME_H
#define LOOPER_METRONOME_H

/*
 * Metronome — sample-accurate algorithmic click generator.
 *
 * Synthesis: a sine burst with an exponential decay envelope, computed
 * inside the audio callback. No sample assets, no disk I/O. The oscillator
 * starts at phase 0 (sin(0) == 0) so the onset is inherently click-free,
 * and the envelope decays to the silence floor in kTickSeconds.
 *
 * Tone: downbeat (beat 1 of the bar) at kDownbeatHz, offbeats at kOffbeatHz.
 *
 * Routing: render() ADDS into the output mix buffer only. It is called
 * after the record/overdub path has already consumed the input scratch, and
 * it never touches the input ring, the input scratch, or the loop tracks —
 * the click reaches headphones/speakers but can never be recorded.
 *
 * Tempo control: the UI writes {active, bpm, beatsPerMeasure} packed into a
 * single 64-bit atomic (lock-free on arm64/x86_64). The audio thread reads
 * it once per block and applies changes at the NEXT BEAT BOUNDARY: the
 * countdown to the already-scheduled beat is never disturbed, so a mid-bar
 * BPM edit produces no phase glitch — the current inter-beat interval
 * completes at the old tempo and the new period takes over from the next
 * tick. A time-signature change re-wraps the beat counter at the next tick,
 * and re-activating the metronome restarts the bar on an immediate downbeat.
 *
 * Beat spacing stays sample-accurate over time because the countdown is a
 * double accumulator (`countdown += period`) that carries fractional error
 * instead of rounding it away each beat.
 *
 * Realtime rules: render() performs no allocation, locking, I/O, or JNI.
 */

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace looper {

class Metronome {
 public:
  static constexpr float kDownbeatHz = 1500.0f;
  static constexpr float kOffbeatHz = 800.0f;
  static constexpr float kTickSeconds = 0.045f;    // envelope reaches the floor here
  static constexpr float kSilenceFloor = 1.0e-4f;  // ~ -80 dBFS
  static constexpr float kMinBpm = 20.0f;
  static constexpr float kMaxBpm = 400.0f;
  static constexpr int32_t kMaxBeatsPerMeasure = 16;

  // Control thread, before the streams start.
  void configure(int32_t sampleRate, int32_t channelCount) {
    mSampleRate = sampleRate;
    mChannels = channelCount;
    // Per-sample coefficient of an exponential decay that hits the silence
    // floor after exactly kTickSeconds.
    mDecay = std::exp(std::log(kSilenceFloor) / (kTickSeconds * static_cast<float>(sampleRate)));
  }

  // Control thread (any time, including mid-bar). One relaxed atomic store;
  // the audio thread folds the change in at the next beat boundary.
  void setState(bool active, float bpm, int32_t beatsPerMeasure) {
    mControl.store(pack(active, bpm, beatsPerMeasure), std::memory_order_relaxed);
  }

  void setGain(float gain) {
    mGain.store(gain < 0.0f ? 0.0f : (gain > 2.0f ? 2.0f : gain), std::memory_order_relaxed);
  }

  bool isActive() const { return (mControl.load(std::memory_order_relaxed) & 1u) != 0; }
  // Monotonic tick counter + position in bar, for the UI beat flash.
  uint32_t beatCount() const { return mBeatCountAtomic.load(std::memory_order_relaxed); }
  int32_t beatInBar() const { return mBeatInBarAtomic.load(std::memory_order_relaxed); }

  // AUDIO THREAD ONLY. Mixes the click into `out` (interleaved,
  // channelCount channels, `frames` frames). Output-path exclusive — see
  // the routing note in the file header.
  //
  // Free-running when loopLen <= 0. When the engine passes the loop playhead
  // (loopPos = position at the first frame of this block, loopLen = loop
  // length in frames), the metronome is PHASE-LOCKED to the loop: the loop
  // start is bar 1 beat 1, and the beat grid re-anchors at every wrap, so
  // the click can never drift against the recorded material. When the loop
  // length is not an exact multiple of the beat period the seam interval is
  // truncated — the wrap downbeat always wins. Switching between the two
  // modes mid-flight is glitch-free: the grid simply re-anchors at the next
  // wrap (locking) or keeps its current phase (unlocking).
  void render(float* out, int32_t frames, int32_t loopPos = -1, int32_t loopLen = 0) {
    const uint64_t ctrl = mControl.load(std::memory_order_relaxed);
    const bool active = (ctrl & 1u) != 0;
    const bool locked = active && loopLen > 0 && loopPos >= 0;

    if (active && !mWasActive) {
      // (Re)armed: the bar restarts and the downbeat fires immediately.
      mCountdown = 0.0;
      mBeatInBar = -1;
    }
    mWasActive = active;

    // When inactive, stop scheduling ticks but let a sounding envelope decay
    // out naturally instead of truncating it.
    if (!active && mEnv <= kSilenceFloor) return;

    const float gain = mGain.load(std::memory_order_relaxed);
    for (int32_t f = 0; f < frames; ++f) {
      if (active) {
        if (locked &&
            static_cast<int32_t>((static_cast<int64_t>(loopPos) + f) % loopLen) == 0) {
          // Loop wrapped exactly here: force bar 1 beat 1 on this frame.
          mCountdown = 0.0;
          mBeatInBar = -1;
        }
        if (mCountdown <= 0.0) trigger(ctrl);
        mCountdown -= 1.0;
      }
      if (mEnv > kSilenceFloor) {
        const float s = std::sin(mPhase) * mEnv * gain;
        mPhase += mPhaseInc;
        if (mPhase > kTwoPi) mPhase -= kTwoPi;
        mEnv *= mDecay;
        float* o = out + static_cast<size_t>(f) * mChannels;
        for (int32_t c = 0; c < mChannels; ++c) o[c] += s;
      }
    }
  }

 private:
  static constexpr float kTwoPi = 6.283185307179586f;

  // Control word layout: [63..32] bpm (float bits), [15..8] beatsPerMeasure,
  // [0] active.
  static uint64_t pack(bool active, float bpm, int32_t beats) {
    uint32_t bpmBits = 0;
    std::memcpy(&bpmBits, &bpm, sizeof(bpmBits));
    return (static_cast<uint64_t>(bpmBits) << 32) |
           (static_cast<uint64_t>(static_cast<uint32_t>(beats) & 0xFFu) << 8) |
           (active ? 1u : 0u);
  }

  void trigger(uint64_t ctrl) {
    // Unpack and sanitize on the audio side — never trust the control word.
    uint32_t bpmBits = static_cast<uint32_t>(ctrl >> 32);
    float bpm = 0.0f;
    std::memcpy(&bpm, &bpmBits, sizeof(bpm));
    if (!(bpm >= kMinBpm && bpm <= kMaxBpm)) bpm = 120.0f;  // also rejects NaN
    int32_t beats = static_cast<int32_t>((ctrl >> 8) & 0xFFu);
    if (beats < 1) beats = 1;
    if (beats > kMaxBeatsPerMeasure) beats = kMaxBeatsPerMeasure;

    // The new period takes effect from this boundary onward. `+=` (not `=`)
    // carries the fractional remainder for long-term sample accuracy.
    mCountdown += static_cast<double>(mSampleRate) * 60.0 / static_cast<double>(bpm);
    mBeatInBar = (mBeatInBar + 1) % beats;

    const float freq = (mBeatInBar == 0) ? kDownbeatHz : kOffbeatHz;
    mPhase = 0.0f;  // sine from zero: no onset discontinuity
    mPhaseInc = kTwoPi * freq / static_cast<float>(mSampleRate);
    mEnv = 1.0f;

    mBeatCountAtomic.fetch_add(1, std::memory_order_relaxed);
    mBeatInBarAtomic.store(mBeatInBar, std::memory_order_relaxed);
  }

  // Configuration (set before streams start)
  int32_t mSampleRate = 48000;
  int32_t mChannels = 2;
  float mDecay = 0.9957f;

  // Control plane -> audio plane
  std::atomic<uint64_t> mControl{0};
  std::atomic<float> mGain{0.8f};

  // Audio-thread voice state
  double mCountdown = 0.0;
  int32_t mBeatInBar = -1;
  bool mWasActive = false;
  float mPhase = 0.0f;
  float mPhaseInc = 0.0f;
  float mEnv = 0.0f;

  // Audio plane -> UI
  std::atomic<uint32_t> mBeatCountAtomic{0};
  std::atomic<int32_t> mBeatInBarAtomic{0};
};

}  // namespace looper

#endif  // LOOPER_METRONOME_H
