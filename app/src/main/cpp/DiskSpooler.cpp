#include "DiskSpooler.h"

#include <chrono>
#include <cstdio>

#include "Log.h"

namespace looper {

namespace {

constexpr int32_t kCaptureRingFrames = 1 << 17;  // ~2.7 s @48k: covers flash-write stalls
constexpr int32_t kStreamRingFrames = 1 << 16;   // ~1.4 s of read-ahead per stream
constexpr int32_t kWriterChunkFrames = 16384;    // fwrite granularity
constexpr int32_t kStreamChunkFrames = 8192;     // fread granularity
constexpr auto kWorkerTick = std::chrono::milliseconds(5);

// Longer than any audio callback (<= ~10 ms). After clearing an audio-side
// gate, sleeping this long guarantees no callback is still inside the ring,
// making a reset() race-free.
void graceSleep() { std::this_thread::sleep_for(std::chrono::milliseconds(20)); }

void putU16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}

void putU32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
  p[2] = static_cast<uint8_t>(v >> 16);
  p[3] = static_cast<uint8_t>(v >> 24);
}

uint16_t getU16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

uint32_t getU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// ---------------------------------------------------------------------------
// WavWriter — RIFF / WAVE_FORMAT_IEEE_FLOAT (32-bit, the engine's native
// format). Header is written with zero sizes and patched on close. Owned and
// used exclusively by the DiskWriter thread.
// ---------------------------------------------------------------------------
class WavWriter {
 public:
  ~WavWriter() { close(); }

  bool isOpen() const { return mFile != nullptr; }
  int64_t frames() const { return mFrames; }

  bool open(const std::string& path, int32_t sampleRate, int32_t channels) {
    close();
    mFile = std::fopen(path.c_str(), "wb");
    if (mFile == nullptr) return false;
    mChannels = channels;
    mFrames = 0;

    uint8_t h[kHeaderBytes] = {0};
    std::memcpy(h + 0, "RIFF", 4);
    putU32(h + 4, 0);  // RIFF size: patched on close
    std::memcpy(h + 8, "WAVE", 4);
    std::memcpy(h + 12, "fmt ", 4);
    putU32(h + 16, 18);
    putU16(h + 20, 3);  // WAVE_FORMAT_IEEE_FLOAT
    putU16(h + 22, static_cast<uint16_t>(channels));
    putU32(h + 24, static_cast<uint32_t>(sampleRate));
    putU32(h + 28, static_cast<uint32_t>(sampleRate * channels * 4));
    putU16(h + 32, static_cast<uint16_t>(channels * 4));
    putU16(h + 34, 32);
    putU16(h + 36, 0);  // cbSize
    std::memcpy(h + 38, "fact", 4);
    putU32(h + 42, 4);
    putU32(h + 46, 0);  // dwSampleLength: patched on close
    std::memcpy(h + 50, "data", 4);
    putU32(h + 54, 0);  // data size: patched on close

    if (std::fwrite(h, 1, kHeaderBytes, mFile) != kHeaderBytes) {
      std::fclose(mFile);
      mFile = nullptr;
      return false;
    }
    return true;
  }

  int32_t writeFrames(const float* interleaved, int32_t n) {
    if (mFile == nullptr || n <= 0) return 0;
    const size_t written =
        std::fwrite(interleaved, static_cast<size_t>(mChannels) * sizeof(float),
                    static_cast<size_t>(n), mFile);
    mFrames += static_cast<int64_t>(written);
    return static_cast<int32_t>(written);
  }

  void flush() {
    if (mFile != nullptr) std::fflush(mFile);
  }

