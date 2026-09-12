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

That compiles the native code with the debug variant's settings. For a build whose speed you
want to judge on a phone, build the AAR in `platform/android` with `assembleRelease`, copy it to
`app/libs/offline-translator-<version>.aar` and build the app normally, which is what the workflow
does.

## Install and update on the phone

Every push to any branch that touches the app or the engine produces a development build at one
fixed address. Open it on the phone and install:

```
https://github.com/Kevin-KIM98/offline-translator/releases/download/dev-build/offline-interpreter-dev.apk
```

- **It updates in place.** Every APK is signed with `app/debug.keystore` and takes the workflow
  run number as its `versionCode`, so it installs over the previous build and keeps the downloaded
  models and settings. Do not replace that keystore: a different key forces an uninstall, which
  deletes the models.
- **The first time needs one uninstall.** Builds before development build 14 were signed with a
  per-machine key and cannot be updated in place.
- **Engine changes are included.** When a push changes the engine, the workflow builds the engine
  AAR from that commit before the APK, which takes longer than an app-only change.
- **The build is identified.** The release page and the app's version name, such as
  `1.0.0-dev.14+3678e6e`, give the build number and commit.

Release APKs are attached to the [releases](https://github.com/Kevin-KIM98/offline-translator/releases)
with `gh workflow run android-app.yml -f release_tag=vX.Y.Z`. The key's password is Android's
public debug default: it keeps development builds updatable and is not meant for a store release.

Requirements: Android 8.0 (API 26) or newer, arm64 device. Android Studio opens the
`apps/android` folder directly.

## What the app does

| screen | behaviour |
|---|---|
| first run | lists exactly the models the chosen language pair needs, with sizes, then downloads them with progress, resume after interruption and a free-space check |
| conversation | one talk button per speaker, held down while speaking; the transcript shows the original quietly and the translation large, with the route and latency, and every line can be replayed |
| hands-free | keeps the microphone open and routes each utterance by the language whisper detected, so both people can just talk |
| keyboard | type a sentence instead of speaking it |
| settings | languages, speak-aloud and speed, translation backend, which speech model (Whisper small or medium, downloaded when chosen) and which LLM to use (Qwen2.5 1.5B or 3B, downloaded when the languages need one), installed models with sizes and delete, engine build info |

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
