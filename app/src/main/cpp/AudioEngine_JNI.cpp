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
#include <string>

#include "AudioEngine.h"

namespace {

using looper::AudioEngine;

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
  delete fromHandle(handle);  // ~AudioEngine() stops and closes the streams
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

JNIEXPORT void JNICALL Java_com_audio_loopstation_AudioEngine_nativeSetTrackMuted(
    JNIEnv*, jobject, jlong handle, jint track, jboolean muted) {
  if (AudioEngine* engine = fromHandle(handle)) engine->setTrackMuted(track, muted == JNI_TRUE);
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
