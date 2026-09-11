#!/usr/bin/env python3
"""Synthesise the speech-recognition test clips: one WAV per sentence and voice.

    python tests/eval/stt_synthesize.py [--out tests/eval/out/stt_audio]

Uses Microsoft Edge's neural voices through the edge-tts package (an online service; only the
test sentences are sent) because this machine has offline voices for Korean and English only.
Each clip is converted to 16 kHz mono PCM16 with ffmpeg. A female and a male voice per language
so a result does not hinge on one speaker. Existing clips are kept, so the script can resume.
"""
import argparse
import asyncio
import json
import pathlib
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent


async def synth(text: str, voice: str, mp3: pathlib.Path) -> None:
    import edge_tts
    await edge_tts.Communicate(text, voice).save(str(mp3))


def to_wav(mp3: pathlib.Path, wav: pathlib.Path) -> None:
    # One second of silence before and after, as a microphone delivers it. The streaming path
    # needs the runway: without it the segmenter's pre-roll and RNNoise's voice probability start
    # inside the first word and the app configuration lost sentence beginnings.
    subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-i", str(mp3), "-af", "adelay=1000,apad=pad_dur=1",
                    "-ac", "1", "-ar", "16000", "-sample_fmt", "s16", str(wav)], check=True)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--set", default=str(HERE / "stt_sentences.json"))
    ap.add_argument("--out", default=str(HERE / "out" / "stt_audio"))
    ap.add_argument("--langs", help="comma-separated subset")
    args = ap.parse_args()

    spec = json.load(open(args.set, encoding="utf-8"))
    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    langs = args.langs.split(",") if args.langs else list(spec["languages"])

    made = kept = failed = 0
    for lang in langs:
        for vi, voice in enumerate(spec["voices"][lang]):
            for si, text in enumerate(spec["languages"][lang], start=1):
                wav = out / f"{lang}_{si:02d}_v{vi}.wav"
                if wav.exists() and wav.stat().st_size > 1000:
                    kept += 1
                    continue
                mp3 = wav.with_suffix(".mp3")
                try:
                    asyncio.run(synth(text, voice, mp3))
                    to_wav(mp3, wav)
                    mp3.unlink(missing_ok=True)
                    made += 1
                    print(f"  {wav.name}  {voice}", flush=True)
                except Exception as exc:  # noqa: BLE001
                    failed += 1
                    print(f"  FAILED {wav.name} ({voice}): {exc}", flush=True)
    print(f"synthesised {made}, kept {kept}, failed {failed} -> {out}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
