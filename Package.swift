// swift-tools-version:5.9
// Swift Package for iOS apps.
//
//   Xcode → File → Add Package Dependencies… → https://github.com/Kevin-KIM98/offline-translator
//   then `import OfflineTranslator`.
//
// The C++ engine ships as a prebuilt XCFramework attached to each GitHub release; the URL and
// checksum below are rewritten by .github/workflows/release.yml when a release is published.
import PackageDescription

let package = Package(
    name: "OfflineTranslator",
    platforms: [.iOS(.v15), .macOS(.v13)],
    products: [
        .library(name: "OfflineTranslator", targets: ["OfflineTranslator"]),
    ],
    targets: [
        // Prebuilt C++ core (whisper.cpp + CTranslate2 + SentencePiece + RNNoise), C ABI only.
        .binaryTarget(
            name: "OfflineTranslatorCore",
            url: "https://github.com/Kevin-KIM98/offline-translator/releases/download/v0.3.4/OfflineTranslatorCore.xcframework.zip",
            checksum: "0f514b613bf592fd59f2db2e5c29a6095a131445a8f632e542a31703a3c33e95"
        ),
        // Objective-C++ wrapper (OTTranslationPipeline / OTModelManager).
        .target(
            name: "OfflineTranslatorObjC",
            dependencies: ["OfflineTranslatorCore"],
            path: "platform/ios/OfflineTranslatorObjC",
            publicHeadersPath: "include",
            linkerSettings: [
                .linkedFramework("Accelerate"),
                .linkedFramework("CoreML"),
                .linkedFramework("Metal"),
                .linkedFramework("MetalKit"),
                .linkedFramework("Foundation"),
                .linkedLibrary("c++"),
            ]
        ),
        // Swift API: TranslatorSession, ModelDownloader, AudioCapture, OfflineTTSManager.
        .target(
            name: "OfflineTranslator",
            dependencies: ["OfflineTranslatorObjC"],
            path: "platform/ios/OfflineTranslator",
            linkerSettings: [.linkedFramework("AVFoundation")]
        ),
    ],
    cxxLanguageStandard: .cxx17
)
