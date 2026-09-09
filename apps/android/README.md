# Offline Interpreter — Android app

A complete two-way interpreter built on the engine in this repository. Use it as-is, or as a
worked example of how the library is meant to be driven.

## Build

```
cd apps/android
./gradlew assembleDebug
```

The first build downloads `offline-translator-<version>.aar` from the GitHub release into
`app/libs/`, so no NDK and no C++ toolchain are needed. Install with:

```
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

To build the engine from source instead (needs the Android NDK, roughly half an hour):

```
./gradlew assembleDebug -PuseLocalEngine=true
```

Prebuilt APKs are attached to the [releases](https://github.com/Kevin-KIM98/offline-translator/releases)
and to every run of the *Android app* workflow.

Requirements: Android 8.0 (API 26) or newer, arm64 device. Android Studio opens the
`apps/android` folder directly.

## What the app does

| screen | behaviour |
|---|---|
| first run | lists exactly the models the chosen language pair needs, with sizes, then downloads them with progress, resume after interruption and a free-space check |
| conversation | one talk button per speaker, held down while speaking; the transcript shows the original quietly and the translation large, with the route and latency, and every line can be replayed |
| hands-free | keeps the microphone open and routes each utterance by the language whisper detected, so both people can just talk |
| keyboard | type a sentence instead of speaking it |
| settings | languages, speak-aloud and speed, translation backend, installed models with sizes and delete, engine build info |

The interface is English by default and Korean on a Korean device; language names in the
picker always appear in their own script.

Nothing leaves the device. The only network use is the one-time model download.

## Structure

```
MainActivity.kt      permission, keep-awake, lifecycle → pauses the mic when backgrounded
MainViewModel.kt     the whole state machine: models → session → conversation
Prefs.kt             what is remembered between launches
Languages.kt         the seven languages and the two conversation sides
ui/InterpreterApp.kt phase routing (checking / setup / downloading / ready / failed)
ui/SetupScreen.kt    first-run download
ui/ConversationScreen.kt  main screen
ui/SettingsScreen.kt settings and model management
ui/Components.kt     talk button, level meter, language picker
ui/theme/Theme.kt    palette and type scale
```

The app never calls the native layer directly; everything goes through `TranslatorSession`
and `ModelRepository` from the library.