  void close() {
    if (mFile == nullptr) return;
    const int64_t dataBytes = mFrames * mChannels * 4;
    uint8_t buf[4];
    putU32(buf, static_cast<uint32_t>(kHeaderBytes - 8 + dataBytes));  // RIFF size
    std::fseek(mFile, 4, SEEK_SET);
    std::fwrite(buf, 1, 4, mFile);
    putU32(buf, static_cast<uint32_t>(mFrames));  // fact: frames per channel
    std::fseek(mFile, 46, SEEK_SET);
    std::fwrite(buf, 1, 4, mFile);
    putU32(buf, static_cast<uint32_t>(dataBytes));  // data size
    std::fseek(mFile, 54, SEEK_SET);
    std::fwrite(buf, 1, 4, mFile);
    std::fclose(mFile);
    mFile = nullptr;
  }

 private:
  static constexpr size_t kHeaderBytes = 58;

  FILE* mFile = nullptr;
  int32_t mChannels = 2;
  int64_t mFrames = 0;
};

}  // namespace

// ---------------------------------------------------------------------------
// WavReader — parses RIFF chunks; accepts 16-bit PCM (format 1) and float32
// (format 3), mono or stereo. Converts to the engine's float/channel layout
// while reading. Owned and used exclusively by the DiskReader thread.
// ---------------------------------------------------------------------------
struct WavReader {
  FILE* file = nullptr;
  uint16_t format = 0;
  uint16_t bits = 0;
  int32_t channels = 0;
  int32_t rate = 0;
  int64_t dataOffset = 0;
  int64_t frames = 0;
  int64_t pos = 0;
  std::vector<uint8_t> raw;

  ~WavReader() { close(); }

  void close() {
    if (file != nullptr) {
      std::fclose(file);
      file = nullptr;
    }
  }

  int32_t bytesPerFrame() const { return channels * (bits / 8); }

  bool open(const std::string& path) {
    close();
    file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) return false;

    uint8_t hdr[12];
    if (std::fread(hdr, 1, 12, file) != 12 || std::memcmp(hdr, "RIFF", 4) != 0 ||
        std::memcmp(hdr + 8, "WAVE", 4) != 0) {
      close();
      return false;
    }

    bool haveFmt = false;
    int64_t dataBytes = -1;
    for (;;) {
      uint8_t ch[8];
      if (std::fread(ch, 1, 8, file) != 8) break;
      const uint32_t size = getU32(ch + 4);
      if (std::memcmp(ch, "fmt ", 4) == 0) {
        uint8_t fmt[16];
        if (size < 16 || std::fread(fmt, 1, 16, file) != 16) break;
        format = getU16(fmt + 0);
        channels = getU16(fmt + 2);
        rate = static_cast<int32_t>(getU32(fmt + 4));
        bits = getU16(fmt + 14);
        haveFmt = true;
        std::fseek(file, static_cast<long>(size - 16 + (size & 1)), SEEK_CUR);
      } else if (std::memcmp(ch, "data", 4) == 0) {
        dataOffset = std::ftell(file);
        dataBytes = size;
        break;
      } else {
        std::fseek(file, static_cast<long>(size + (size & 1)), SEEK_CUR);
      }
    }

    if (!haveFmt || dataBytes < 0 || channels < 1 || channels > 2 ||
        !((format == 1 && bits == 16) || (format == 3 && bits == 32))) {
      close();
      return false;
    }

    // Clamp to the real file size, and recover files whose header was never
    // patched (e.g. a capture interrupted by a crash: sizes still zero).
    std::fseek(file, 0, SEEK_END);
    const int64_t fileEnd = std::ftell(file);
    const int64_t available = fileEnd - dataOffset;
    if (dataBytes == 0 || dataBytes > available) dataBytes = available;

