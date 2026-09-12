#!/usr/bin/env python3
"""Download the models a language set needs, for testing on a desktop.

This is the desktop counterpart of what the phone apps do on first launch. The policy — which
models a language pair needs, whether the LLM has to come along, SHA-256 verification and the
atomic install — all lives in the C++ core and is reached through translator_cli, so this script
only performs the HTTP downloads.

    python scripts/fetch_models.py --models pc-models --langs ko,en
    python scripts/fetch_models.py --models pc-models --langs ko,th --llm always

Then:

    translator_cli listen --models pc-models --src ko --tgt en
"""
import argparse
import json
import os
import pathlib
import shutil
import subprocess
import sys
import urllib.request
import zipfile

DEFAULT_MANIFEST = "https://raw.githubusercontent.com/Kevin-KIM98/offline-translator/main/assets/manifest.json"


def find_cli(explicit: str | None) -> str:
    if explicit:
        return explicit
    root = pathlib.Path(__file__).resolve().parent.parent
    names = ["translator_cli.exe", "translator_cli"]
    candidates = []
    for build in root.glob("build*"):
        for name in names:
            candidates += [build / "Release" / name, build / name]
    # Newest first: several build directories often coexist, and only the current one has the
    # engines linked in.
    existing = sorted((c for c in candidates if c.is_file()), key=lambda c: c.stat().st_mtime, reverse=True)
    if existing:
        return str(existing[0])
    found = shutil.which("translator_cli")
    if found:
        return found
    sys.exit(
        "translator_cli not found. Build it first:\n"
        "  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release\n"
        "  cmake --build build --config Release --target translator_cli\n"
        "or pass --cli <path>."
    )


def human(n: int) -> str:
    if n >= 1_000_000_000:
        return f"{n / 1_000_000_000:.1f} GB"
    if n >= 1_000_000:
        return f"{n // 1_000_000} MB"
    return f"{n // 1_000} KB"


def download(url: str, target: pathlib.Path, expected: int) -> None:
    """Resumable download into `target` via a .part file."""
    target.parent.mkdir(parents=True, exist_ok=True)
    if target.is_file() and expected and target.stat().st_size == expected:
        print(f"    {target.name}: already downloaded")
        return

    part = target.with_suffix(target.suffix + ".part")
    offset = part.stat().st_size if part.is_file() else 0
    if expected and offset >= expected:
        part.unlink()
        offset = 0

    request = urllib.request.Request(url)
    if offset:
        request.add_header("Range", f"bytes={offset}-")
    with urllib.request.urlopen(request) as response:
        if offset and response.status != 206:
            offset = 0  # server ignored the range
        total = expected or (int(response.headers.get("Content-Length", 0)) + offset)
        done = offset
        with open(part, "ab" if offset else "wb") as out:
            while True:
                block = response.read(1 << 18)
                if not block:
                    break
                out.write(block)
                done += len(block)
                if total:
                    pct = done * 100 // total
                    print(f"\r    {target.name}: {pct:3d}%  {human(done)} / {human(total)}", end="", flush=True)
    print()
    if expected and part.stat().st_size != expected:
        sys.exit(f"    {target.name}: incomplete ({part.stat().st_size} of {expected} bytes)")
    if target.exists():
        target.unlink()
    part.rename(target)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--models", required=True, help="directory to install into (created if needed)")
    ap.add_argument("--langs", help="comma-separated languages, e.g. ko,en. Omit for every model in the manifest.")
    ap.add_argument("--llm", choices=["if-needed", "always", "never"], default="if-needed")
    ap.add_argument("--llm-id", help="which LLM from the manifest (see llm_options); default: the manifest's llm")
    ap.add_argument("--stt-id", help="which whisper model from the manifest (see stt_options); default: the manifest's stt")
    ap.add_argument("--manifest", default=DEFAULT_MANIFEST, help="manifest URL or local path")
    ap.add_argument("--cli", help="path to translator_cli")
    ap.add_argument("--dry-run", action="store_true", help="list what would be downloaded and stop")
    args = ap.parse_args()

    cli = find_cli(args.cli)
    models = pathlib.Path(args.models)
    models.mkdir(parents=True, exist_ok=True)

    manifest = pathlib.Path(args.manifest)
    if not manifest.is_file():
        manifest = models / "manifest.json"
        print(f"manifest: {args.manifest}")
        urllib.request.urlretrieve(args.manifest, manifest)

    cmd = [cli, "status", "--models", str(models), "--manifest", str(manifest)]
    if args.langs:
        cmd += ["--langs", args.langs, "--llm", args.llm]
        if args.llm_id:
            cmd += ["--llm-id", args.llm_id]
        if args.stt_id:
            cmd += ["--stt-id", args.stt_id]
    result = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8")
    if result.returncode != 0:
        sys.stderr.write(result.stderr)
        return result.returncode
    status = json.loads(result.stdout)

    pending = [m for m in status["models"] if m["state"] != "ready"]
    if not pending:
        print(f"all models ready in {models}")
        return 0

    total = sum(m["total_bytes"] for m in pending)
    print(f"\n{len(pending)} model(s) to download, {human(total)}:")
    for m in pending:
        print(f"  {m['id']:<34} {human(m['total_bytes']):>8}  ({m['state']})")
    if args.dry_run:
        return 0
    print()

    staging_root = models / ".staging"
    failed = []
    for m in pending:
        print(f"{m['id']}:")
        # Kept between runs so an interrupted download resumes instead of starting over.
        staging = staging_root / m["id"]
        staging.mkdir(parents=True, exist_ok=True)
        try:
            for d in m["downloads"]:
                target = staging / d["filename"]
                download(d["url"], target, int(d.get("size_bytes", 0)))
                if d.get("archive"):
                    with zipfile.ZipFile(target) as z:
                        z.extractall(staging)
                    target.unlink()
            install = subprocess.run(
                [cli, "install", "--models", str(models), "--id", m["id"], "--staged", str(staging)],
                capture_output=True, text=True, encoding="utf-8",
            )
            if install.returncode != 0:
                # A hash mismatch would fail again on retry, so drop the staged copy.
                failed.append(f"{m['id']}: {install.stderr.strip()}")
                print(f"    install failed: {install.stderr.strip()}")
                shutil.rmtree(staging, ignore_errors=True)
            else:
                print(f"    verified and installed")
                shutil.rmtree(staging, ignore_errors=True)
        except Exception as exc:  # noqa: BLE001 - report and continue with the next model
            failed.append(f"{m['id']}: {exc}")
            print(f"    failed: {exc}")

    if staging_root.exists() and not any(staging_root.iterdir()):
        staging_root.rmdir()

    if failed:
        print("\nfailed:")
        for f in failed:
            print("  " + f)
        print("Re-run to resume; finished files are kept.")
        return 1

    print(f"\nready. Try:\n  translator_cli listen --models {models} --src {(args.langs or 'ko,en').split(',')[0]} "
          f"--tgt {(args.langs or 'ko,en').split(',')[-1]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
