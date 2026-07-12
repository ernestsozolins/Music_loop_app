/*
 * AudioEngine_JNI — the JNI bridge for com.audio.loopstation.AudioEngine.
 *
 * Threading contract (matches AudioEngine.h):
 *  - Control hooks (record/playback/metronome/params) are safe to call from
 *    any Kotlin thread: they resolve to lock-free atomic stores or to the
 *    command queue, whose control-side producer is mutex-serialized inside
 *    the engine. The audio callbacks never see a JNI frame or a lock.
 *  - nativeReadWaveform must be driven by ONE reader (the 60 Hz polling
 *    coroutine in AudioEngine.kt) — the meter queue is single-consumer.
 *  - nativeCreate/nativeDestroy manage the engine lifetime; the Kotlin
 *    wrapper confines them and zeroes its handle so no call can race a
 *    destroyed engine.
 */

#include <jni.h>

#include <algorithm>
#include <memory>
#include <string>

#include "AudioEngine.h"

namespace {

using looper::AudioEngine;

JavaVM* gVm = nullptr;

// Attaches the current thread to the JVM if needed (engine events arrive on
// Oboe's error thread, which the JVM has never seen) and detaches on scope
// exit only if this scope did the attach.
class ScopedEnv {
 public:
  ScopedEnv() {
    if (gVm == nullptr) return;
    if (gVm->GetEnv(reinterpret_cast<void**>(&mEnv), JNI_VERSION_1_6) == JNI_EDETACHED) {
      // The NDK declares AttachCurrentThread(JNIEnv**, ...); desktop JDKs
      // (used for host static analysis) declare (void**, ...).
#if defined(__ANDROID__)
      const jint rc = gVm->AttachCurrentThread(&mEnv, nullptr);
#else
      const jint rc = gVm->AttachCurrentThread(reinterpret_cast<void**>(&mEnv), nullptr);
#endif
      if (rc == JNI_OK) {
        mAttached = true;
      } else {
        mEnv = nullptr;
      }
    }
  }
  ~ScopedEnv() {
    if (mAttached) gVm->DetachCurrentThread();
  }
  JNIEnv* get() const { return mEnv; }

 private:
  JNIEnv* mEnv = nullptr;
  bool mAttached = false;
};

// One flattened WaveformPoint = [inputRms, inputPeak, mixRms, mixPeak,
// playheadFrames, loopLengthFrames, state]. Must match AudioEngine.kt.
constexpr int32_t kFloatsPerPoint = 7;
constexpr int32_t kMaxPointsPerPoll = 128;

AudioEngine* fromHandle(jlong handle) { return reinterpret_cast<AudioEngine*>(handle); }

// Copies a jstring (e.g. a path built from Context.getFilesDir()) into a
// std::string the worker threads can own safely after this JNI frame ends.
std::string toStdString(JNIEnv* env, jstring s) {
  if (s == nullptr) return {};
  const char* chars = env->GetStringUTFChars(s, nullptr);
  std::string out(chars != nullptr ? chars : "");
  if (chars != nullptr) env->ReleaseStringUTFChars(s, chars);
  return out;
}

}  // namespace

