// SPDX-License-Identifier: MIT
//
// The whole app is one Activity, three custom Views and a C library built by
// the NDK.  No AndroidX, no Material, no Compose, no WebView, no Kotlin.
plugins {
    id("com.android.application")
}

android {
    namespace = "dev.rvm.app"
    compileSdk = 35

    // The NDK project lives in ../../jni so the same CMakeLists.txt can be
    // reused by a desktop cross-build.
    externalNativeBuild {
        cmake {
            path = file("../jni/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    defaultConfig {
        applicationId = "dev.rvm.app"
        minSdk = 26            // AAudio needs 26; that is also the audio floor
        targetSdk = 35
        versionCode = 8
        versionName = "0.2.2"

        // Only the ABIs people actually run RISC-V guests on.  Shipping one
        // instead of four is the single biggest APK size win available.
        ndk {
            abiFilters += listOf("arm64-v8a", "x86_64")
        }

        externalNativeBuild {
            cmake {
                // No STL: the core is C and links only libc/libm/libaaudio.
                arguments += listOf(
                    "-DANDROID_STL=none",
                    "-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON",
                )
                cFlags += listOf(
                    "-std=c11", "-O2", "-fvisibility=hidden",
                    "-Wall", "-Wextra", "-Wno-unused-parameter",
                )
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro",
            )
            signingConfig = signingConfigs.getByName("debug")
        }
        debug {
            isMinifyEnabled = false
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    packaging {
        // Keep the APK small: no debug metadata, no licences, no kotlin/
        resources.excludes += setOf(
            "META-INF/*.version", "META-INF/*.kotlin_module",
            "META-INF/LICENSE*", "META-INF/DEPENDENCIES",
            "Debug/*", "**/*.so.meta",
        )
        jniLibs.useLegacyPackaging = false
    }

    // The default launcher icon set drags in PNGs for six densities; we ship
    // one adaptive vector instead.
    androidResources.noCompress += listOf("dtb")
}

dependencies {
    // Intentionally empty.  Adding anything here costs APK size.
}
