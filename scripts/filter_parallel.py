#!/usr/bin/env python3
"""Keep only parallel sentences that translate back consistently.

    python scripts/filter_parallel.py --in data/train_opus.jsonl --out data/train_filtered.jsonl [--min-chrf 35]

Subtitle corpora such as opus-100 contain misaligned pairs ("How could you tell?" paired with a
Thai line that says "I have seen something like this before"). For every example whose target
language has a Marian model into English, the target side is translated back with the engine's
own model (translator_cli) and compared to the English source with chrF; pairs under the
threshold are dropped. Examples without such a model are kept as they are.
"""
import argparse
import json
import pathlib
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(ROOT / "tests" / "eval"))
import chrf  # noqa: E402


def back_translate(cli, models, lines, src):
    # One sentence per line: characters Python or the CLI would take for line breaks are spaced out.
    clean = [" ".join(l.replace("\r", " ").replace(" ", " ").replace(" ", " ").split()) or "." for l in lines]
    p = subprocess.run([cli, "translate", "--models", models, "--src", src, "--tgt", "en", "--no-llm", "--lines"],
                       input="\n".join(clean) + "\n", capture_output=True, text=True, encoding="utf-8", errors="replace")
    out = []
    for l in p.stdout.split("\n"):
        if not l.startswith("{"):
            continue
        try:
            out.append(json.loads(l).get("translated_text", ""))
        except json.JSONDecodeError:
            out.append("")   # an unreadable line scores 0 and is dropped
    if len(out) != len(lines):
        sys.stderr.write(p.stderr[-1000:])
        raise RuntimeError(f"{src}: expected {len(lines)} back-translations, got {len(out)}")
    return out


def sentence_chrf(hyp, ref):
    return chrf.chrf_from(chrf.sentence_stats(hyp, ref))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--in", dest="inp", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--cli", default=str(ROOT / "build-full" / "Release" / "translator_cli.exe"))
    ap.add_argument("--models", default=str(ROOT / "models" / "release"))
    ap.add_argument("--min-chrf", type=float, default=35.0)
    ap.add_argument("--batch", type=int, default=250)
    ap.add_argument("--workers", type=int, default=5)
    args = ap.parse_args()

    rows = [json.loads(l) for l in open(args.inp, encoding="utf-8") if l.strip()]
    by_tgt = {}
    for i, r in enumerate(rows):
        if r["src_lang"] == "en":
            by_tgt.setdefault(r["tgt_lang"], []).append(i)
    # Several CLI processes at once: on Windows each CTranslate2 instance is capped to one thread.
    from concurrent.futures import ThreadPoolExecutor
    jobs = []
    for tgt, idxs in by_tgt.items():
        for start in range(0, len(idxs), args.batch):
            jobs.append((tgt, idxs[start:start + args.batch]))

    def run_job(job):
        tgt, chunk = job
        backs = back_translate(args.cli, args.models, [rows[i]["tgt"] for i in chunk], tgt)
        return [(i, sentence_chrf(b, rows[i]["src"])) for i, b in zip(chunk, backs)]

    scores = {}
    done = 0
    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        for result in pool.map(run_job, jobs):
            scores.update(result)
            done += len(result)
            print(f"  scored {done}/{sum(len(v) for v in by_tgt.values())}", flush=True)
    kept, dropped = 0, 0
    with open(args.out, "w", encoding="utf-8") as fh:
        for i, r in enumerate(rows):
            s = scores.get(i)
            if s is not None and s < args.min_chrf:
                dropped += 1
                continue
            if s is not None:
                r = dict(r, chrf=round(s, 1))
            fh.write(json.dumps(r, ensure_ascii=False) + "\n")
            kept += 1
    per = {}
    for i, s in scores.items():
        per.setdefault(rows[i]["tgt_lang"], []).append(s)
    for tgt, ss in per.items():
        ss.sort()
        print(f"  {tgt}: median chrF {ss[len(ss) // 2]:.1f}, kept {sum(1 for x in ss if x >= args.min_chrf)}/{len(ss)}")
    print(f"→ {args.out}: kept {kept}, dropped {dropped}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