    frames = dataBytes / bytesPerFrame();
    return seekToFrame(0);
  }

  bool seekToFrame(int64_t frame) {
    if (file == nullptr) return false;
    if (std::fseek(file, static_cast<long>(dataOffset + frame * bytesPerFrame()), SEEK_SET) != 0) {
      return false;
    }
    pos = frame;
    return true;
  }

  // Reads up to maxFrames, converting to float and to dstChannels (1 or 2).
  int32_t readFrames(float* dst, int32_t maxFrames, int32_t dstChannels) {
    if (file == nullptr || maxFrames <= 0) return 0;
    const int64_t remain = frames - pos;
    const int32_t want = static_cast<int32_t>(std::min<int64_t>(maxFrames, remain));
    if (want <= 0) return 0;

    const size_t needBytes = static_cast<size_t>(want) * bytesPerFrame();
    if (raw.size() < needBytes) raw.resize(needBytes);  // reader thread: allocation is fine

    const int32_t got = static_cast<int32_t>(
        std::fread(raw.data(), static_cast<size_t>(bytesPerFrame()), static_cast<size_t>(want), file));
    pos += got;

    for (int32_t f = 0; f < got; ++f) {
      float left, right;
      if (format == 3) {
        const float* s = reinterpret_cast<const float*>(raw.data()) + f * channels;
        left = s[0];
        right = (channels == 2) ? s[1] : s[0];
      } else {
        const int16_t* s = reinterpret_cast<const int16_t*>(raw.data()) + f * channels;
        left = static_cast<float>(s[0]) / 32768.0f;
        right = static_cast<float>((channels == 2) ? s[1] : s[0]) / 32768.0f;
      }
      if (dstChannels == 1) {
        dst[f] = 0.5f * (left + right);
      } else {
        dst[f * 2] = left;
        dst[f * 2 + 1] = right;
      }
    }
    return got;
  }
};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

DiskSpooler::~DiskSpooler() { stop(); }

void DiskSpooler::configure(int32_t sampleRate, int32_t channelCount, int32_t maxBlockFrames) {
  mSampleRate = sampleRate;
  mChannels = channelCount;
  mMaxBlockFrames = maxBlockFrames;
  mCaptureRing.allocate(kCaptureRingFrames, channelCount);
  for (StreamSlot& slot : mSlots) slot.ring.allocate(kStreamRingFrames, channelCount);
  mMixScratch.assign(static_cast<size_t>(maxBlockFrames) * channelCount, 0.0f);
  mReadScratch.assign(static_cast<size_t>(kStreamChunkFrames) * channelCount, 0.0f);
}

void DiskSpooler::start() {
  if (mRunning) return;
  mQuit.store(false, std::memory_order_relaxed);
  mWriterThread = std::thread([this] { writerThreadMain(); });
  mReaderThread = std::thread([this] { readerThreadMain(); });
  mRunning = true;
}

void DiskSpooler::stop() {
  if (!mRunning) return;
  mQuit.store(true, std::memory_order_relaxed);
  mWriterCv.notify_all();
  mReaderCv.notify_all();
  if (mWriterThread.joinable()) mWriterThread.join();
  if (mReaderThread.joinable()) mReaderThread.join();
  mRunning = false;
}

// ---------------------------------------------------------------------------
// Control surface
// ---------------------------------------------------------------------------

void DiskSpooler::startCapture(const std::string& path) {
  std::lock_guard<std::mutex> lock(mWriterMutex);
  mWriterQueue.push_back({WriterCommand::Type::StartCapture, path});
  mWriterCv.notify_all();
}

void DiskSpooler::stopCapture() {
  std::lock_guard<std::mutex> lock(mWriterMutex);
  mWriterQueue.push_back({WriterCommand::Type::StopCapture, {}});
  mWriterCv.notify_all();
}

void DiskSpooler::openStream(int32_t slot, const std::string& path, bool loop) {
  if (!validSlot(slot)) return;
  std::lock_guard<std::mutex> lock(mReaderMutex);
  mReaderQueue.push_back({ReaderCommand::Type::Open, slot, path, loop});
  mReaderCv.notify_all();
}

void DiskSpooler::playStream(int32_t slot) {
  if (!validSlot(slot)) return;
  std::lock_guard<std::mutex> lock(mReaderMutex);
  mReaderQueue.push_back({ReaderCommand::Type::Play, slot, {}, false});
  mReaderCv.notify_all();
}

