#!/usr/bin/env python3
"""Compare LLM configurations for translation into Thai, over a fixed sentence set.

    python tests/eval/run_th_eval.py --set tests/eval/th_conversation.json
    python tests/eval/run_th_eval.py --set tests/eval/th_heldout.json --only app-1.5b,app-1.5b-legacy

Each configuration runs `translator_cli translate --lines`, so the models load once per
configuration, and configurations run one after another so they do not compete for the CPU.

Routes:
  app     Korean to Thai through the pipeline's own Auto backend: exactly what the apps run,
          including any route it chooses. Timings are end to end.
  direct  Korean straight to Thai with the LLM.
  pivot   Korean to English with Marian, then English to Thai with the LLM, run as two passes.
  gold    the reference English to Thai with the LLM: the ceiling of the pivot route, which
          separates errors of the English step from errors of the Thai step.
"""
import argparse
import json
import pathlib
import statistics
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(HERE))
import chrf  # noqa: E402

LLM = {
    "1.5b": ROOT / "models" / "llm" / "qwen2.5-1.5b-instruct-q4_k_m.gguf",
    "3b": ROOT / "models" / "llm" / "qwen2.5-3b-instruct-q4_k_m.gguf",
}

CONFIGS = [
    # name,                  route,    model,  extra translator_cli arguments
    ("app-1.5b",            "app",    "1.5b", []),
    ("app-1.5b-legacy",     "app",    "1.5b", ["--llm-examples", "single", "--no-llm-pivot"]),
    ("app-3b",              "app",    "3b",   ["--no-llm-pivot"]),
    ("direct-1.5b-single",  "direct", "1.5b", ["--llm-examples", "single"]),
    ("direct-1.5b-none",    "direct", "1.5b", ["--llm-examples", "none"]),
    ("direct-1.5b-diverse", "direct", "1.5b", ["--llm-examples", "diverse"]),
    ("pivot-1.5b-single",   "pivot",  "1.5b", ["--llm-examples", "single"]),
    ("pivot-1.5b-none",     "pivot",  "1.5b", ["--llm-examples", "none"]),
    ("pivot-1.5b-diverse",  "pivot",  "1.5b", ["--llm-examples", "diverse"]),
    ("gold-1.5b-diverse",   "gold",   "1.5b", ["--llm-examples", "diverse"]),
    ("direct-3b-diverse",   "direct", "3b",   ["--llm-examples", "diverse"]),
    ("pivot-3b-diverse",    "pivot",  "3b",   ["--llm-examples", "diverse"]),
]


def translate_lines(cli, models, lines, src, tgt, backend, llm=None, extra=()):
    cmd = [cli, "translate", "--models", str(models), "--src", src, "--tgt", tgt,
           "--backend", backend, "--lines"]
    if llm:
        cmd += ["--llm", str(llm)]
    cmd += list(extra)
    proc = subprocess.run(cmd, input="\n".join(lines) + "\n", capture_output=True,
                          text=True, encoding="utf-8")
    results = [json.loads(l) for l in proc.stdout.splitlines() if l.startswith("{")]
    if len(results) != len(lines):
        sys.stderr.write(proc.stderr[-2000:])
        raise RuntimeError(f"expected {len(lines)} results, got {len(results)}")
    return results


def score(refs, hyps):
    totals = [[0, 0, 0] for _ in range(chrf.MAX_N)]
    foreign = 0
    for s in refs:
        hyp = hyps[str(s["id"])]
        for i, (m, h, r) in enumerate(chrf.sentence_stats(hyp, s["th"])):
            totals[i][0] += m
            totals[i][1] += h
            totals[i][2] += r
        if chrf.foreign_letters(hyp):
            foreign += 1
    return chrf.chrf_from([tuple(t) for t in totals]), foreign


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--set", required=True)
    ap.add_argument("--out", default=str(HERE / "out"))
    ap.add_argument("--cli", default=str(ROOT / "build-full" / "Release" / "translator_cli.exe"))
    ap.add_argument("--models", default=str(ROOT / "models" / "release"))
    ap.add_argument("--only", help="comma-separated configuration names")
    args = ap.parse_args()

    data = json.load(open(args.set, encoding="utf-8"))["sentences"]
    out = pathlib.Path(args.out) / pathlib.Path(args.set).stem
    out.mkdir(parents=True, exist_ok=True)
    wanted = set(args.only.split(",")) if args.only else None

    english = None  # Korean to English once, shared by every two-pass pivot configuration
    summary = []
    for name, route, model, extra in CONFIGS:
        if wanted and name not in wanted:
            continue
        print(f"running {name} ...", flush=True)
        if route == "pivot" and english is None:
            first = translate_lines(args.cli, args.models, [s["ko"] for s in data], "ko", "en", "marian")
            english = [r["translated_text"] for r in first]
            json.dump(dict(zip((str(s["id"]) for s in data), english)),
                      open(out / "pivot-english.json", "w", encoding="utf-8"), ensure_ascii=False, indent=1)

        if route == "app":
            results = translate_lines(args.cli, args.models, [s["ko"] for s in data], "ko", "th", "auto",
                                      LLM[model], extra)
        elif route == "direct":
            results = translate_lines(args.cli, args.models, [s["ko"] for s in data], "ko", "th", "llm",
                                      LLM[model], extra)
        elif route == "pivot":
            results = translate_lines(args.cli, args.models, english, "en", "th", "llm", LLM[model], extra)
        else:
            results = translate_lines(args.cli, args.models, [s["en"] for s in data], "en", "th", "llm",
                                      LLM[model], extra)

        hyps = {str(s["id"]): r["translated_text"] for s, r in zip(data, results)}
        ms = statistics.median(r["timings"]["total_ms"] for r in results)
        routes = sorted({"->".join(r["route"]) for r in results})
        value, foreign = score(data, hyps)
        summary.append((name, value, foreign, ms, routes))
        json.dump(hyps, open(out / f"{name}.json", "w", encoding="utf-8"), ensure_ascii=False, indent=1)

    print(f"\n{'configuration':22} {'chrF':>6} {'foreign':>8} {'median ms':>10}  routes")
    for name, value, foreign, ms, routes in summary:
        print(f"{name:22} {value:6.1f} {foreign:8d} {ms:10.0f}  {', '.join(routes)}")
    json.dump([dict(name=n, chrf=v, foreign=f, median_ms=m, routes=r) for n, v, f, m, r in summary],
              open(out / "summary.json", "w", encoding="utf-8"), indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
