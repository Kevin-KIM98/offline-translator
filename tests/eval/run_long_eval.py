#!/usr/bin/env python3
"""Long-form speech through the app's streaming path.

    python tests/eval/run_long_eval.py [--langs ko,en] [--whisper <ggml file>]

Feeds each 40 s monologue of long_speech.json (synthesised by the script itself with edge-tts when
the clip is missing) through `speech --stream`, the path the app uses, and reports how the
segmenter cut it, the STT time of each piece, and the character error rate of all pieces joined
against the reference text. A monologue longer than the segmenter's maximum utterance has to be
cut somewhere; the question is whether the cuts cost words.
"""
import argparse
import asyncio
import json
import pathlib
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from run_stt_eval import ROOT, cer  # noqa: E402


def synth(text: str, voice: str, wav: pathlib.Path) -> None:
    import edge_tts
    mp3 = wav.with_suffix(".mp3")
    asyncio.run(edge_tts.Communicate(text, voice).save(str(mp3)))
    subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-i", str(mp3), "-af", "adelay=1000,apad=pad_dur=1",
                    "-ac", "1", "-ar", "16000", "-sample_fmt", "s16", str(wav)], check=True)
    mp3.unlink(missing_ok=True)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--set", default=str(HERE / "long_speech.json"))
    ap.add_argument("--audio", default=str(HERE / "out" / "long"))
    ap.add_argument("--cli", default=str(ROOT / "build-full" / "Release" / "translator_cli.exe"))
    ap.add_argument("--models", default=str(ROOT / "models" / "release"))
    ap.add_argument("--langs", default="ko,en")
    ap.add_argument("--whisper")
    ap.add_argument("--stt-denoised", action="store_true")
    args = ap.parse_args()

    spec = json.load(open(args.set, encoding="utf-8"))
    audio = pathlib.Path(args.audio)
    audio.mkdir(parents=True, exist_ok=True)
    for lang in args.langs.split(","):
        ref = spec[lang]
        wav = audio / f"{lang}.wav"
        if not wav.exists():
            synth(ref, spec["voices"][lang], wav)
        tgt = "ko" if lang == "en" else "en"
        cmd = [args.cli, "speech", "--models", args.models, "--wav", str(wav), "--src", lang, "--tgt", tgt,
               "--stream", "--no-llm"]
        if args.whisper:
            cmd += ["--whisper", args.whisper]
        if args.stt_denoised:
            cmd.append("--stt-denoised")
        out = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8").stdout
        pieces = []
        for block in out.split("--- utterance")[1:]:
            body = block[block.find("{"):block.rfind("}") + 1]
            try:
                d = json.loads(body)
            except json.JSONDecodeError:
                continue
            pieces.append(d)
        joined = " ".join(p.get("source_text", "") for p in pieces)
        print(f"\n[{lang}] {len(pieces)} piece(s), CER {cer(ref, joined, lang) * 100:.1f}%")
        for i, p in enumerate(pieces, 1):
            print(f"  {i:2d}. {p.get('timings', {}).get('stt_ms', 0):5.0f} ms  {p.get('source_text', '')}")
        print(f"  ref: {ref}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