void DiskSpooler::pauseStream(int32_t slot) {
  if (!validSlot(slot)) return;
  // Immediate: the audio thread stops consuming from its next block on.
  mSlots[slot].playing.store(false, std::memory_order_release);
}

void DiskSpooler::closeStream(int32_t slot) {
  if (!validSlot(slot)) return;
  mSlots[slot].playing.store(false, std::memory_order_release);
  std::lock_guard<std::mutex> lock(mReaderMutex);
  mReaderQueue.push_back({ReaderCommand::Type::Close, slot, {}, false});
  mReaderCv.notify_all();
}

void DiskSpooler::setStreamGain(int32_t slot, float gain) {
  if (!validSlot(slot)) return;
  mSlots[slot].gain.store(gain < 0.0f ? 0.0f : (gain > 4.0f ? 4.0f : gain),
                          std::memory_order_relaxed);
}

StreamState DiskSpooler::streamState(int32_t slot) const {
  return validSlot(slot) ? mSlots[slot].state.load(std::memory_order_acquire)
                         : StreamState::Empty;
}

int64_t DiskSpooler::streamPositionFrames(int32_t slot) const {
  if (!validSlot(slot)) return 0;
  const int64_t length = mSlots[slot].length.load(std::memory_order_relaxed);
  const int64_t consumed = mSlots[slot].consumed.load(std::memory_order_relaxed);
  return length > 0 ? consumed % length : 0;
}

int64_t DiskSpooler::streamLengthFrames(int32_t slot) const {
  return validSlot(slot) ? mSlots[slot].length.load(std::memory_order_relaxed) : 0;
}

int64_t DiskSpooler::streamUnderrunFrames(int32_t slot) const {
  return validSlot(slot) ? mSlots[slot].underruns.load(std::memory_order_relaxed) : 0;
}

// ---------------------------------------------------------------------------
// AUDIO-THREAD entry points (lock-free; no file I/O, no allocation)
// ---------------------------------------------------------------------------

void DiskSpooler::writeCaptureFrames(const float* frames, int32_t n) {
  if (!mCaptureEnabled.load(std::memory_order_relaxed)) return;
  const int32_t written = mCaptureRing.writeFrames(frames, n);
  if (written < n) {
    // Disk stalled longer than the ring's ~2.7 s of headroom; drop the
    // newest frames and account for them.
    mCaptureDropped.fetch_add(n - written, std::memory_order_relaxed);
  }
}

void DiskSpooler::mixStreams(float* dst, int32_t frames) {
  if (frames > mMaxBlockFrames) frames = mMaxBlockFrames;
  for (StreamSlot& slot : mSlots) {
    if (!slot.playing.load(std::memory_order_acquire)) continue;
    const int32_t got = slot.ring.readFrames(mMixScratch.data(), frames);
    if (got > 0) {
      const float gain = slot.gain.load(std::memory_order_relaxed);
      const int32_t samples = got * mChannels;
      for (int32_t i = 0; i < samples; ++i) dst[i] += mMixScratch[i] * gain;
      slot.consumed.fetch_add(got, std::memory_order_relaxed);
    }
    if (got < frames) {
      // Reader fell behind (or the file just ended); the shortfall plays as
      // silence and no frames are lost — consumption resumes where it left off.
      slot.underruns.fetch_add(frames - got, std::memory_order_relaxed);
    }
  }
}

// ---------------------------------------------------------------------------
// Export helper — synchronous, caller's (non-realtime) thread
// ---------------------------------------------------------------------------

int64_t DiskSpooler::writeWavFile(const std::string& path, const float* interleaved,
                                  int64_t frames) {
  WavWriter wav;
  if (!wav.open(path, mSampleRate, mChannels)) {
    LOGW("export: failed to open %s", path.c_str());
    return -1;
  }
  int64_t done = 0;
  while (done < frames) {
    const int32_t n =
        static_cast<int32_t>(std::min<int64_t>(kWriterChunkFrames, frames - done));
    if (wav.writeFrames(interleaved + done * mChannels, n) != n) break;  // disk full?
    done += n;
  }
  wav.close();  // patches RIFF/fact/data sizes
  return done;
}

