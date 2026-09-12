#!/usr/bin/env bash
# Clone the four engine dependencies at pinned versions into third_party/.
# Usage: scripts/fetch_third_party.sh [--shallow]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TP="$ROOT/third_party"
mkdir -p "$TP"

RNNOISE_REF="${RNNOISE_REF:-v0.2}"
WHISPER_REF="${WHISPER_REF:-v1.7.5}"
CT2_REF="${CT2_REF:-v4.5.0}"
SPM_REF="${SPM_REF:-v0.2.0}"
LLAMA_REF="${LLAMA_REF:-b5030}"   # same-week ggml as whisper.cpp v1.7.5
MINIAUDIO_REF="${MINIAUDIO_REF:-0.11.25}"  # single header, capture only, used by the CLI

DEPTH=""
[[ "${1:-}" == "--shallow" ]] && DEPTH="--depth 1"

clone() { # name url ref [recursive]
    local name="$1" url="$2" ref="$3" recursive="${4:-}"
    if [[ -d "$TP/$name/.git" ]]; then
        echo ">> $name already present (git -C third_party/$name checkout $ref to change)"
        return
    fi
    echo ">> cloning $name @ $ref"
    # shellcheck disable=SC2086
    git clone $DEPTH --branch "$ref" ${recursive:+--recursive --shallow-submodules} "$url" "$TP/$name"
}

clone rnnoise       https://github.com/xiph/rnnoise            "$RNNOISE_REF"
clone whisper.cpp   https://github.com/ggml-org/whisper.cpp    "$WHISPER_REF"
clone ctranslate2   https://github.com/OpenNMT/CTranslate2     "$CT2_REF" recursive
clone sentencepiece https://github.com/google/sentencepiece    "$SPM_REF"
clone llama.cpp     https://github.com/ggml-org/llama.cpp      "$LLAMA_REF"

# media.xiph.org (RNNoise weights) drops connections now and then; a CI run should not fail
# on one timeout. Retries cover connection and transfer errors alike.
CURL_RETRY="--retry 5 --retry-delay 15 --retry-all-errors --connect-timeout 30 --max-time 600"

# miniaudio is a single public-domain header; the desktop CLI uses it for `listen`.
if [[ ! -f "$TP/miniaudio/miniaudio.h" ]]; then
    echo ">> downloading miniaudio $MINIAUDIO_REF"
    mkdir -p "$TP/miniaudio"
    url="https://raw.githubusercontent.com/mackron/miniaudio/$MINIAUDIO_REF/miniaudio.h"
    if command -v curl >/dev/null; then curl -sSL $CURL_RETRY -o "$TP/miniaudio/miniaudio.h" "$url"
    else wget -q -O "$TP/miniaudio/miniaudio.h" "$url"; fi
fi

# RNNoise ≥ 0.2 ships its weights separately. media.xiph.org goes down for hours at a time, so a
# copy sits on this repository's "third-party" release; either source must match the recorded hash.
RNNOISE_DATA_SHA256="4ac81c5c0884ec4bd5907026aaae16209b7b76cd9d7f71af582094a2f98f4b43"   # rnnoise_data-0b50c45.tar.gz
if [[ -f "$TP/rnnoise/model_version" && ! -f "$TP/rnnoise/src/rnnoise_data.c" ]]; then
    echo ">> downloading RNNoise model weights"
    (
        cd "$TP/rnnoise"
        model="rnnoise_data-$(cat model_version).tar.gz"
        fetch() {   # url
            if command -v curl >/dev/null; then curl -sSL $CURL_RETRY -o "$model" "$1"
            else wget -q -O "$model" "$1"; fi
        }
        digest() {
            if command -v sha256sum >/dev/null; then sha256sum "$model" | cut -c1-64
            else shasum -a 256 "$model" | cut -c1-64; fi
        }
        ok=0
        for url in "https://github.com/Kevin-KIM98/offline-translator/releases/download/third-party/$model" \
                   "https://media.xiph.org/rnnoise/models/$model"; do
            rm -f "$model"
            if fetch "$url" && [[ "$(digest)" == "$RNNOISE_DATA_SHA256" ]]; then ok=1; break; fi
            echo "   $url: download failed or hash mismatch, trying the next source"
        done
        [[ $ok == 1 ]] || { echo "RNNoise weights: no source delivered $model"; exit 1; }
        tar xzf "$model"
    )
fi

echo
echo "third_party/ ready:"
ls -1 "$TP"
