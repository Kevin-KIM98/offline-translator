"""Photo text recognition: Tesseract's thresholding and the photo's orientation.

The app hands Tesseract (tessdata_fast, PSM_AUTO) a bitmap of at most 2000 px with no resolution;
Tesseract estimates one from the text. This renders a ticket in capitals (English) and a price board
(Spanish), makes each look photographed (uneven light, noise, blur, JPEG) and rotates it, then
reports:

  * per thresholding method, how many of the page's words Tesseract returns before and after the
    app's confidence filter (words under 30, lines under 45), and how many invented words;
  * per rotation of the input, OcrEngine's orientation decision: the score (letters in words of
    confidence >= 70 on lines that run across) as the photo stands at full size, the scores of the
    four ways round on probes, the turn the app makes, and the words found after it.

Tesseract reads text turned a quarter clockwise by itself (lines running down), so counting every
confident letter could not tell that from upright; the overlay needs lines that run across.

2026-09-15 (tesseract 5.3.4 on the CI runner; the app bundles 5.5.1): thresholding_method=1 found
26% (ticket) and 21% (board) of the words on the photo look against 95% / 79% with the default 0.

Needs tesseract 5.x on PATH, Pillow, numpy, DejaVu fonts, and a tessdata directory holding
eng/spa traineddata (the app's `ocr_*.traineddata` from models-v1, renamed).
"""
import argparse
import csv
import io
import subprocess
import sys
import tempfile
import time
from collections import Counter
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

NAVY = (45, 42, 110)
RED = (215, 45, 45)
WHITE = (255, 255, 255)
BLACK = (20, 20, 20)

# OcrEngine's constants.
CONFIDENT = 70
UPRIGHT_LETTERS = 20
UPRIGHT_SHARE = 0.5
TURN_MARGIN = 1.5

TICKET = [
    ("text", 110, 30, 70, "bold", BLACK, "DELTA"),
    ("text", 800, 40, 44, "mono", BLACK, "MILITARY EXCUSE"),
    ("text", 1500, 40, 44, "mono", BLACK, "MILITARY EXCUSE"),
    ("text", 850, 85, 44, "mono", BLACK, "10SEP26 B0066"),
    ("text", 1370, 85, 44, "mono", BLACK, "US"),
    ("text", 720, 130, 44, "mono", BLACK, "DL/CC"),
    ("text", 1030, 130, 44, "mono", BLACK, "/ATLFTO"),
    ("text", 100, 175, 44, "mono", BLACK, "PARK/MINSOOMR"),
    ("text", 150, 220, 44, "mono", BLACK, "**NOT VALID FOR**"),
    ("text", 145, 262, 44, "mono", BLACK, "**TRANSPORTATION*"),
    ("text", 1440, 220, 44, "mono", BLACK, "NOT VALID FOR TRAVEL"),
    ("text", 75, 430, 44, "mono", BLACK, "FLIGHT DIVERTED DUE TO MEDICAL EMERGENCY CAUSING"),
    ("text", 65, 475, 44, "mono", BLACK, "MR PARK TO MISS CONNECTION TO SEOUL"),
    ("text", 55, 560, 44, "mono", BLACK, "WE SINCERELY REGRET THIS INCONVENIENCE"),
    ("text", 1430, 560, 44, "mono", BLACK, "NOT VALID FOR TRAVEL"),
]

BOARD_LEFT = [
    ("head", 90, "ABONOS"),
    ("red", 30, "¡Ahorrá y usalos a medida que los necesites!"),
    ("bar", 52, "VALET/LAVADO Y SECADO"),
    ("bold", 30, "LAVADO + SECADO"),
    ("text", 36, "• Unidad ________ $ 9.000"),
    ("text", 36, "• Abono de 5 ____ $ 42.600"),
    ("bar", 52, "PACK PLANCHADO"),
    ("bold", 30, "SOLO PLANCHADO"),
    ("text", 36, "• Unidad ______ desde $ 4.200"),
    ("text", 36, "• Abono de 10 ____ $ 42.600"),
    ("bar", 52, "BLANCO PLUS"),
    ("bold", 28, "RECUPERACIÓN DE BLANCOS"),
    ("bold", 54, "LAVANDERÍA DESDE $ 3.500"),
    ("bold", 54, "CAMISA DESDE $ 2.000"),
    ("bold", 54, "BLUSA DESDE $ 4.900"),
    ("bold", 54, "PANTALÓN DESDE $ 6.500"),
    ("bold", 54, "VESTIDO DESDE $ 6.500"),
]

