#!/usr/bin/env python3
"""
Prepare the model assets served to the apps and generate manifest.json.

    pip install "ctranslate2>=4.5" "transformers>=4.40" sentencepiece torch huggingface_hub

    # 1. STT: download a ggml whisper model (quantized q5_1 ≈ 190 MB for small)
    python scripts/prepare_models.py whisper --model small --quant q5_1 --out dist/models

    # 2. NMT: convert Helsinki-NLP OPUS-MT pairs to CTranslate2 INT8 (+ their SentencePiece)
    python scripts/prepare_models.py nmt --pairs ko-en en-ko ja-ko zh-ko es-ko --out dist/models

    # 3. Manifest: hash everything under dist/models and write manifest.json
    python scripts/prepare_models.py manifest --root dist/models --base-url https://assets.yourdomain.com/models

Upload dist/models/ as-is to your CDN; the manifest uses "files" mode (per-file SHA-256),
so no zip handling is needed on the devices.

Notes on language pairs: OPUS-MT has direct ko-en / en-ko / ja-ko(?) etc. Not every pair
exists — the engine pivots automatically through "ko" or "en" when a direct pair is missing
(see PipelineConfig.pivotLangs). Check https://huggingface.co/Helsinki-NLP for availability;
you can override any source with --hf-model PAIR=repo (e.g. --hf-model ja-ko=Helsinki-NLP/opus-mt-ja-ko).
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import urllib.request
from pathlib import Path

WHISPER_BASE = "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/"

DEFAULT_HF = {
    "ko-en": "Helsinki-NLP/opus-mt-ko-en",
    "en-ko": "Helsinki-NLP/opus-mt-tc-big-en-ko",
    "ja-ko": "Helsinki-NLP/opus-mt-ja-ko",   # may not exist → falls back to ja-en + en-ko pivot
    "zh-ko": "Helsinki-NLP/opus-mt-zh-ko",   # same
    "es-ko": "Helsinki-NLP/opus-mt-es-ko",   # same
    "ja-en": "Helsinki-NLP/opus-mt-ja-en",
    "en-ja": "Helsinki-NLP/opus-mt-en-jap",
    "zh-en": "Helsinki-NLP/opus-mt-zh-en",
    "en-zh": "Helsinki-NLP/opus-mt-en-zh",
    "es-en": "Helsinki-NLP/opus-mt-es-en",
    "en-es": "Helsinki-NLP/opus-mt-en-es",
}


def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def download(url: str, dest: Path) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    if dest.exists():
        print(f"  exists: {dest}")
        return
    print(f"  GET {url}")
    tmp = dest.with_suffix(dest.suffix + ".part")
    with urllib.request.urlopen(url) as r, open(tmp, "wb") as f:
        total = int(r.headers.get("Content-Length") or 0)
        done = 0
        while True:
            chunk = r.read(1 << 20)
            if not chunk:
                break
            f.write(chunk)
            done += len(chunk)
            if total:
                sys.stdout.write(f"\r  {done * 100 // total:3d}%")
                sys.stdout.flush()
    print()
    tmp.rename(dest)


# ---------------------------------------------------------------------------

def cmd_whisper(a: argparse.Namespace) -> None:
    name = f"ggml-{a.model}" + (f"-{a.quant}" if a.quant else "") + ".bin"
    out = Path(a.out) / "stt" / f"whisper-{a.model}{('-' + a.quant) if a.quant else ''}.bin"
    download(WHISPER_BASE + name, out)
    if a.coreml:
        # CoreML encoder archive (iOS NPU). Unzipped next to the ggml file on device.
        zip_name = f"ggml-{a.model}-encoder.mlmodelc.zip"
        download(WHISPER_BASE + zip_name, Path(a.out) / "stt" / zip_name)
    print(f"→ {out} ({out.stat().st_size / 1e6:.0f} MB)")


def cmd_nmt(a: argparse.Namespace) -> None:
    overrides = dict(kv.split("=", 1) for kv in (a.hf_model or []))
    root = Path(a.out) / "nmt"
    for pair in a.pairs:
        repo = overrides.get(pair) or DEFAULT_HF.get(pair) or f"Helsinki-NLP/opus-mt-{pair}"
        dest = root / pair
        if (dest / "model.bin").exists() and not a.force:
            print(f"  exists: {dest}")
            continue
        print(f"→ converting {repo} → {dest}")
        if dest.exists():
            shutil.rmtree(dest)
        cmd = [
            sys.executable, "-m", "ctranslate2.converters.transformers",
            "--model", repo, "--output_dir", str(dest),
            "--quantization", a.quant, "--copy_files", "source.spm", "target.spm", "tokenizer_config.json",
        ]
        try:
            subprocess.run(cmd, check=True)
        except subprocess.CalledProcessError as e:
            print(f"  !! conversion failed for {pair} ({repo}); the engine will pivot via ko/en if possible", file=sys.stderr)
            if dest.exists():
                shutil.rmtree(dest)
            if a.strict:
                raise e
            continue
        # Some multilingual OPUS-MT models need a target-language token on the source side.
        if a.source_prefix and pair in a.source_prefix:
            (dest / "pair.json").write_text(json.dumps({"source_prefix_token": a.source_prefix[pair]}, indent=2))
        print(f"  ok: {sum(p.stat().st_size for p in dest.iterdir()) / 1e6:.0f} MB")


def flat_name(kind: str, group: str, filename: str) -> str:
    """Asset name used when files are hosted flat (e.g. GitHub release assets)."""
    return f"{kind}_{group}_{filename}" if group else f"{kind}_{filename}"


def cmd_manifest(a: argparse.Namespace) -> None:
    root = Path(a.root)
    base = a.base_url.rstrip("/")
    manifest = {"manifest_version": a.version, "base_url": base + "/"}
    flat = a.flat
    only_files = {"model.bin", "config.json", "shared_vocabulary.json", "source_vocabulary.json",
                  "target_vocabulary.json", "source.spm", "target.spm", "pair.json"}

    def url(kind: str, group: str, filename: str) -> str:
        if flat:
            return flat_name(kind, group, filename)
        return f"{kind}/{group}/{filename}" if group else f"{kind}/{filename}"

    stt_dir = root / "stt"
    stt_files = sorted(stt_dir.glob(a.stt_glob)) if stt_dir.exists() else []
    if stt_files:
        f = stt_files[0]
        manifest["stt"] = {
            "id": f.stem,
            "version": a.model_version,
            "filename": f.name,
            "size_bytes": f.stat().st_size,
            "sha256": sha256_of(f),
            "download_url": url("stt", "", f.name),
        }
        print(f"stt: {f.name} ({f.stat().st_size / 1e6:.0f} MB)")

    nmt_root = root / "nmt"
    tok = nmt_root / "tokenizer"
    if tok.is_dir():
        files = []
        for p in sorted(tok.iterdir()):
            if p.is_file():
                files.append({"filename": p.name, "size_bytes": p.stat().st_size, "sha256": sha256_of(p),
                              "download_url": url("nmt", "tokenizer", p.name)})
        manifest["tokenizer"] = {"version": a.model_version, "dir_name": "tokenizer", "files": files}

    nmt = []
    if nmt_root.exists():
        for d in sorted(nmt_root.iterdir()):
            if not d.is_dir() or d.name == "tokenizer" or not (d / "model.bin").exists():
                continue
            files = []
            for p in sorted(d.iterdir()):
                if p.is_file() and p.name in only_files:
                    files.append({"filename": p.name, "size_bytes": p.stat().st_size, "sha256": sha256_of(p),
                                  "download_url": url("nmt", d.name, p.name)})
            nmt.append({"pair": d.name, "dir_name": d.name, "version": a.model_version, "files": files})
            print(f"nmt: {d.name} ({len(files)} files, {sum(x['size_bytes'] for x in files) / 1e6:.0f} MB)")
    manifest["nmt"] = nmt

    out = Path(a.out) if a.out else root / "manifest.json"
    out.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"→ {out}")

    if a.upload_script:
        # Emit a script that uploads every referenced file under its flat asset name. GitHub
        # names an asset after the local file (the `file#label` form only sets the label), so
        # the files are first copied into a flat/ directory under their asset names.
        flat_dir = Path(a.upload_script).parent / "flat"
        lines = ["#!/usr/bin/env bash", "set -euo pipefail", f"TAG=\"${{1:-{a.release_tag or 'models'}}}\"",
                 f"FLAT=\"{flat_dir.as_posix()}\"", "mkdir -p \"$FLAT\"",
                 "gh release view \"$TAG\" >/dev/null 2>&1 || gh release create \"$TAG\" --title \"$TAG\" --notes \"Model assets referenced by assets/manifest.json\"",
                 "up() { cp -f \"$1\" \"$FLAT/$2\"; gh release upload \"$TAG\" \"$FLAT/$2\" --clobber; echo \"uploaded $2\"; }"]
        count = 0
        if stt_files:
            f = stt_files[0]
            lines.append(f"up \"{f.as_posix()}\" \"{flat_name('stt', '', f.name)}\"")
            count += 1
        for entry in nmt:
            for fi in entry["files"]:
                lines.append(f"up \"{(nmt_root / entry['dir_name'] / fi['filename']).as_posix()}\" \"{flat_name('nmt', entry['dir_name'], fi['filename'])}\"")
                count += 1
        lines.append("echo \"all $((0)) done\"".replace("$((0))", str(count)))
        with open(a.upload_script, "w", encoding="utf-8", newline="\n") as fh:  # bash needs LF even on Windows
            fh.write("\n".join(lines) + "\n")
        print(f"→ {a.upload_script} (run it with the release tag to upload {count} assets)")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    w = sub.add_parser("whisper", help="download a ggml whisper model")
    w.add_argument("--model", default="small", help="tiny|base|small|medium|large-v3-turbo")
    w.add_argument("--quant", default="q5_1", help="q5_1|q8_0|'' (empty = fp16)")
    w.add_argument("--coreml", action="store_true", help="also fetch the CoreML encoder zip for iOS")
    w.add_argument("--out", default="dist/models")
    w.set_defaults(fn=cmd_whisper)

    n = sub.add_parser("nmt", help="convert OPUS-MT pairs to CTranslate2")
    n.add_argument("--pairs", nargs="+", default=["ko-en", "en-ko", "ja-ko", "zh-ko", "es-ko"])
    n.add_argument("--quant", default="int8", help="int8|int8_float16|float16")
    n.add_argument("--hf-model", nargs="*", help="override PAIR=hf_repo")
    n.add_argument("--source-prefix", type=json.loads, default=None, help='JSON {"en-mul": ">>kor<<"}')
    n.add_argument("--force", action="store_true")
    n.add_argument("--strict", action="store_true", help="fail on the first conversion error")
    n.add_argument("--out", default="dist/models")
    n.set_defaults(fn=cmd_nmt)

    m = sub.add_parser("manifest", help="hash a models tree and write manifest.json")
    m.add_argument("--root", default="dist/models")
    m.add_argument("--base-url", required=True)
    m.add_argument("--version", default="1.1.0", help="manifest_version")
    m.add_argument("--model-version", default="1", help="per-model version string")
    m.add_argument("--stt-glob", default="*.bin", help="which STT file to reference (e.g. 'whisper-small-*.bin')")
    m.add_argument("--flat", action="store_true", help="flat asset names (stt_x.bin, nmt_ko-en_model.bin) for GitHub releases")
    m.add_argument("--out", default=None, help="manifest output path (default <root>/manifest.json)")
    m.add_argument("--upload-script", default=None, help="also write a bash script that uploads assets with gh")
    m.add_argument("--release-tag", default="models", help="default tag used by the upload script")
    m.set_defaults(fn=cmd_manifest)

    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