// ---------------------------------------------------------------------------
// DiskWriter thread — the ONLY place capture-file I/O happens
// ---------------------------------------------------------------------------

void DiskSpooler::writerThreadMain() {
  WavWriter wav;
  std::vector<float> staging(static_cast<size_t>(kWriterChunkFrames) * mChannels);

  const auto drainChunk = [&]() -> int32_t {
    const int32_t n = mCaptureRing.readFrames(staging.data(), kWriterChunkFrames);
    if (n > 0 && wav.isOpen()) {
      wav.writeFrames(staging.data(), n);
      mCapturedFrames.store(wav.frames(), std::memory_order_relaxed);
    }
    return n;
  };
  const auto drainAll = [&]() {
    while (drainChunk() > 0) {
    }
  };
  const auto finalize = [&]() {
    mCaptureEnabled.store(false, std::memory_order_relaxed);
    if (wav.isOpen() || mCaptureRing.framesReadable() > 0) {
      graceSleep();  // let the in-flight audio block finish writing
      drainAll();
    }
    wav.close();
    mCaptureActive.store(false, std::memory_order_release);
  };

  for (;;) {
    WriterCommand cmd;
    bool haveCommand = false;
    {
      std::unique_lock<std::mutex> lock(mWriterMutex);
      mWriterCv.wait_for(lock, kWorkerTick, [this] {
        return mQuit.load(std::memory_order_relaxed) || !mWriterQueue.empty();
      });
      if (!mWriterQueue.empty()) {
        cmd = std::move(mWriterQueue.front());
        mWriterQueue.pop_front();
        haveCommand = true;
      }
    }

    if (haveCommand) {
      switch (cmd.type) {
        case WriterCommand::Type::StartCapture:
          finalize();  // completes any capture already running
          mCaptureRing.reset();  // safe: gate off + grace elapsed inside finalize()
          if (wav.open(cmd.path, mSampleRate, mChannels)) {
            mCapturedFrames.store(0, std::memory_order_relaxed);
            mCaptureDropped.store(0, std::memory_order_relaxed);
            mCaptureActive.store(true, std::memory_order_release);
            mCaptureEnabled.store(true, std::memory_order_relaxed);
            LOGI("capture started: %s", cmd.path.c_str());
          } else {
            LOGW("capture failed to open: %s", cmd.path.c_str());
          }
          break;
        case WriterCommand::Type::StopCapture:
          finalize();
          LOGI("capture stopped");
          break;
      }
    }

    if (mQuit.load(std::memory_order_relaxed)) {
      finalize();  // never lose the tail of a take on shutdown
      return;
    }

    // Steady-state spooling: append whatever the audio thread produced.
    if (wav.isOpen() && drainChunk() > 0) wav.flush();
  }
}

// ---------------------------------------------------------------------------
// DiskReader thread — the ONLY place backing-track file I/O happens
// ---------------------------------------------------------------------------

void DiskSpooler::readerTopUpSlot(StreamSlot& slot) {
  while (slot.reader != nullptr && !slot.eof) {
    const int32_t space = slot.ring.framesWritable();
    if (space < kStreamChunkFrames / 4) break;  // avoid tiny reads
    const int32_t want = std::min(space, kStreamChunkFrames);
    const int32_t got = slot.reader->readFrames(mReadScratch.data(), want, mChannels);
    if (got > 0) slot.ring.writeFrames(mReadScratch.data(), got);
    if (got < want) {
      if (slot.loop && slot.reader->frames > 0) {
        slot.reader->seekToFrame(0);  // seamless wrap for looping streams
      } else {
        slot.eof = true;
        break;
      }
    }
  }
}

