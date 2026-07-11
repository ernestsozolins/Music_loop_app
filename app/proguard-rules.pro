# Native code resolves these by name; R8 must not strip or rename them.

# Engine events arrive via JNI GetMethodID("onNativeEvent", "(II)V").
-keepclassmembers class com.audio.loopstation.AudioEngine {
    void onNativeEvent(int, int);
}

# All native bridge methods (their JNI symbols encode the exact names).
-keepclasseswithmembers class com.audio.loopstation.AudioEngine {
    native <methods>;
}
