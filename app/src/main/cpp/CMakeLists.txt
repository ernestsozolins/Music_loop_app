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

# Oboe via Prefab. The app module declares:
#   implementation "com.google.oboe:oboe:1.9.0"
#   android { buildFeatures { prefab true } }
find_package(oboe REQUIRED CONFIG)
target_link_libraries(looperengine PRIVATE oboe::oboe)

if(ANDROID)
  target_link_libraries(looperengine PRIVATE android log)
endif()
