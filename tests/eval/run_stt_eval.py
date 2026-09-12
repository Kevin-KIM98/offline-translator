#!/usr/bin/env python3
"""Measure speech recognition per language over the synthesised clips.

    python tests/eval/stt_synthesize.py
    python tests/eval/run_stt_eval.py [--configs app,app_denoised,oneshot,noprompt,fullctx,greedy,auto]

Configurations (each is one translator_cli invocation per clip, so whisper loads every time):
  app       the app's path: `speech --stream` through RNNoise (voice activity and level), the
            utterance segmenter, language given, per-language prompt and transcript cleaning;
            whisper hears the microphone audio
  app_denoised  the same, but whisper hears RNNoise's output (the behaviour before 0.3.9)
  oneshot   `transcribe --lang L` on the whole clip (no segmenter)
  noprompt  oneshot without the per-language style prompt
  fullctx   oneshot with whisper's full 30 s encoder window instead of the adaptive one
  greedy    oneshot with beam size 1
  auto      oneshot with `--lang auto`: measures language identification

Scores: CER = character error rate after normalisation (NFC, lower case, punctuation and spaces
removed, number words mapped to digits on both sides); WER additionally for the space-delimited
languages. Lower is better; 0 is a perfect transcript.
"""
import argparse
import json
import pathlib
import re
import statistics
import subprocess
import sys
import unicodedata

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent

NUMBER_WORDS = {
    "ko": {"한": "1", "두": "2", "세": "3", "네": "4", "다섯": "5", "여섯": "6", "일곱": "7", "여덟": "8", "아홉": "9", "열": "10"},
    "en": {"one": "1", "two": "2", "three": "3", "four": "4", "five": "5", "six": "6", "seven": "7", "eight": "8", "nine": "9", "ten": "10"},
    "es": {"una": "1", "uno": "1", "dos": "2", "tres": "3", "cuatro": "4", "cinco": "5", "seis": "6", "siete": "7", "ocho": "8", "nueve": "9", "diez": "10"},
    "vi": {"một": "1", "hai": "2", "ba": "3", "bốn": "4", "năm": "5", "sáu": "6", "bảy": "7", "tám": "8", "chín": "9", "mười": "10"},
    "th": {"หนึ่ง": "1", "สอง": "2", "สาม": "3", "สี่": "4", "ห้า": "5", "หก": "6", "เจ็ด": "7", "แปด": "8", "เก้า": "9", "สิบ": "10"},
    "ja": {"一": "1", "二": "2", "三": "3", "四": "4", "五": "5", "六": "6", "七": "7", "八": "8", "九": "9", "十": "10"},
    "zh": {"一": "1", "两": "2", "二": "2", "三": "3", "四": "4", "五": "5", "六": "6", "七": "7", "八": "8", "九": "9", "十": "10"},
}
SPACED = {"en", "es", "vi"}
PUNCT = re.compile(r"[\s\.\,\?\!\"'“”‘’…:;¿¡。、，？！「」『』（）()\[\]【】\-–—~]")


def normalise(text: str, lang: str) -> str:
    t = unicodedata.normalize("NFC", text).lower()
    if lang in SPACED:
        words = [NUMBER_WORDS[lang].get(w, w) for w in re.split(r"\s+", PUNCT.sub(" ", t)) if w]
        return " ".join(words)
    for word, digit in NUMBER_WORDS[lang].items():
        t = t.replace(word, digit)
    return PUNCT.sub("", t)


def edit_distance(a, b) -> int:
    prev = list(range(len(b) + 1))
    for i, x in enumerate(a, start=1):
        cur = [i]
        for j, y in enumerate(b, start=1):
            cur.append(min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (x != y)))
        prev = cur
    return prev[-1]


def cer(ref: str, hyp: str, lang: str) -> float:
    r, h = normalise(ref, lang), normalise(hyp, lang)
    if lang in SPACED:
        r, h = r.replace(" ", ""), h.replace(" ", "")
    return edit_distance(r, h) / max(1, len(r))


def wer(ref: str, hyp: str, lang: str) -> float:
    r, h = normalise(ref, lang).split(), normalise(hyp, lang).split()
    return edit_distance(r, h) / max(1, len(r))


WHISPER = []   # extra CLI arguments, e.g. ["--whisper", path] to test another whisper model


