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

# RNNoise ≥ 0.2 ships its weights separately.
if [[ -f "$TP/rnnoise/model_version" && ! -f "$TP/rnnoise/src/rnnoise_data.c" ]]; then
    echo ">> downloading RNNoise model weights"
    (
        cd "$TP/rnnoise"
        model="rnnoise_data-$(cat model_version).tar.gz"
        if command -v curl >/dev/null; then curl -sSL -o "$model" "https://media.xiph.org/rnnoise/models/$model"
        else wget -q -O "$model" "https://media.xiph.org/rnnoise/models/$model"; fi
        tar xzf "$model"
    )
fi

echo
echo "third_party/ ready:"
ls -1 "$TP"
