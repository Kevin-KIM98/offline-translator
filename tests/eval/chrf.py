#!/usr/bin/env python3
"""Corpus chrF for Thai (or any script without word spaces), no dependencies.

    python tests/eval/chrf.py tests/eval/th_conversation.json results.json

`results.json` maps sentence id to hypothesis: {"1": "...", "2": "...", ...}.

chrF (Popović, 2015) scores character n-grams, so it needs no word segmentation and works for
Thai. Statistics are summed over the corpus per n-gram order, precision and recall are averaged
across orders, then combined with beta = 2 — the sacreBLEU definition, without its whitespace
handling since Thai does not rely on spaces.

Register is normalised first: polite particles and first-person pronouns vary legitimately between
speakers, and a reference that says ผม ... ครับ should not beat an output that says ฉัน ... ค่ะ.
The script also flags any letter outside Thai, which a translation into Thai should not contain.
"""
import json
import re
import sys
import unicodedata
from collections import Counter

MAX_N = 6
BETA = 2.0

# Longest first, so นะครับ is removed before ครับ.
PARTICLES = ["นะครับ", "นะคะ", "ครับผม", "ครับ", "ค่ะ", "คะ", "จ้ะ", "จ้า"]
PRONOUNS = ["ดิฉัน", "ฉัน", "ผม"]


def normalise(text: str) -> str:
    t = unicodedata.normalize("NFC", text)
    for p in PARTICLES:
        t = t.replace(p, "")
    for p in PRONOUNS:
        t = t.replace(p, "ผม")
    t = re.sub(r"[\s\.\,\?\!\"'“”‘’…:;]", "", t)
    return t


def ngrams(text: str, n: int) -> Counter:
    return Counter(text[i:i + n] for i in range(len(text) - n + 1))


def sentence_stats(hyp: str, ref: str):
    h, r = normalise(hyp), normalise(ref)
    stats = []
    for n in range(1, MAX_N + 1):
        hc, rc = ngrams(h, n), ngrams(r, n)
        stats.append((sum((hc & rc).values()), sum(hc.values()), sum(rc.values())))
    return stats


def chrf_from(stats) -> float:
    precisions, recalls = [], []
    for match, hyp_total, ref_total in stats:
        if hyp_total == 0 or ref_total == 0:
            continue
        precisions.append(match / hyp_total)
        recalls.append(match / ref_total)
    if not precisions:
        return 0.0
    p = sum(precisions) / len(precisions)
    r = sum(recalls) / len(recalls)
    if p + r == 0:
        return 0.0
    b2 = BETA * BETA
    return 100.0 * (1 + b2) * p * r / (b2 * p + r)


def foreign_letters(text: str) -> str:
    """Letters that are neither Thai nor Latin (names and units may legitimately stay Latin)."""
    out = []
    for ch in text:
        if not ch.isalpha():
            continue
        name = unicodedata.name(ch, "")
        if name.startswith("THAI") or name.startswith("LATIN"):
            continue
        out.append(ch)
    return "".join(out)


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    refs = {str(s["id"]): s for s in json.load(open(sys.argv[1], encoding="utf-8"))["sentences"]}
    hyps = json.load(open(sys.argv[2], encoding="utf-8"))

    totals = [[0, 0, 0] for _ in range(MAX_N)]
    rows = []
    for sid, ref in refs.items():
        hyp = hyps.get(sid, "")
        stats = sentence_stats(hyp, ref["th"])
        for i, (m, h, r) in enumerate(stats):
            totals[i][0] += m
            totals[i][1] += h
            totals[i][2] += r
        rows.append((sid, chrf_from(stats), foreign_letters(hyp), ref["ko"], hyp))

    for sid, score, foreign, ko, hyp in rows:
        flag = f"  FOREIGN[{foreign}]" if foreign else ""
        print(f"{sid:>3} {score:5.1f}  {ko}  =>  {hyp}{flag}")
    print(f"\ncorpus chrF {chrf_from([tuple(t) for t in totals]):.1f}   "
          f"sentences with foreign letters: {sum(1 for r in rows if r[2])}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
