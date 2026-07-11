#ifndef LOOPER_DISK_SPOOLER_H
#define LOOPER_DISK_SPOOLER_H

/*
 * DiskSpooler — producer/consumer disk I/O for the looper engine.
 *
 * Long, multi-minute material cannot live in RAM. This class moves it to
 * disk through two dedicated background std::threads, decoupled from the
 * realtime audio callbacks by lock-free SPSC rings:
 *
 *   CAPTURE (recording spool)
 *     audio thread --writeCaptureFrames()--> [capture ring] --> DiskWriter
 *     thread --fwrite--> take.wav (RIFF, 32-bit float, appended while the
 *     capture is active; header sizes patched on close)
 *
 *   BACKING STREAMS (playback spool)
 *     backing.wav --fread-- DiskReader thread --> [per-slot ring]
 *     --mixStreams()--> audio thread output mix
 *
 * THREAD-SAFETY CONTRACT
 *   - The audio callback interacts ONLY with the rings + atomics
 *     (writeCaptureFrames / mixStreams / captureEnabled). It never opens,
 *     reads, writes, or closes a file.
 *   - fopen/fread/fwrite/fseek/fclose happen exclusively on the worker
 *     threads. Control-thread calls just enqueue commands (mutex + condvar
 *     — legal, the audio thread never touches them).
 *   - Ring resets follow a grace-window pattern: the audio-side gate
 *     (captureEnabled / slot.playing) is cleared first, then the worker
 *     sleeps longer than one audio callback before touching the ring, so
 *     producer/consumer are quiescent when reset() runs.
 *
 * WAV support: writes IEEE-float32 (the engine's native format); reads
 * float32 and 16-bit PCM, mono or stereo, at the engine sample rate
 * (mismatched rates are rejected — resampling is a later-phase item).
 */

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "LockFreeRing.h"

namespace looper {

constexpr int32_t kMaxBackingStreams = 2;

enum class StreamState : uint8_t {
  Empty = 0,  // no file loaded
  Loading,    // reader thread is opening/prebuffering
  Ready,      // prebuffered, waiting for playStream()
  Playing,    // audio thread is consuming
  Ended,      // reached EOF (non-looping) and the ring drained
  Error,      // open failed (missing file, bad format, rate mismatch)
};

struct WavReader;  // defined in DiskSpooler.cpp; owned by the reader thread

class DiskSpooler {
 public:
  DiskSpooler() = default;
  ~DiskSpooler();

  DiskSpooler(const DiskSpooler&) = delete;
  DiskSpooler& operator=(const DiskSpooler&) = delete;

  // ----- Lifecycle (control thread) -----
  // configure() before start(); allocates every ring/scratch buffer so the
  // audio-thread entry points never allocate.
  void configure(int32_t sampleRate, int32_t channelCount, int32_t maxBlockFrames);
  void start();  // spawns the DiskWriter + DiskReader threads (idempotent)
  void stop();   // finalizes any open files and joins both threads

  // ----- Capture control (control thread; async — executed by DiskWriter) -----
  void startCapture(const std::string& path);  // finalizes any current file first
  void stopCapture();
  bool isCapturing() const { return mCaptureActive.load(std::memory_order_acquire); }
  int64_t capturedFrames() const { return mCapturedFrames.load(std::memory_order_relaxed); }
  int64_t captureDroppedFrames() const { return mCaptureDropped.load(std::memory_order_relaxed); }

  // ----- Capture data path (AUDIO THREAD; lock-free) -----
  bool captureEnabled() const { return mCaptureEnabled.load(std::memory_order_relaxed); }
  void writeCaptureFrames(const float* frames, int32_t n);

  // ----- Backing-stream control (control thread; async where noted) -----
  void openStream(int32_t slot, const std::string& path, bool loop);  // async -> Ready/Error
  void playStream(int32_t slot);   // async; rewinds if Ended
  void pauseStream(int32_t slot);  // immediate (atomic gate)
  void closeStream(int32_t slot);  // async -> Empty
  void setStreamGain(int32_t slot, float gain);
  StreamState streamState(int32_t slot) const;
  int64_t streamPositionFrames(int32_t slot) const;  // position within the file
  int64_t streamLengthFrames(int32_t slot) const;
  int64_t streamUnderrunFrames(int32_t slot) const;

  // ----- Backing-stream data path (AUDIO THREAD; lock-free) -----
  // Adds up to `frames` of every playing stream (with its gain) into `dst`.
  void mixStreams(float* dst, int32_t frames);

  // ----- Export/restore helpers (any NON-realtime thread; synchronous) -----
  // Writes an interleaved float buffer to a IEEE-float32 .wav (format tag 3)
  // at `path`, RIFF/fact/data sizes patched on close. Used by the engine's
  // stem-export worker. Returns frames written (-1 if the file failed to
  // open, short count on a write error).
  int64_t writeWavFile(const std::string& path, const float* interleaved, int64_t frames);

  // Reads an entire .wav into `dest` (interleaved engine channel layout,
  // float32/PCM16 sources converted, up to maxFrames). The file must match
  // the engine sample rate. Used by the engine's session-restore worker.
  // Returns frames read, or -1 on open/format/rate failure.
  int64_t readWavFile(const std::string& path, float* dest, int64_t maxFrames);

 private:
  struct WriterCommand {
    enum class Type : uint8_t { StartCapture, StopCapture };
    Type type;
    std::string path;
  };

  struct ReaderCommand {
    enum class Type : uint8_t { Open, Play, Close };
    Type type;
    int32_t slot = 0;
    std::string path;
    bool loop = false;
  };

  struct StreamSlot {
    SpscSampleRing ring;  // producer: reader thread, consumer: audio thread
    std::atomic<StreamState> state{StreamState::Empty};
    std::atomic<bool> playing{false};  // audio-side consume gate
    std::atomic<float> gain{1.0f};
    std::atomic<int64_t> consumed{0};
    std::atomic<int64_t> length{0};
    std::atomic<int64_t> underruns{0};
    // Reader-thread-only fields:
    WavReader* reader = nullptr;  // new/delete on the reader thread only
    bool loop = false;
    bool eof = false;
  };

  void writerThreadMain();
  void readerThreadMain();
  void readerHandleCommand(const ReaderCommand& cmd);
  void readerTopUpSlot(StreamSlot& slot);
  bool validSlot(int32_t slot) const { return slot >= 0 && slot < kMaxBackingStreams; }

  int32_t mSampleRate = 48000;
  int32_t mChannels = 2;
  int32_t mMaxBlockFrames = 8192;

  // ----- capture -----
  SpscSampleRing mCaptureRing;
  std::atomic<bool> mCaptureEnabled{false};  // audio-side tee gate
  std::atomic<bool> mCaptureActive{false};   // a file is open on the writer
  std::atomic<int64_t> mCapturedFrames{0};
  std::atomic<int64_t> mCaptureDropped{0};

  // ----- streams -----
  StreamSlot mSlots[kMaxBackingStreams];
  std::vector<float> mMixScratch;   // audio thread only
  std::vector<float> mReadScratch;  // reader thread only

  // ----- worker plumbing (control <-> worker threads only) -----
  std::atomic<bool> mQuit{false};
  bool mRunning = false;
  std::thread mWriterThread;
  std::thread mReaderThread;
  std::mutex mWriterMutex;
  std::condition_variable mWriterCv;
  std::deque<WriterCommand> mWriterQueue;
  std::mutex mReaderMutex;
  std::condition_variable mReaderCv;
  std::deque<ReaderCommand> mReaderQueue;
};

}  // namespace looper

#endif  // LOOPER_DISK_SPOOLER_H
