plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose")
    id("com.google.devtools.ksp")
}

android {
    namespace = "com.audio.loopstation"
    compileSdk = 35

    // NDK r27 builds native code (and ships libc++_shared.so) 16 KB-aligned by
    // default. This is AGP 8.7.3's OWN default NDK revision, so it is already
    // present in any environment that has built this project — pinning a newer
    // revision would force a fresh SDK download (and fail offline/CI) for no
    // alignment benefit. Any r27+/r28 revision works if you prefer to bump it.
    ndkVersion = "27.0.12077973"

    defaultConfig {
        applicationId = "com.audio.loopstation"
        // 27: stable AAudio (the Oboe low-latency backend) + AudioFocusRequest.
        minSdk = 27
        targetSdk = 35
        versionCode = 1
        versionName = "1.0.0"

        ndk {
            // arm64 covers every modern phone/tablet (Tab S9 Ultra included);
            // x86_64 keeps the emulator usable.
            abiFilters += listOf("arm64-v8a", "x86_64")
        }

        externalNativeBuild {
            cmake {
                // Oboe's Prefab package is built against the shared C++ runtime,
                // so the app's native code must use it too (default is static,
                // which fails with CXX1212). Canonical Oboe-docs form.
                arguments("-DANDROID_STL=c++_shared")
                cppFlags("-std=c++17")
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildFeatures {
        compose = true
        prefab = true  // imports the Oboe native package for CMake's find_package(oboe)
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro",
            )
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
}

ksp {
    arg("room.schemaLocation", "$projectDir/schemas")  // schema history in VCS
}

dependencies {
    val composeBom = platform("androidx.compose:compose-bom:2024.10.01")
    implementation(composeBom)

    implementation("androidx.core:core-ktx:1.15.0")
    implementation("androidx.activity:activity-compose:1.9.3")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.material3:material3-window-size-class")
    implementation("androidx.compose.material:material-icons-core")
    implementation("androidx.lifecycle:lifecycle-runtime-compose:2.8.7")
    implementation("androidx.lifecycle:lifecycle-viewmodel-compose:2.8.7")

    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.9.0")

    implementation("androidx.room:room-runtime:2.6.1")
    implementation("androidx.room:room-ktx:2.6.1")
    ksp("androidx.room:room-compiler:2.6.1")

    // Native audio: Oboe via Prefab (consumed by CMake as oboe::oboe).
    // 1.9.1+ ships a 16 KB page-aligned liboboe.so (1.9.0 was not aligned).
    // NOTE: this bump only clears the Play Store's 16 KB advisory — it is not
    // needed to run on 4 KB-page devices such as the Galaxy Tab S9 Ultra. If
    // this version cannot be resolved, "1.9.0" is a safe fallback.
    implementation("com.google.oboe:oboe:1.9.3")
}
