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
            url: "https://github.com/Kevin-KIM98/offline-translator/releases/download/v0.3.5/OfflineTranslatorCore.xcframework.zip",
            checksum: "0fbdc0044022121432dae1323d6923421bb7f16a1e839ac42be77c2f80ca8228"
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
