#!/usr/bin/env bash
# Builds a static XCFramework (device arm64 + simulator arm64) of the engine for Xcode.
# Requires macOS + Xcode 15+. CoreML + Metal are enabled by default.
#
#   scripts/build_ios.sh            # → dist/ios/OfflineTranslatorCore.xcframework
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIST="$ROOT/dist/ios"
DEPLOYMENT_TARGET="${IOS_DEPLOYMENT_TARGET:-15.0}"

build() { # sdk sysroot-name output-dir
    local sdk="$1" name="$2" build="$ROOT/build-ios-$2"
    cmake -S "$ROOT" -B "$build" -G Xcode \
        -DCMAKE_SYSTEM_NAME=iOS \
        -DCMAKE_OSX_SYSROOT="$sdk" \
        -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET" \
        -DCMAKE_XCODE_ATTRIBUTE_ONLY_ACTIVE_ARCH=NO \
        -DTRANSLATOR_BUILD_SHARED=OFF \
        -DTRANSLATOR_STRICT_DEPS=ON \
        -DTRANSLATOR_COREML=ON \
        -DTRANSLATOR_METAL=ON
    cmake --build "$build" --config Release --target offline_translator -- -quiet
    # Merge the core + every static third-party archive into a single libtool output.
    local libs
    libs=$(find "$build" -path "*Release-$sdk*" -name "*.a" | grep -v -E "tests|cli" || true)
    mkdir -p "$DIST/$name"
    libtool -static -o "$DIST/$name/libOfflineTranslatorCore.a" $libs
}

build iphoneos device
build iphonesimulator simulator

rm -rf "$DIST/OfflineTranslatorCore.xcframework"
xcodebuild -create-xcframework \
    -library "$DIST/device/libOfflineTranslatorCore.a"    -headers "$ROOT/include" \
    -library "$DIST/simulator/libOfflineTranslatorCore.a" -headers "$ROOT/include" \
    -output "$DIST/OfflineTranslatorCore.xcframework"

echo
echo "→ $DIST/OfflineTranslatorCore.xcframework"
echo "  Add it to the app target, plus platform/ios/OfflineTranslator/*.{h,mm,swift} and the"
echo "  bridging header. Link: Accelerate, CoreML, Metal, MetalKit, AVFoundation, Foundation."
echo "  For CoreML acceleration also ship the whisper *-encoder.mlmodelc next to the ggml file"
echo "  (see docs/MODELS.md)."
