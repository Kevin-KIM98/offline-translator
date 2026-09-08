#!/usr/bin/env bash
# Standalone NDK build of liboffline_translator.so (outside Gradle), e.g. for CI or
# to drop the .so into an existing app's jniLibs/.
#
#   ANDROID_NDK=~/Android/Sdk/ndk/27.2.12479018 scripts/build_android.sh [arm64-v8a|x86_64] [--vulkan]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ABI="${1:-arm64-v8a}"
VULKAN=OFF
[[ "${2:-}" == "--vulkan" ]] && VULKAN=ON

: "${ANDROID_NDK:?set ANDROID_NDK to your NDK path (r25+)}"
API=26
BUILD="$ROOT/build-android-$ABI"

cmake -S "$ROOT" -B "$BUILD" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$ABI" \
    -DANDROID_PLATFORM="android-$API" \
    -DANDROID_STL=c++_shared \
    -DANDROID_ARM_NEON=ON \
    -DCMAKE_BUILD_TYPE=Release \
    -DTRANSLATOR_STRICT_DEPS=ON \
    -DTRANSLATOR_VULKAN="$VULKAN"

cmake --build "$BUILD" --target offline_translator -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu)"

OUT="$ROOT/dist/android/$ABI"
mkdir -p "$OUT"
cp "$BUILD/liboffline_translator.so" "$OUT/"
# Ship libc++_shared.so next to it (required with ANDROID_STL=c++_shared).
cp "$ANDROID_NDK/toolchains/llvm/prebuilt/"*/sysroot/usr/lib/$( [[ "$ABI" == "arm64-v8a" ]] && echo aarch64-linux-android || echo x86_64-linux-android )/libc++_shared.so "$OUT/" 2>/dev/null || true
echo "→ $OUT"
ls -la "$OUT"
