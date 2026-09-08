// Android library module wrapping the native engine.
// Add to settings.gradle.kts of your app:  include(":offline-translator"); project(":offline-translator").projectDir = file("../translator/platform/android")
plugins {
    id("com.android.library")
    id("org.jetbrains.kotlin.android")
}

base {
    archivesName.set("offline-translator")
}

android {
    namespace = "com.offlinetranslator"
    compileSdk = 35
    ndkVersion = "27.2.12479018"

    defaultConfig {
        minSdk = 26 // std::filesystem needs NDK r22+ / API 26+ for reliable support
        consumerProguardFiles("consumer-rules.pro")

        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DANDROID_STL=c++_shared",
                    "-DANDROID_ARM_NEON=ON",
                    "-DTRANSLATOR_VULKAN=OFF",   // set ON for Adreno/Mali GPU offload (needs Vulkan NDK headers)
                    "-DTRANSLATOR_STRICT_DEPS=ON",
                    "-DCMAKE_POLICY_VERSION_MINIMUM=3.5",
                )
                cppFlags += "-O3"
            }
        }
        ndk {
            abiFilters += listOf("arm64-v8a") // add "x86_64" for emulator builds
        }
    }

    externalNativeBuild {
        cmake {
            path = file("CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildTypes {
        release { isMinifyEnabled = false }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }

    packaging {
        jniLibs.useLegacyPackaging = false
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.8.1")
}
