#ifndef LOOPER_LOG_H
#define LOOPER_LOG_H

// Control-plane logging only — nothing on the onAudioReady path may log.
// Host builds (unit tests / static analysis) have no liblog.
#if defined(__ANDROID__)
#include <android/log.h>
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "LooperEngine", __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, "LooperEngine", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "LooperEngine", __VA_ARGS__)
#else
#define LOGI(...) ((void)0)
#define LOGW(...) ((void)0)
#define LOGE(...) ((void)0)
#endif

#endif  // LOOPER_LOG_H
