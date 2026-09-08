// Standalone build of the library module (used by CI to produce the AAR):
//   cd platform/android && gradle assembleRelease
// Apps can also include this directory as a module instead of using the prebuilt AAR.
pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
    plugins {
        id("com.android.library") version "8.5.2"
        id("org.jetbrains.kotlin.android") version "2.0.20"
    }
}

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.PREFER_SETTINGS)
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "offline-translator"
