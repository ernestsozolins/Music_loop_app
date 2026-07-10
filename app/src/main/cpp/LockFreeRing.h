#ifndef LOOPER_LOCK_FREE_RING_H
#define LOOPER_LOCK_FREE_RING_H

/*
 * Lock-free single-producer / single-consumer primitives shared by the
 * audio engine (input ring, command queue, meter queue) and the disk
 * spooler (capture ring, per-stream playback rings).
 *
 * Both classes use monotonically increasing 64-bit positions masked into a
 * power-of-two buffer, so full/empty are never ambiguous, and cache-line
 * aligned indices so the two threads don't false-share.
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

namespace looper {

// ---------------------------------------------------------------------------
// SpscSampleRing — ring buffer of interleaved float frames.
// ---------------------------------------------------------------------------
class SpscSampleRing {
 public:
  // Control thread only, before producer/consumer run. Rounds the capacity
  // up to a power of two and pre-touches the memory so realtime threads
  // never fault a fresh page.
  void allocate(int32_t capacityFrames, int32_t channelCount) {
    int32_t cap = 1;
    while (cap < capacityFrames) cap <<= 1;
    mCapacityFrames = cap;
    mMask = cap - 1;
    mChannels = channelCount;
    mData.assign(static_cast<size_t>(cap) * channelCount, 0.0f);
    reset();
  }

  // Only safe while both producer and consumer are quiescent (see the
  // grace-window pattern in DiskSpooler.cpp).
  void reset() {
    mWritePos.store(0, std::memory_order_relaxed);
    mReadPos.store(0, std::memory_order_relaxed);
  }

  int32_t capacityFrames() const { return mCapacityFrames; }

  // Safe from any thread (value is approximate while the ring is active).
  int32_t framesReadable() const {
    const int32_t n = static_cast<int32_t>(mWritePos.load(std::memory_order_acquire) -
                                           mReadPos.load(std::memory_order_acquire));
    return n > 0 ? n : 0;
  }

  int32_t framesWritable() const { return mCapacityFrames - framesReadable(); }

  // PRODUCER only. Copies up to `frames` interleaved frames in; returns the
  // count actually written (the newest frames are dropped when full).
  int32_t writeFrames(const float* src, int32_t frames) {
    const uint64_t wr = mWritePos.load(std::memory_order_relaxed);
    const uint64_t rd = mReadPos.load(std::memory_order_acquire);
    const int32_t writable = mCapacityFrames - static_cast<int32_t>(wr - rd);
    const int32_t n = std::min(frames, writable);
    if (n <= 0) return 0;
    copyIn(wr, src, n);
    mWritePos.store(wr + n, std::memory_order_release);
    return n;
  }

  // CONSUMER only. Copies up to `frames` out; returns the count actually
  // read. Does not zero any shortfall — the caller pads.
  int32_t readFrames(float* dst, int32_t frames) {
    const uint64_t rd = mReadPos.load(std::memory_order_relaxed);
    const uint64_t wr = mWritePos.load(std::memory_order_acquire);
    const int32_t readable = static_cast<int32_t>(wr - rd);
    const int32_t n = std::min(frames, readable);
    if (n <= 0) return 0;
    copyOut(rd, dst, n);
    mReadPos.store(rd + n, std::memory_order_release);
    return n;
  }

  // CONSUMER only. Drops the OLDEST `frames` without copying (drift
  // catch-up). Returns the count actually discarded.
  int32_t discardFrames(int32_t frames) {
    const uint64_t rd = mReadPos.load(std::memory_order_relaxed);
    const uint64_t wr = mWritePos.load(std::memory_order_acquire);
    const int32_t n = std::min(frames, static_cast<int32_t>(wr - rd));
    if (n <= 0) return 0;
    mReadPos.store(rd + n, std::memory_order_release);
    return n;
  }

 private:
  void copyIn(uint64_t pos, const float* src, int32_t frames) {
    const int32_t start = static_cast<int32_t>(pos) & mMask;
    const int32_t first = std::min(frames, mCapacityFrames - start);
    std::memcpy(&mData[static_cast<size_t>(start) * mChannels], src,
                static_cast<size_t>(first) * mChannels * sizeof(float));
    if (frames > first) {
      std::memcpy(mData.data(), src + static_cast<size_t>(first) * mChannels,
                  static_cast<size_t>(frames - first) * mChannels * sizeof(float));
    }
  }

  void copyOut(uint64_t pos, float* dst, int32_t frames) const {
    const int32_t start = static_cast<int32_t>(pos) & mMask;
    const int32_t first = std::min(frames, mCapacityFrames - start);
    std::memcpy(dst, &mData[static_cast<size_t>(start) * mChannels],
                static_cast<size_t>(first) * mChannels * sizeof(float));
    if (frames > first) {
      std::memcpy(dst + static_cast<size_t>(first) * mChannels, mData.data(),
                  static_cast<size_t>(frames - first) * mChannels * sizeof(float));
    }
  }

  std::vector<float> mData;
  int32_t mCapacityFrames = 0;
  int32_t mMask = 0;
  int32_t mChannels = 1;

  alignas(64) std::atomic<uint64_t> mWritePos{0};
  alignas(64) std::atomic<uint64_t> mReadPos{0};
};

// ---------------------------------------------------------------------------
// SpscQueue — lock-free SPSC queue of trivially copyable structs.
// ---------------------------------------------------------------------------
template <typename T, uint32_t kCapacity>
class SpscQueue {
  static_assert((kCapacity & (kCapacity - 1)) == 0, "capacity must be a power of two");

 public:
  // PRODUCER only. Returns false (drops the item) when full.
  bool push(const T& item) {
    const uint64_t wr = mWritePos.load(std::memory_order_relaxed);
    const uint64_t rd = mReadPos.load(std::memory_order_acquire);
    if (wr - rd >= kCapacity) return false;
    mSlots[wr & (kCapacity - 1)] = item;
    mWritePos.store(wr + 1, std::memory_order_release);
    return true;
  }

  // CONSUMER only. Returns false when empty.
  bool pop(T& out) {
    const uint64_t rd = mReadPos.load(std::memory_order_relaxed);
    const uint64_t wr = mWritePos.load(std::memory_order_acquire);
    if (rd == wr) return false;
    out = mSlots[rd & (kCapacity - 1)];
    mReadPos.store(rd + 1, std::memory_order_release);
    return true;
  }

  uint32_t size() const {
    return static_cast<uint32_t>(mWritePos.load(std::memory_order_acquire) -
                                 mReadPos.load(std::memory_order_acquire));
  }

 private:
  std::array<T, kCapacity> mSlots{};
  alignas(64) std::atomic<uint64_t> mWritePos{0};
  alignas(64) std::atomic<uint64_t> mReadPos{0};
};

}  // namespace looper

#endif  // LOOPER_LOCK_FREE_RING_H
