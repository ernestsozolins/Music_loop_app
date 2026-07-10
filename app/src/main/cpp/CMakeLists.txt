# Phase 1: the engine core builds as a static library. Phase 2 adds the JNI
# bridge (looper_jni) and turns the tree into the loadable shared library the
# Kotlin side System.loadLibrary()s.
cmake_minimum_required(VERSION 3.22)
project(looper_engine LANGUAGES CXX)

add_library(looper_core STATIC AudioEngine.cpp)
target_compile_features(looper_core PUBLIC cxx_std_17)
target_compile_options(looper_core PRIVATE -Wall -Wextra -O2)
target_include_directories(looper_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Oboe via Prefab. The app module (Phase 2) declares:
#   implementation "com.google.oboe:oboe:1.9.0"
#   android { buildFeatures { prefab true } }
find_package(oboe REQUIRED CONFIG)
target_link_libraries(looper_core PUBLIC oboe::oboe)

if(ANDROID)
  target_link_libraries(looper_core PRIVATE android log)
endif()
