#!/usr/bin/env python3
"""Recognition under noise, through the app's streaming path.

    python tests/eval/stt_noise.py [--clips ko_02_v0,en_02_v0] [--snr 20,15,10,5]

Mixes pink noise (close to the spectrum of a crowd or a PA system) into a synthesised clip at
the given signal-to-noise ratios (speech RMS over noise RMS, speech measured on the voiced
part), then runs the app's path (`speech --stream`) twice per mix: whisper hearing the
microphone audio (the default since 0.3.9) and hearing RNNoise's output (`--stt-denoised`).
Prints both transcripts with their character error rate, so a change to the front end can be
checked in both directions: clean speech must not lose words, noisy speech must not get worse.
The noise also covers the silent second before the speech, so the segmenter's noise floor
tracks it the way it would in a room.
"""
import argparse
import pathlib
import sys
import wave

import numpy as np

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from run_stt_eval import ROOT, cer, run  # noqa: E402


def read_wav(path: pathlib.Path) -> np.ndarray:
    with wave.open(str(path), "rb") as w:
        assert w.getframerate() == 16000 and w.getnchannels() == 1 and w.getsampwidth() == 2
        return np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float32) / 32768.0


def write_wav(path: pathlib.Path, pcm: np.ndarray) -> None:
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(16000)
        w.writeframes((np.clip(pcm, -1.0, 1.0) * 32767.0).astype(np.int16).tobytes())


def pink_noise(n: int, seed: int) -> np.ndarray:
    """White noise shaped to 1/f in the frequency domain, unit RMS."""
    rng = np.random.default_rng(seed)
    spectrum = np.fft.rfft(rng.standard_normal(n))
    f = np.fft.rfftfreq(n, d=1.0 / 16000)
    spectrum[1:] /= np.sqrt(f[1:])
    spectrum[0] = 0.0
    noise = np.fft.irfft(spectrum, n)
    return noise / np.sqrt(np.mean(noise**2))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--clips", default="ko_02_v0,en_02_v0,ja_02_v0,es_02_v0")
    ap.add_argument("--snr", default="clean,20,15,10,5")
    ap.add_argument("--audio", default=str(HERE / "out" / "stt_audio"))
    ap.add_argument("--out", default=str(HERE / "out" / "stt_noise"))
    ap.add_argument("--cli", default=str(ROOT / "build-full" / "Release" / "translator_cli.exe"))
    ap.add_argument("--models", default=str(ROOT / "models" / "release"))
    ap.add_argument("--set", default=str(HERE / "stt_sentences.json"))
    args = ap.parse_args()

    import json
    spec = json.load(open(args.set, encoding="utf-8"))
    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    for clip in args.clips.split(","):
        lang, idx, _ = clip.split("_")
        ref = spec["languages"][lang][int(idx) - 1]
        speech = read_wav(pathlib.Path(args.audio) / f"{clip}.wav")
        voiced = speech[16000:-16000] if len(speech) > 40000 else speech
        speech_rms = float(np.sqrt(np.mean(voiced**2)))
        print(f"\n{clip}  ref: {ref}")
        print(f"  {'SNR':>5}  {'whisper hears':<14} {'CER':>5}  transcript")
        for snr in args.snr.split(","):
            if snr == "clean":
                mixed, wav = speech, pathlib.Path(args.audio) / f"{clip}.wav"
            else:
                noise = pink_noise(len(speech), seed=1) * speech_rms / (10 ** (float(snr) / 20))
                mixed = speech + noise
                wav = out / f"{clip}_snr{snr}.wav"
                write_wav(wav, mixed)
            for config, label in (("app", "microphone"), ("app_denoised", "RNNoise")):
                text, _, _ = run(args.cli, args.models, config, wav, lang)
                print(f"  {snr:>5}  {label:<14} {cer(ref, text, lang):5.2f}  {text}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