extern "C" {

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* /*reserved*/) {
  gVm = vm;
  return JNI_VERSION_1_6;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

JNIEXPORT jlong JNICALL Java_com_audio_loopstation_AudioEngine_nativeCreate(
    JNIEnv* /*env*/, jobject /*thiz*/, jint sampleRate, jint channelCount, jint trackCount,
    jint maxLoopSeconds, jint lookAheadMillis, jint driftSlackMillis, jint inputDeviceId,
    jint outputDeviceId) {
  AudioEngine::Config config;
  config.sampleRate = sampleRate;
  config.channelCount = channelCount;
  config.trackCount = trackCount;
  config.maxLoopSeconds = maxLoopSeconds;
  config.lookAheadMillis = lookAheadMillis;
  config.driftSlackMillis = driftSlackMillis;
  config.inputDeviceId = inputDeviceId;
  config.outputDeviceId = outputDeviceId;
  return reinterpret_cast<jlong>(new AudioEngine(config));
}

JNIEXPORT void JNICALL Java_com_audio_loopstation_AudioEngine_nativeDestroy(JNIEnv*, jobject,
                                                                            jlong handle) {
  AudioEngine* engine = fromHandle(handle);
  if (engine == nullptr) return;
  engine->setEventCallback(nullptr);  // drop the Kotlin listener ref first
  delete engine;                      // ~AudioEngine() stops and closes the streams
}

// Registers `listener` (the Kotlin AudioEngine instance) to receive engine
// events via its `onNativeEvent(int type, int arg)` method. Events originate
// on Oboe's non-realtime error thread; ScopedEnv attaches it to the JVM per
// event. Pass null to unregister. The global ref is held by the callback
// closure and released (on a JVM-attached thread) when it is replaced or the
// engine is destroyed.
JNIEXPORT void JNICALL Java_com_audio_loopstation_AudioEngine_nativeSetEventListener(
    JNIEnv* env, jobject, jlong handle, jobject listener) {
  AudioEngine* engine = fromHandle(handle);
  if (engine == nullptr) return;
  if (listener == nullptr) {
    engine->setEventCallback(nullptr);
    return;
  }
  jclass cls = env->GetObjectClass(listener);
  const jmethodID method = env->GetMethodID(cls, "onNativeEvent", "(II)V");
  env->DeleteLocalRef(cls);
  if (method == nullptr) {
    env->ExceptionClear();
    return;
  }
  std::shared_ptr<_jobject> ref(env->NewGlobalRef(listener), [](jobject obj) {
    if (obj == nullptr) return;
    ScopedEnv scoped;
    if (scoped.get() != nullptr) scoped.get()->DeleteGlobalRef(obj);
  });
  engine->setEventCallback([ref, method](int32_t type, int32_t arg) {
    ScopedEnv scoped;
    JNIEnv* e = scoped.get();
    if (e == nullptr) return;
    e->CallVoidMethod(ref.get(), method, static_cast<jint>(type), static_cast<jint>(arg));
    if (e->ExceptionCheck()) e->ExceptionClear();  // never propagate into the error thread
  });
}

JNIEXPORT jboolean JNICALL Java_com_audio_loopstation_AudioEngine_nativeStart(JNIEnv*, jobject,
                                                                              jlong handle) {
  AudioEngine* engine = fromHandle(handle);
  return (engine != nullptr && engine->start() == oboe::Result::OK) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_com_audio_loopstation_AudioEngine_nativeStop(JNIEnv*, jobject,
                                                                         jlong handle) {
  if (AudioEngine* engine = fromHandle(handle)) engine->stop();
}

// Retarget playback/capture to an AudioDeviceInfo id (0 = system default).
// Reopens the streams if running; returns true on success. Blocks briefly —
// call off the main thread.
JNIEXPORT jboolean JNICALL Java_com_audio_loopstation_AudioEngine_nativeSetOutputDevice(
    JNIEnv*, jobject, jlong handle, jint deviceId) {
  AudioEngine* engine = fromHandle(handle);
  return (engine != nullptr && engine->setOutputDevice(deviceId) == oboe::Result::OK) ? JNI_TRUE
                                                                                      : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_com_audio_loopstation_AudioEngine_nativeSetInputDevice(
    JNIEnv*, jobject, jlong handle, jint deviceId) {
  AudioEngine* engine = fromHandle(handle);
  return (engine != nullptr && engine->setInputDevice(deviceId) == oboe::Result::OK) ? JNI_TRUE
                                                                                     : JNI_FALSE;
}

JNIEXPORT jint JNICALL Java_com_audio_loopstation_AudioEngine_nativeGetOutputDevice(JNIEnv*, jobject,
                                                                                    jlong handle) {
  AudioEngine* engine = fromHandle(handle);
  return engine != nullptr ? engine->outputDeviceId() : 0;
}

// ---------------------------------------------------------------------------
// Transport
// ---------------------------------------------------------------------------

JNIEXPORT void JNICALL Java_com_audio_loopstation_AudioEngine_nativeStartRecording(JNIEnv*, jobject,
                                                                                   jlong handle) {
  if (AudioEngine* engine = fromHandle(handle)) engine->startRecording();
}

JNIEXPORT void JNICALL Java_com_audio_loopstation_AudioEngine_nativeStopRecording(JNIEnv*, jobject,
                                                                                  jlong handle) {
  if (AudioEngine* engine = fromHandle(handle)) engine->stopRecording();
}

JNIEXPORT void JNICALL Java_com_audio_loopstation_AudioEngine_nativeStartPlayback(JNIEnv*, jobject,
                                                                                  jlong handle) {
  if (AudioEngine* engine = fromHandle(handle)) engine->play();
}

JNIEXPORT void JNICALL Java_com_audio_loopstation_AudioEngine_nativeStopPlayback(JNIEnv*, jobject,
                                                                                 jlong handle) {
  if (AudioEngine* engine = fromHandle(handle)) engine->stopPlayback();
}

JNIEXPORT void JNICALL Java_com_audio_loopstation_AudioEngine_nativeClearAll(JNIEnv*, jobject,
                                                                             jlong handle) {
  if (AudioEngine* engine = fromHandle(handle)) engine->clearAll();
}

JNIEXPORT void JNICALL Java_com_audio_loopstation_AudioEngine_nativeClearTrack(JNIEnv*, jobject,
                                                                               jlong handle,
                                                                               jint track) {
  if (AudioEngine* engine = fromHandle(handle)) engine->clearTrack(track);
}

JNIEXPORT void JNICALL Java_com_audio_loopstation_AudioEngine_nativeResetLoopForRestore(
    JNIEnv*, jobject, jlong handle) {
  if (AudioEngine* engine = fromHandle(handle)) engine->resetLoopForRestore();
}

// ---------------------------------------------------------------------------
// Output monitoring reverb — parameters are lock-free atomics, safe to drive
// from a UI slider mid-performance. Applied to the headphone/speaker mix
// only; recordings and exported stems stay dry.
// ---------------------------------------------------------------------------

JNIEXPORT void JNICALL Java_com_audio_loopstation_AudioEngine_nativeSetReverbRoomSize(
    JNIEnv*, jobject, jlong handle, jfloat size) {
  if (AudioEngine* engine = fromHandle(handle)) engine->setReverbRoomSize(size);
}

JNIEXPORT void JNICALL Java_com_audio_loopstation_AudioEngine_nativeSetReverbMix(
    JNIEnv*, jobject, jlong handle, jfloat mix) {
  if (AudioEngine* engine = fromHandle(handle)) engine->setReverbMix(mix);
}

JNIEXPORT void JNICALL Java_com_audio_loopstation_AudioEngine_nativeSetReverbDamping(
    JNIEnv*, jobject, jlong handle, jfloat damping) {
  if (AudioEngine* engine = fromHandle(handle)) engine->setReverbDamping(damping);
}

JNIEXPORT void JNICALL Java_com_audio_loopstation_AudioEngine_nativeSetReverbEnabled(
    JNIEnv*, jobject, jlong handle, jboolean enabled) {
  if (AudioEngine* engine = fromHandle(handle)) engine->setReverbEnabled(enabled == JNI_TRUE);
}

// ---------------------------------------------------------------------------
// Latency calibration (ping-and-listen)
// ---------------------------------------------------------------------------

JNIEXPORT void JNICALL Java_com_audio_loopstation_AudioEngine_nativeStartCalibration(
    JNIEnv*, jobject, jlong handle) {
  if (AudioEngine* engine = fromHandle(handle)) engine->calibrateLatency();
}

JNIEXPORT void JNICALL Java_com_audio_loopstation_AudioEngine_nativeCancelCalibration(
    JNIEnv*, jobject, jlong handle) {
  if (AudioEngine* engine = fromHandle(handle)) engine->cancelCalibration();
}

// LatencyCalibrator::State: 0 idle, 1 running, 2 succeeded, 3 failed.
JNIEXPORT jint JNICALL Java_com_audio_loopstation_AudioEngine_nativeGetCalibrationState(
    JNIEnv*, jobject, jlong handle) {
  AudioEngine* engine = fromHandle(handle);
  return engine != nullptr ? engine->calibrationState() : 0;
}

// Last successful round-trip measurement in frames, -1 if none yet. On
// success the engine has already applied it as the overdub record offset.
JNIEXPORT jint JNICALL Java_com_audio_loopstation_AudioEngine_nativeGetCalibratedLatencyFrames(
    JNIEnv*, jobject, jlong handle) {
  AudioEngine* engine = fromHandle(handle);
  return engine != nullptr ? engine->calibratedLatencyFrames() : -1;
}

// ---------------------------------------------------------------------------
// Metronome
// ---------------------------------------------------------------------------

JNIEXPORT void JNICALL Java_com_audio_loopstation_AudioEngine_nativeSetMetronomeState(
    JNIEnv*, jobject, jlong handle, jboolean isActive, jfloat bpm, jint beatsPerMeasure) {
  if (AudioEngine* engine = fromHandle(handle)) {
    engine->setMetronomeState(isActive == JNI_TRUE, bpm, beatsPerMeasure);
  }
}

JNIEXPORT void JNICALL Java_com_audio_loopstation_AudioEngine_nativeSetMetronomeGain(
    JNIEnv*, jobject, jlong handle, jfloat gain) {
  if (AudioEngine* engine = fromHandle(handle)) engine->setMetronomeGain(gain);
}

JNIEXPORT void JNICALL Java_com_audio_loopstation_AudioEngine_nativeSetMetronomeSync(
    JNIEnv*, jobject, jlong handle, jboolean enabled) {
  if (AudioEngine* engine = fromHandle(handle)) {
    engine->setMetronomeSyncToLoop(enabled == JNI_TRUE);
  }
}

JNIEXPORT void JNICALL Java_com_audio_loopstation_AudioEngine_nativeSetCountInEnabled(
    JNIEnv*, jobject, jlong handle, jboolean enabled) {
  if (AudioEngine* engine = fromHandle(handle)) engine->setCountInEnabled(enabled == JNI_TRUE);
}

JNIEXPORT void JNICALL Java_com_audio_loopstation_AudioEngine_nativeSetLoopQuantize(
    JNIEnv*, jobject, jlong handle, jboolean enabled) {
  if (AudioEngine* engine = fromHandle(handle)) engine->setLoopQuantize(enabled == JNI_TRUE);
}

JNIEXPORT void JNICALL Java_com_audio_loopstation_AudioEngine_nativeSetInputChannels(
    JNIEnv*, jobject, jlong handle, jint channels, jint mapLeft, jint mapRight) {
  if (AudioEngine* engine = fromHandle(handle)) {
    engine->setInputChannels(channels, mapLeft, mapRight);
  }
}

// Packed beat info for the UI flash: bit 7 = active, bits 0..6 = beat-in-bar,
// bits 8+ = monotonic beat count. Must match AudioEngine.kt decoding.
JNIEXPORT jlong JNICALL Java_com_audio_loopstation_AudioEngine_nativeGetBeatInfo(JNIEnv*, jobject,
                                                                                 jlong handle) {
  AudioEngine* engine = fromHandle(handle);
  if (engine == nullptr) return 0;
  const jlong count = static_cast<jlong>(engine->metronomeBeatCount());
  const jlong inBar = static_cast<jlong>(engine->metronomeBeatInBar() & 0x7F);
  const jlong active = engine->metronomeActive() ? 0x80 : 0x00;
  return (count << 8) | active | inBar;
}

// ---------------------------------------------------------------------------
// Disk spooling — paths arrive from Kotlin (e.g. under
// Context.getFilesDir().absolutePath); all file I/O runs on the spooler's
// worker threads, never on this JNI frame's thread and never on audio threads.
// ---------------------------------------------------------------------------

JNIEXPORT void JNICALL Java_com_audio_loopstation_AudioEngine_nativeStartCapture(
    JNIEnv* env, jobject, jlong handle, jstring path, jboolean captureMix) {
  if (AudioEngine* engine = fromHandle(handle)) {
    engine->startCapture(toStdString(env, path), captureMix == JNI_TRUE);
  }
}

JNIEXPORT void JNICALL Java_com_audio_loopstation_AudioEngine_nativeStopCapture(JNIEnv*, jobject,
                                                                                jlong handle) {
  if (AudioEngine* engine = fromHandle(handle)) engine->stopCapture();
}

JNIEXPORT jboolean JNICALL Java_com_audio_loopstation_AudioEngine_nativeIsCapturing(JNIEnv*, jobject,
                                                                                    jlong handle) {
  AudioEngine* engine = fromHandle(handle);
  return (engine != nullptr && engine->isCapturing()) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jlong JNICALL Java_com_audio_loopstation_AudioEngine_nativeGetCapturedFrames(
    JNIEnv*, jobject, jlong handle) {
  AudioEngine* engine = fromHandle(handle);
  return engine != nullptr ? engine->capturedFrames() : 0;
}

JNIEXPORT jlong JNICALL Java_com_audio_loopstation_AudioEngine_nativeGetCaptureDroppedFrames(
    JNIEnv*, jobject, jlong handle) {
  AudioEngine* engine = fromHandle(handle);
  return engine != nullptr ? engine->captureDroppedFrames() : 0;
}

JNIEXPORT void JNICALL Java_com_audio_loopstation_AudioEngine_nativeOpenBackingTrack(
    JNIEnv* env, jobject, jlong handle, jint slot, jstring path, jboolean loop) {
  if (AudioEngine* engine = fromHandle(handle)) {
    engine->openBackingTrack(slot, toStdString(env, path), loop == JNI_TRUE);
  }
}

JNIEXPORT void JNICALL Java_com_audio_loopstation_AudioEngine_nativePlayBackingTrack(
    JNIEnv*, jobject, jlong handle, jint slot) {
  if (AudioEngine* engine = fromHandle(handle)) engine->playBackingTrack(slot);
}

JNIEXPORT void JNICALL Java_com_audio_loopstation_AudioEngine_nativePauseBackingTrack(
    JNIEnv*, jobject, jlong handle, jint slot) {
  if (AudioEngine* engine = fromHandle(handle)) engine->pauseBackingTrack(slot);
}

JNIEXPORT void JNICALL Java_com_audio_loopstation_AudioEngine_nativeCloseBackingTrack(
    JNIEnv*, jobject, jlong handle, jint slot) {
  if (AudioEngine* engine = fromHandle(handle)) engine->closeBackingTrack(slot);
}

JNIEXPORT void JNICALL Java_com_audio_loopstation_AudioEngine_nativeSetBackingTrackGain(
    JNIEnv*, jobject, jlong handle, jint slot, jfloat gain) {
  if (AudioEngine* engine = fromHandle(handle)) engine->setBackingTrackGain(slot, gain);
}

JNIEXPORT jint JNICALL Java_com_audio_loopstation_AudioEngine_nativeGetBackingTrackState(
    JNIEnv*, jobject, jlong handle, jint slot) {
  AudioEngine* engine = fromHandle(handle);
  return engine != nullptr ? static_cast<jint>(engine->backingTrackState(slot)) : 0;
}

JNIEXPORT jlong JNICALL Java_com_audio_loopstation_AudioEngine_nativeGetBackingTrackPosition(
    JNIEnv*, jobject, jlong handle, jint slot) {
  AudioEngine* engine = fromHandle(handle);
  return engine != nullptr ? engine->backingTrackPositionFrames(slot) : 0;
}

JNIEXPORT jlong JNICALL Java_com_audio_loopstation_AudioEngine_nativeGetBackingTrackLength(
    JNIEnv*, jobject, jlong handle, jint slot) {
  AudioEngine* engine = fromHandle(handle);
  return engine != nullptr ? engine->backingTrackLengthFrames(slot) : 0;
}

// ---------------------------------------------------------------------------
// Session finalization & stem export
// ---------------------------------------------------------------------------

// Flushes the remaining ring-buffer audio to disk, patches the RIFF/data
// chunk sizes, and closes the capture file handle. BLOCKS up to
// timeoutMillis — call from a background dispatcher, never the main thread.
JNIEXPORT jboolean JNICALL Java_com_audio_loopstation_AudioEngine_nativeFlushAndCloseSession(
    JNIEnv*, jobject, jlong handle, jint timeoutMillis) {
  AudioEngine* engine = fromHandle(handle);
  return (engine != nullptr && engine->flushAndCloseSession(timeoutMillis)) ? JNI_TRUE
                                                                            : JNI_FALSE;
}

// Starts the asynchronous per-track stem export into `directory` (which must
// already exist). Poll nativeGetExportState; count via
// nativeGetExportedStemCount.
JNIEXPORT jboolean JNICALL Java_com_audio_loopstation_AudioEngine_nativeExportStems(
    JNIEnv* env, jobject, jlong handle, jstring directory) {
  AudioEngine* engine = fromHandle(handle);
  return (engine != nullptr && engine->exportStems(toStdString(env, directory))) ? JNI_TRUE
                                                                                 : JNI_FALSE;
}

// AudioEngine::kExport*: 0 idle, 1 running, 2 done, 3 failed.
JNIEXPORT jint JNICALL Java_com_audio_loopstation_AudioEngine_nativeGetExportState(JNIEnv*, jobject,
                                                                                   jlong handle) {
  AudioEngine* engine = fromHandle(handle);
  return engine != nullptr ? engine->exportState() : 0;
}

JNIEXPORT jint JNICALL Java_com_audio_loopstation_AudioEngine_nativeGetExportedStemCount(
    JNIEnv*, jobject, jlong handle) {
  AudioEngine* engine = fromHandle(handle);
  return engine != nullptr ? engine->exportedStemCount() : 0;
}

// Starts the asynchronous session restore: parallel arrays of track slots
// and stem .wav paths (from the Room persistence layer). The worker loads
// the buffers; the audio thread adopts loop length + content flags via
// RestoreCommit. Poll nativeGetRestoreState.
JNIEXPORT jboolean JNICALL Java_com_audio_loopstation_AudioEngine_nativeRestoreSession(
    JNIEnv* env, jobject, jlong handle, jintArray trackIndices, jobjectArray paths) {
  AudioEngine* engine = fromHandle(handle);
  if (engine == nullptr || trackIndices == nullptr || paths == nullptr) return JNI_FALSE;
  const jsize count =
      std::min(env->GetArrayLength(trackIndices), env->GetArrayLength(paths));
  if (count <= 0) return JNI_FALSE;

  jint* indices = env->GetIntArrayElements(trackIndices, nullptr);
  if (indices == nullptr) return JNI_FALSE;
  std::vector<AudioEngine::RestoreFile> files;
  files.reserve(static_cast<size_t>(count));
  for (jsize i = 0; i < count; ++i) {
    auto path = static_cast<jstring>(env->GetObjectArrayElement(paths, i));
    files.push_back({indices[i], toStdString(env, path)});
    env->DeleteLocalRef(path);
  }
  env->ReleaseIntArrayElements(trackIndices, indices, JNI_ABORT);
  return engine->restoreSession(std::move(files)) ? JNI_TRUE : JNI_FALSE;
}

// AudioEngine::kRestore*: 0 idle, 1 running, 2 done, 3 failed.
JNIEXPORT jint JNICALL Java_com_audio_loopstation_AudioEngine_nativeGetRestoreState(
    JNIEnv*, jobject, jlong handle) {
  AudioEngine* engine = fromHandle(handle);
  return engine != nullptr ? engine->restoreState() : 0;
}

// ---------------------------------------------------------------------------
// Per-pass undo + offline track waveform
// ---------------------------------------------------------------------------

JNIEXPORT void JNICALL Java_com_audio_loopstation_AudioEngine_nativeSnapshotTrackForUndo(
    JNIEnv*, jobject, jlong handle, jint track) {
  if (AudioEngine* engine = fromHandle(handle)) engine->snapshotTrackForUndo(track);
}

// BLOCKS ~30 ms while the audio thread releases the track; call from a
// background dispatcher.
JNIEXPORT jboolean JNICALL Java_com_audio_loopstation_AudioEngine_nativeUndoLastPass(
    JNIEnv*, jobject, jlong handle) {
  AudioEngine* engine = fromHandle(handle);
  return (engine != nullptr && engine->undoLastPass()) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL Java_com_audio_loopstation_AudioEngine_nativeGetUndoPassTrack(
    JNIEnv*, jobject, jlong handle) {
  AudioEngine* engine = fromHandle(handle);
  return engine != nullptr ? engine->undoPassTrack() : -1;
}

// Silences the first `frames` of a track's loop (cut a bad start); undoable.
// BLOCKS ~30 ms — call from a background dispatcher.
JNIEXPORT jboolean JNICALL Java_com_audio_loopstation_AudioEngine_nativeTrimTrackStart(
    JNIEnv*, jobject, jlong handle, jint track, jint frames) {
  AudioEngine* engine = fromHandle(handle);
  return (engine != nullptr && engine->trimTrackStart(track, frames)) ? JNI_TRUE : JNI_FALSE;
}

// Fills `dest` with downsampled |peak| bins of the track's loop audio;
// returns the bin count (0 = empty track / no loop).
JNIEXPORT jint JNICALL Java_com_audio_loopstation_AudioEngine_nativeGetTrackWaveform(
    JNIEnv* env, jobject, jlong handle, jint track, jfloatArray dest) {
  AudioEngine* engine = fromHandle(handle);
  if (engine == nullptr || dest == nullptr) return 0;
  constexpr int32_t kMaxWaveformBins = 512;
  float bins[kMaxWaveformBins];
  const int32_t maxBins =
      std::min<int32_t>(kMaxWaveformBins, static_cast<int32_t>(env->GetArrayLength(dest)));
  const int32_t n = engine->trackWaveform(track, bins, maxBins);
  if (n > 0) env->SetFloatArrayRegion(dest, 0, n, bins);
  return n;
}

// ---------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------

JNIEXPORT void JNICALL Java_com_audio_loopstation_AudioEngine_nativeSelectTrack(JNIEnv*, jobject,
                                                                                jlong handle,
                                                                                jint track) {
  if (AudioEngine* engine = fromHandle(handle)) engine->selectTrack(track);
}

JNIEXPORT void JNICALL Java_com_audio_loopstation_AudioEngine_nativeSetTrackGain(JNIEnv*, jobject,
                                                                                 jlong handle,
                                                                                 jint track,
                                                                                 jfloat gain) {
  if (AudioEngine* engine = fromHandle(handle)) engine->setTrackGain(track, gain);
}

JNIEXPORT void JNICALL Java_com_audio_loopstation_AudioEngine_nativeSetTrackPan(
    JNIEnv*, jobject, jlong handle, jint track, jfloat pan) {
  if (AudioEngine* engine = fromHandle(handle)) engine->setTrackPan(track, pan);
}

JNIEXPORT void JNICALL Java_com_audio_loopstation_AudioEngine_nativeSetTrackMuted(
    JNIEnv*, jobject, jlong handle, jint track, jboolean muted) {
  if (AudioEngine* engine = fromHandle(handle)) engine->setTrackMuted(track, muted == JNI_TRUE);
}

JNIEXPORT void JNICALL Java_com_audio_loopstation_AudioEngine_nativeSetTrackShift(
    JNIEnv*, jobject, jlong handle, jint track, jint frames) {
  if (AudioEngine* engine = fromHandle(handle)) engine->setTrackShiftFrames(track, frames);
}

JNIEXPORT jint JNICALL Java_com_audio_loopstation_AudioEngine_nativeGetTrackShift(
    JNIEnv*, jobject, jlong handle, jint track) {
  AudioEngine* engine = fromHandle(handle);
  return engine != nullptr ? engine->trackShiftFrames(track) : 0;
}

// Bit t = track t has content, bit (t+16) = track t is clearing.
JNIEXPORT jint JNICALL Java_com_audio_loopstation_AudioEngine_nativeGetTrackContentMask(
    JNIEnv*, jobject, jlong handle) {
  AudioEngine* engine = fromHandle(handle);
  return engine != nullptr ? static_cast<jint>(engine->trackContentMask()) : 0;
}

JNIEXPORT void JNICALL Java_com_audio_loopstation_AudioEngine_nativeSetMonitorGain(JNIEnv*, jobject,
                                                                                   jlong handle,
                                                                                   jfloat gain) {
  if (AudioEngine* engine = fromHandle(handle)) engine->setMonitorGain(gain);
}

JNIEXPORT void JNICALL Java_com_audio_loopstation_AudioEngine_nativeSetRecordOffsetFrames(
    JNIEnv*, jobject, jlong handle, jint frames) {
  if (AudioEngine* engine = fromHandle(handle)) engine->setRecordOffsetFrames(frames);
}

// ---------------------------------------------------------------------------
// Waveform / observability
// ---------------------------------------------------------------------------

// Drains the lock-free meter queue into `dest` as flattened points of
// kFloatsPerPoint floats each. Returns the number of POINTS written. Called
// by the single 60 Hz polling coroutine.
JNIEXPORT jint JNICALL Java_com_audio_loopstation_AudioEngine_nativeReadWaveform(
    JNIEnv* env, jobject, jlong handle, jfloatArray dest) {
  AudioEngine* engine = fromHandle(handle);
  if (engine == nullptr || dest == nullptr) return 0;

  const jsize destFloats = env->GetArrayLength(dest);
  const int32_t maxPoints =
      std::min<int32_t>(kMaxPointsPerPoll, static_cast<int32_t>(destFloats) / kFloatsPerPoint);
  if (maxPoints <= 0) return 0;

  looper::WaveformPoint points[kMaxPointsPerPoll];
  const int32_t n = engine->readWaveform(points, maxPoints);
  if (n <= 0) return 0;

  float flat[kMaxPointsPerPoll * kFloatsPerPoint];
  for (int32_t i = 0; i < n; ++i) {
    float* p = flat + i * kFloatsPerPoint;
    p[0] = points[i].inputRms;
    p[1] = points[i].inputPeak;
    p[2] = points[i].mixRms;
    p[3] = points[i].mixPeak;
    p[4] = static_cast<float>(points[i].playheadFrames);
    p[5] = static_cast<float>(points[i].loopLengthFrames);
    p[6] = static_cast<float>(points[i].state);
  }
  env->SetFloatArrayRegion(dest, 0, n * kFloatsPerPoint, flat);
  return n;
}

JNIEXPORT jint JNICALL Java_com_audio_loopstation_AudioEngine_nativeGetState(JNIEnv*, jobject,
                                                                             jlong handle) {
  AudioEngine* engine = fromHandle(handle);
  return engine != nullptr ? static_cast<jint>(engine->state()) : 0;
}

JNIEXPORT jint JNICALL Java_com_audio_loopstation_AudioEngine_nativeGetSampleRate(JNIEnv*, jobject,
                                                                                  jlong handle) {
  AudioEngine* engine = fromHandle(handle);
  return engine != nullptr ? engine->sampleRate() : 0;
}

JNIEXPORT jint JNICALL Java_com_audio_loopstation_AudioEngine_nativeGetLoopLengthFrames(
    JNIEnv*, jobject, jlong handle) {
  AudioEngine* engine = fromHandle(handle);
  return engine != nullptr ? engine->loopLengthFrames() : 0;
}

JNIEXPORT jint JNICALL Java_com_audio_loopstation_AudioEngine_nativeGetPlayheadFrames(
    JNIEnv*, jobject, jlong handle) {
  AudioEngine* engine = fromHandle(handle);
  return engine != nullptr ? engine->playheadFrames() : 0;
}

JNIEXPORT jint JNICALL Java_com_audio_loopstation_AudioEngine_nativeGetTrackCount(JNIEnv*, jobject,
                                                                                  jlong handle) {
  AudioEngine* engine = fromHandle(handle);
  return engine != nullptr ? engine->trackCount() : 0;
}

}  // extern "C"