BOARD_RIGHT = [
    ("head", 90, "ADICIONALES"),
    ("red", 32, "para tu servicio habitual"),
    ("text", 32, "Tratamientos que cuidan más tus prendas"),
    ("bar", 52, "SANITIZANTE"),
    ("bold", 30, "Inhibe y reduce la proliferación de bacterias"),
    ("text", 34, "• PRENDA DE TINTORERÍA DESDE $ 3.500"),
    ("text", 34, "• VALET $ 2.000"),
    ("text", 34, "• JUEGO DE SÁBANAS $ 2.500"),
    ("text", 34, "• ACOLCHADO DE PLUMAS $ 6.000"),
    ("bar", 52, "PLANCHA PLUS"),
    ("bold", 40, "Prolonga la duración del planchado"),
    ("bold", 40, "y reduce la fijación de las manchas"),
    ("bar", 44, "PARA SU INFORMACIÓN:"),
]


def column(x, w, y, rows):
    out = []
    for kind, size, text in rows:
        if kind in ("head", "bar"):
            out.append(("rect", x, y, x + w, y + size + 34, RED if kind == "head" else NAVY))
            out.append(("text", x + 24, y + 12, size, "bold", WHITE, text))
            y += size + 50
        else:
            color = RED if kind == "red" else BLACK
            out.append(("text", x + 24, y, size, "regular" if kind == "text" else "bold", color, text))
            y += int(size * 1.4)
    return out


def board():
    return column(40, 940, 40, BOARD_LEFT) + column(1030, 940, 40, BOARD_RIGHT)


PAGES = {
    "ticket-en": ("eng", (2000, 830), TICKET),
    "board-es": ("spa", (2000, 1400), board()),
}


def render(size, items, fonts):
    img = Image.new("RGB", size, WHITE)
    d = ImageDraw.Draw(img)
    files = {"regular": "DejaVuSans.ttf", "bold": "DejaVuSans-Bold.ttf", "mono": "DejaVuSansMono.ttf"}
    for it in items:
        if it[0] == "rect":
            d.rectangle(it[1:5], fill=it[5])
        else:
            _, x, y, px, style, color, text = it
            d.text((x, y), text, font=ImageFont.truetype(str(Path(fonts) / files[style]), px), fill=color)
    return img


