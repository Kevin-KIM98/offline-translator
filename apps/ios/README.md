# Offline Interpreter — iOS app

The same two-way interpreter as the Android app, in SwiftUI, on the same engine.

## Build

The Xcode project is generated from `project.yml`, so no `.pbxproj` is checked in.

```
brew install xcodegen
cd apps/ios
xcodegen generate
open OfflineInterpreter.xcodeproj
```

Pick your development team in *Signing & Capabilities* and run on a device. The engine comes
from the Swift package at the repository root, which pulls the prebuilt XCFramework from the
matching GitHub release.

Requirements: iOS 15 or newer, Xcode 15 or newer. The simulator works for the UI, but speech
recognition is much slower there than on a real device.

## What the app does

| screen | behaviour |
|---|---|
| first run | lists exactly the models the chosen language pair needs, with sizes, then downloads them with progress, resume after interruption and a free-space check |
| conversation | one talk button per speaker, held down while speaking; the transcript shows the original quietly and the translation large, with the route and latency, and every line can be replayed |
| hands-free | keeps the microphone open and routes each utterance by the language whisper detected |
| keyboard | type a sentence instead of speaking it |
| settings | languages, speak-aloud and speed, translation backend, installed models with sizes and delete, engine build info |

Nothing leaves the device. The only network use is the one-time model download.

## Structure

```
App.swift             scene, keep-awake, mic pause/resume on scene phase
AppModel.swift        the whole state machine: models → session → conversation
RootView.swift        phase routing (checking / setup / downloading / ready / failed)
SetupView.swift       first-run download
ConversationView.swift  main screen
SettingsView.swift    settings and model management
Components.swift      talk button, level meter, language picker
Theme.swift           palette and byte formatting
```

The app never calls the native layer directly; everything goes through `TranslatorSession`
and `ModelDownloader` from the package.

## Voices

Translations are spoken with the system voice for the target language. If a language sounds
robotic or stays silent, install its voice in *Settings → Accessibility → Spoken Content →
Voices*. Thai and Vietnamese are not installed by default on most devices.