def run(cli, models, config, wav, lang):
    """Returns (text, detected_lang, stt_ms)."""
    if config in ("app", "app_denoised"):
        tgt = "ko" if lang == "en" else "en"
        # --no-llm: every pair here has a Marian model, and loading the LLM costs ~50 s per clip.
        cmd = [cli, "speech", "--models", str(models), "--wav", str(wav), "--src", lang, "--tgt", tgt, "--stream", "--no-llm"] + WHISPER
        if config == "app_denoised":
            cmd.append("--stt-denoised")
        out = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8").stdout
        texts, ms, det = [], 0.0, ""
        for block in re.findall(r"\{.*?\n\}", out, re.S):
            try:
                d = json.loads(block)
            except json.JSONDecodeError:
                continue
            if d.get("source_text"):
                texts.append(d["source_text"])
                det = d.get("source_lang", det)
                ms += d.get("timings", {}).get("stt_ms", 0.0)
        return " ".join(texts), det, ms
    cmd = [cli, "transcribe", "--models", str(models), "--wav", str(wav),
           "--lang", "auto" if config == "auto" else lang] + WHISPER
    if config == "noprompt":
        cmd.append("--no-default-prompt")
    elif config == "fullctx":
        cmd.append("--full-audio-ctx")
    elif config == "greedy":
        cmd += ["--stt-beam", "1"]
    out = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8").stdout
    m = re.search(r"^\[(\w+)\] (.*)\n\((\d+) ms\)", out, re.S | re.M)
    if not m:
        return "", "", 0.0
    return m.group(2).strip(), m.group(1), float(m.group(3))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--set", default=str(HERE / "stt_sentences.json"))
    ap.add_argument("--audio", default=str(HERE / "out" / "stt_audio"))
    ap.add_argument("--out", default=str(HERE / "out" / "stt"))
    ap.add_argument("--cli", default=str(ROOT / "build-full" / "Release" / "translator_cli.exe"))
    ap.add_argument("--models", default=str(ROOT / "models" / "release"))
    ap.add_argument("--configs", default="app,app_denoised,oneshot,noprompt,fullctx,greedy,auto")
    ap.add_argument("--langs")
    ap.add_argument("--whisper", help="ggml whisper model file to test instead of the manifest's (e.g. medium)")
    args = ap.parse_args()
    if args.whisper:
        WHISPER[:] = ["--whisper", args.whisper]

    spec = json.load(open(args.set, encoding="utf-8"))
    audio = pathlib.Path(args.audio)
    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    langs = args.langs.split(",") if args.langs else list(spec["languages"])
    configs = args.configs.split(",")

    summary = {}
    for config in configs:
        rows = []
        for lang in langs:
            for vi in range(len(spec["voices"][lang])):
                for si, ref in enumerate(spec["languages"][lang], start=1):
                    wav = audio / f"{lang}_{si:02d}_v{vi}.wav"
                    if not wav.exists():
                        continue
                    text, det, ms = run(args.cli, args.models, config, wav, lang)
                    rows.append(dict(lang=lang, voice=vi, id=si, ref=ref, hyp=text, detected=det, ms=ms,
                                     cer=cer(ref, text, lang), wer=wer(ref, text, lang) if lang in SPACED else None))
                    print(f"[{config}] {lang} v{vi} {si:02d}  cer {rows[-1]['cer']:.2f}  {ms:5.0f} ms  {text}", flush=True)
        json.dump(rows, open(out / f"{config}.json", "w", encoding="utf-8"), ensure_ascii=False, indent=1)
        per_lang = {}
        for lang in langs:
            lr = [r for r in rows if r["lang"] == lang]
            if not lr:
                continue
            per_lang[lang] = dict(
                cer=statistics.mean(r["cer"] for r in lr) * 100,
                wer=(statistics.mean(r["wer"] for r in lr) * 100) if lang in SPACED else None,
                exact=sum(1 for r in lr if r["cer"] == 0),
                n=len(lr),
                detect=sum(1 for r in lr if r["detected"] == lang),
                ms=statistics.median(r["ms"] for r in lr),
            )
        summary[config] = per_lang

    print("\n" + "=" * 78)
    for config, per_lang in summary.items():
        print(f"\n{config}")
        print(f"  {'lang':4} {'CER%':>6} {'WER%':>6} {'exact':>7} {'lang-id':>8} {'median ms':>10}")
        for lang, s in per_lang.items():
            w = f"{s['wer']:6.1f}" if s["wer"] is not None else "     -"
            print(f"  {lang:4} {s['cer']:6.1f} {w} {s['exact']:>3}/{s['n']:<3} {s['detect']:>3}/{s['n']:<3} {s['ms']:10.0f}")
    json.dump(summary, open(out / "summary.json", "w", encoding="utf-8"), indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
