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
            url: "https://github.com/Kevin-KIM98/offline-translator/releases/download/v0.2.0/OfflineTranslatorCore.xcframework.zip",
            checksum: "83062ab0f3978a661b53e098b0f629755c4902b18134c50fa5d7474f68fd20dc"
        ),
        // Objective-C++ wrapper (OTTranslationPipeline / OTModelManager).
        .target(
            name: "OfflineTranslatorObjC",
            dependencies: ["OfflineTranslatorCore"],
            path: "platform/ios/OfflineTranslatorObjC",
            publicHeadersPath: "include",
            cxxSettings: [.unsafeFlags(["-std=c++17"])],
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