void DiskSpooler::readerHandleCommand(const ReaderCommand& cmd) {
  StreamSlot& slot = mSlots[cmd.slot];
  switch (cmd.type) {
    case ReaderCommand::Type::Open: {
      slot.playing.store(false, std::memory_order_release);
      slot.state.store(StreamState::Loading, std::memory_order_release);
      graceSleep();  // audio is off the ring after this
      slot.ring.reset();
      delete slot.reader;
      slot.reader = new WavReader();
      if (!slot.reader->open(cmd.path) || slot.reader->rate != mSampleRate) {
        LOGW("backing track rejected (missing/bad format/rate!=%d): %s", mSampleRate,
             cmd.path.c_str());
        delete slot.reader;
        slot.reader = nullptr;
        slot.length.store(0, std::memory_order_relaxed);
        slot.state.store(StreamState::Error, std::memory_order_release);
        return;
      }
      slot.loop = cmd.loop;
      slot.eof = false;
      slot.length.store(slot.reader->frames, std::memory_order_relaxed);
      slot.consumed.store(0, std::memory_order_relaxed);
      slot.underruns.store(0, std::memory_order_relaxed);
      readerTopUpSlot(slot);  // prebuffer before reporting Ready
      slot.state.store(StreamState::Ready, std::memory_order_release);
      LOGI("backing track ready: slot=%d frames=%lld %s", cmd.slot,
           static_cast<long long>(slot.reader->frames), cmd.path.c_str());
      break;
    }
    case ReaderCommand::Type::Play: {
      const StreamState st = slot.state.load(std::memory_order_acquire);
      if (slot.reader == nullptr) return;
      if (st == StreamState::Ended) {
        // Rewind and go again (audio stopped consuming when we flagged Ended).
        graceSleep();
        slot.ring.reset();
        slot.reader->seekToFrame(0);
        slot.eof = false;
        slot.consumed.store(0, std::memory_order_relaxed);
        readerTopUpSlot(slot);
      }
      if (st == StreamState::Ready || st == StreamState::Ended || st == StreamState::Playing) {
        slot.state.store(StreamState::Playing, std::memory_order_release);
        slot.playing.store(true, std::memory_order_release);
      }
      break;
    }
    case ReaderCommand::Type::Close: {
      slot.playing.store(false, std::memory_order_release);
      graceSleep();
      delete slot.reader;
      slot.reader = nullptr;
      slot.ring.reset();
      slot.loop = false;
      slot.eof = false;
      slot.length.store(0, std::memory_order_relaxed);
      slot.consumed.store(0, std::memory_order_relaxed);
      slot.state.store(StreamState::Empty, std::memory_order_release);
      break;
    }
  }
}

void DiskSpooler::readerThreadMain() {
  for (;;) {
    ReaderCommand cmd;
    bool haveCommand = false;
    {
      std::unique_lock<std::mutex> lock(mReaderMutex);
      mReaderCv.wait_for(lock, kWorkerTick, [this] {
        return mQuit.load(std::memory_order_relaxed) || !mReaderQueue.empty();
      });
      if (!mReaderQueue.empty()) {
        cmd = std::move(mReaderQueue.front());
        mReaderQueue.pop_front();
        haveCommand = true;
      }
    }
    if (haveCommand) readerHandleCommand(cmd);

    if (mQuit.load(std::memory_order_relaxed)) {
      for (StreamSlot& slot : mSlots) {
        slot.playing.store(false, std::memory_order_release);
        delete slot.reader;
        slot.reader = nullptr;
      }
      return;
    }

    for (StreamSlot& slot : mSlots) {
      const StreamState st = slot.state.load(std::memory_order_acquire);
      if (st == StreamState::Ready || st == StreamState::Playing) readerTopUpSlot(slot);
      // A non-looping stream is Ended once the file is exhausted AND the
      // audio thread has drained the last buffered frames.
      if (st == StreamState::Playing && slot.eof && slot.ring.framesReadable() == 0 &&
          slot.playing.load(std::memory_order_acquire)) {
        slot.playing.store(false, std::memory_order_release);
        slot.state.store(StreamState::Ended, std::memory_order_release);
      }
    }
  }
}

}  // namespace looper
