# liblooperengine.so — the native library loaded by
# com.audio.loopstation.AudioEngine (System.loadLibrary("looperengine")).
cmake_minimum_required(VERSION 3.22)
project(looper_engine LANGUAGES CXX)

add_library(looperengine SHARED
    AudioEngine.cpp
    AudioEngine_JNI.cpp
    DiskSpooler.cpp
    Reverb.cpp)

target_compile_features(looperengine PUBLIC cxx_std_17)
target_compile_options(looperengine PRIVATE -Wall -Wextra -O2)
target_include_directories(looperengine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# 16 KB page-size compatibility (Android 15+ on 16 KB-page devices, and a Play
# Store requirement). Align this library's ELF LOAD segments to 16 KB so it
# loads on 16 KB-page kernels. NDK r27+ does this by default; the explicit flag
# keeps older NDKs correct too. (The prebuilt liboboe.so / libc++_shared.so are
# aligned by bumping the Oboe dependency and the NDK version — see build.gradle.)
target_link_options(looperengine PRIVATE "-Wl,-z,max-page-size=16384")

# Oboe via Prefab. The app module declares:
#   implementation "com.google.oboe:oboe:1.9.0"
#   android { buildFeatures { prefab true } }
find_package(oboe REQUIRED CONFIG)
target_link_libraries(looperengine PRIVATE oboe::oboe)

if(ANDROID)
  target_link_libraries(looperengine PRIVATE android log)
endif()
