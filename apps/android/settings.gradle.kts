// Standalone Android app that uses the offline translation engine.
//
//   cd apps/android && ./gradlew assembleDebug
//
// By default the app links the prebuilt engine AAR from the GitHub release (downloaded on first
// build into app/libs/). To build the engine from source instead — needs the NDK and ~30 min:
//
//   ./gradlew assembleDebug -PuseLocalEngine=true
pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
    plugins {
        id("com.android.application") version "8.5.2"
        id("com.android.library") version "8.5.2"
        id("org.jetbrains.kotlin.android") version "2.0.20"
        id("org.jetbrains.kotlin.plugin.compose") version "2.0.20"
    }
}

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.PREFER_SETTINGS)
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "offline-interpreter"
include(":app")

if (providers.gradleProperty("useLocalEngine").orNull == "true") {
    include(":engine")
    project(":engine").projectDir = file("../../platform/android")
}
