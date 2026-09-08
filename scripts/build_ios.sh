#!/usr/bin/env bash
# Builds OfflineTranslatorCore.xcframework (device arm64 + simulator arm64) — the binary target
# consumed by Package.swift — and zips it for a GitHub release.
# Requires macOS + Xcode 15+. CoreML + Metal are enabled by default.
#
#   scripts/build_ios.sh            # → dist/ios/OfflineTranslatorCore.xcframework(.zip)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIST="$ROOT/dist/ios"
DEPLOYMENT_TARGET="${IOS_DEPLOYMENT_TARGET:-15.0}"
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"

build() { # sdk name
    local sdk="$1" name="$2" build="$ROOT/build-ios-$2"
    cmake -S "$ROOT" -B "$build" -G Xcode \
        -DCMAKE_SYSTEM_NAME=iOS \
        -DCMAKE_OSX_SYSROOT="$sdk" \
        -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET" \
        -DCMAKE_XCODE_ATTRIBUTE_ONLY_ACTIVE_ARCH=NO \
        -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO \
        -DTRANSLATOR_BUILD_SHARED=OFF \
        -DTRANSLATOR_STRICT_DEPS=ON \
        -DTRANSLATOR_COREML=ON \
        -DTRANSLATOR_METAL=ON
    cmake --build "$build" --config Release --target offline_translator --parallel "$JOBS" -- -quiet
    # Merge the core + every static third-party archive into one library.
    local libs
    libs=$(find "$build" -path "*Release-$sdk*" -name "*.a" | grep -v -E "translator_tests|translator_cli|translator_static" || true)
    mkdir -p "$DIST/$name"
    # shellcheck disable=SC2086
    libtool -static -o "$DIST/$name/libOfflineTranslatorCore.a" $libs
}

build iphoneos device
build iphonesimulator simulator

# Headers exposed to SwiftPM: the C ABI + a module map.
HDR="$DIST/headers"
rm -rf "$HDR" && mkdir -p "$HDR"
cp "$ROOT/include/translator_c_api.h" "$HDR/"
cp "$ROOT/cmake/ios/module.modulemap" "$HDR/"

rm -rf "$DIST/OfflineTranslatorCore.xcframework" "$DIST/OfflineTranslatorCore.xcframework.zip"
xcodebuild -create-xcframework \
    -library "$DIST/device/libOfflineTranslatorCore.a"    -headers "$HDR" \
    -library "$DIST/simulator/libOfflineTranslatorCore.a" -headers "$HDR" \
    -output "$DIST/OfflineTranslatorCore.xcframework"

(cd "$DIST" && ditto -c -k --keepParent OfflineTranslatorCore.xcframework OfflineTranslatorCore.xcframework.zip)
CHECKSUM=$(swift package compute-checksum "$DIST/OfflineTranslatorCore.xcframework.zip")
echo "$CHECKSUM" > "$DIST/OfflineTranslatorCore.xcframework.zip.checksum"

echo
echo "→ $DIST/OfflineTranslatorCore.xcframework.zip"
echo "  SwiftPM checksum: $CHECKSUM"