def photograph(img, seed=1):
    """Uneven light (a third darker at one corner), sensor noise, a little blur, JPEG."""
    rng = np.random.default_rng(seed)
    a = np.asarray(img, dtype=np.float32)
    h, w = a.shape[:2]
    yy, xx = np.mgrid[0:h, 0:w]
    light = 0.62 + 0.38 * (0.6 * xx / w + 0.4 * yy / h)
    a = a * light[..., None] + rng.normal(0, 6, a.shape)
    out = Image.fromarray(np.clip(a, 0, 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(1.0))
    buf = io.BytesIO()
    out.save(buf, "JPEG", quality=80)
    buf.seek(0)
    return Image.open(buf).convert("RGB")


def fit(img, longest):
    s = longest / max(img.size)
    return img if s >= 1 else img.resize((round(img.width * s), round(img.height * s)), Image.LANCZOS)


def ocr(img, lang, tessdata, method=0):
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "page.png"
        img.save(path)  # no pHYs, like the app's bitmap: Tesseract estimates the resolution
        cmd = ["tesseract", "--tessdata-dir", str(Path(tessdata).resolve()), str(path), "stdout",
               "-l", lang, "--psm", "3", "-c", f"thresholding_method={method}",
               "-c", "tessedit_create_tsv=1", "-c", "tessedit_create_txt=0"]
        t = time.time()
        proc = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8")
        secs = time.time() - t
    out = proc.stdout
    if not out.startswith("level"):
        raise RuntimeError(f"tesseract failed ({proc.returncode}): {proc.stderr.strip()[:2000]}")
    words = []
    for row in csv.DictReader(io.StringIO(out), delimiter="\t", quoting=csv.QUOTE_NONE):
        text = (row.get("text") or "").strip()
        if row["level"] == "5" and text and float(row["conf"]) >= 0:
            key = (int(row["block_num"]), int(row["par_num"]), int(row["line_num"]))
            left, top = int(row["left"]), int(row["top"])
            box = (left, top, left + int(row["width"]), top + int(row["height"]))
            words.append({"line": key, "conf": float(row["conf"]), "text": text, "box": box})
    return words, secs


def app_filter(words):
    """OcrEngine.lineOf: drop lines whose letters average under 45, then words under 30."""
    lines = {}
    for w in words:
        lines.setdefault(w["line"], []).append(w)
    kept = []
    for ws in lines.values():
        chars = sum(len(w["text"]) for w in ws)
        mean = sum(w["conf"] * len(w["text"]) for w in ws) / max(1, chars)
        if mean < 45:
            continue
        good = [w for w in ws if w["conf"] >= 30]
        if any(c.isalnum() for w in good for c in w["text"]):
            kept += good
    return kept


def norm(t):
    return "".join(c for c in t.upper() if c.isalnum())


def truth_of(items):
    return Counter(n for it in items if it[0] == "text" for tok in it[6].split() if (n := norm(tok)))


def compare(words, truth):
    found = Counter(n for w in words if (n := norm(w["text"])))
    hit = sum((truth & found).values())
    return hit / sum(truth.values()), sum((found - truth).values())


def nletters(w):
    return sum(c.isalnum() for c in w["text"])


def letters(words):
    return sum(nletters(w) for w in words)


def confident(words):
    return sum(nletters(w) for w in words if w["conf"] >= CONFIDENT)


def across(words):
    """OcrEngine.read: confident letters on lines whose words' union box is at least as wide as tall."""
    boxes = {}
    for w in words:
        b = boxes.get(w["line"])
        x0, y0, x1, y1 = w["box"]
        boxes[w["line"]] = w["box"] if b is None else (min(b[0], x0), min(b[1], y0), max(b[2], x1), max(b[3], y1))
    flat = {k for k, b in boxes.items() if b[2] - b[0] >= b[3] - b[1]}
    return sum(nletters(w) for w in words if w["conf"] >= CONFIDENT and w["line"] in flat)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tessdata", required=True)
    ap.add_argument("--fonts", default="/usr/share/fonts/truetype/dejavu")
    ap.add_argument("--probe", type=int, default=1000)
    ap.add_argument("--out", default="tests/eval/out/ocr")
    ap.add_argument("--skip-thresholding", action="store_true")
    args = ap.parse_args()
    sys.stdout.reconfigure(encoding="utf-8", line_buffering=True)
    Path(args.out).mkdir(parents=True, exist_ok=True)

    if not args.skip_thresholding:
        print("== thresholding (upright, 2000 px, no resolution given)")
        print(f"{'page':<10} {'look':<6} {'method':>6} {'words':>6} {'found':>6} {'filtered':>8} {'invented':>8} {'conf>=70':>8} {'letters':>7} {'secs':>5}")
        for name, (lang, size, items) in PAGES.items():
            truth = truth_of(items)
            clean = render(size, items, args.fonts)
            for look, img in (("scan", clean), ("photo", photograph(clean))):
                img.save(Path(args.out) / f"{name}-{look}.jpg", quality=85)
                for method in (0, 1, 2):
                    words, secs = ocr(img, lang, args.tessdata, method)
                    recall, invented = compare(words, truth)
                    filtered, _ = compare(app_filter(words), truth)
                    print(f"{name:<10} {look:<6} {method:>6} {len(words):>6} {recall:>6.0%} {filtered:>8.0%} {invented:>8} {confident(words):>8} {letters(words):>7} {secs:>5.1f}")
        print()

    print("== orientation (photo look): OcrEngine's decision")
    print("   as-is = across/confident/letters at 2000 px; probes = across score turned 0/90/180/270 clockwise at probe size")
    print(f"{'page':<10} {'input':>5}  {'as-is':>13} {'reads':>5}  {'probes':>21}  {'turn':>4} {'right':>5}  found")
    for name, (lang, size, items) in PAGES.items():
        truth = truth_of(items)
        photo = photograph(render(size, items, args.fonts))
        for rot in (0, 90, 180, 270):
            turned = photo.rotate(rot, expand=True)  # PIL turns counter-clockwise
            words, _ = ocr(turned, lang, args.tessdata)
            a, c, n = across(words), confident(words), letters(words)
            reads = a >= UPRIGHT_LETTERS and a >= n * UPRIGHT_SHARE
            probes = "-"
            turn = 0
            if not reads:
                scores = []
                for back in (0, 90, 180, 270):
                    pw, _ = ocr(fit(turned.rotate(-back, expand=True), args.probe), lang, args.tessdata)
                    scores.append(across(pw))
                probes = "/".join(map(str, scores))
                best = max(range(4), key=lambda i: scores[i])
                if best != 0 and scores[best] >= UPRIGHT_LETTERS and scores[best] > scores[0] * TURN_MARGIN:
                    turn = best * 90
            final = words if turn == 0 else ocr(turned.rotate(-turn, expand=True), lang, args.tessdata)[0]
            recall = compare(app_filter(final), truth)[0]
            ok = "ok" if turn == rot else "WRONG"
            print(f"{name:<10} {rot:>5}  {f'{a}/{c}/{n}':>13} {'yes' if reads else 'no':>5}  {probes:>21}  {turn:>4} {ok:>5}  {recall:.0%}")


if __name__ == "__main__":
    main()
