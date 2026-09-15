"""Sign text in capitals vs the same text in sentence case through the app's Marian pairs.

Photos of signs are mostly capitals, which OPUS-MT has rarely seen. The app sentence-cases all-caps
text before translating it (`OcrEngine.forTranslation`); this prints both outputs side by side to be
judged by reading. Needs ctranslate2 and sentencepiece (py -3.11) and the pairs under
models/release/nmt (or --models).

2026-09-15, beam 4: right in capitals 16/50, sentence-cased 36/50
(en->ko 12 -> 14 of 20, fr->en 1 -> 7, es->en 0 -> 8, ru->en 3 -> 7 of 10).
"""
import argparse
import sys
from pathlib import Path

import ctranslate2
import sentencepiece as spm

SETS = {
    "en-ko": ["NO PARKING", "EMERGENCY EXIT", "PUSH TO OPEN", "WET FLOOR", "STAFF ONLY", "KEEP OUT",
              "CLOSED ON SUNDAYS", "PLEASE WAIT TO BE SEATED", "NO SMOKING IN THIS AREA", "PULL",
              "TOILETS", "BEWARE OF THE DOG", "DO NOT TOUCH", "ELEVATOR OUT OF SERVICE", "TICKET OFFICE",
              "OPENING HOURS", "FRESH FISH", "ALL DAY BREAKFAST", "MIND THE GAP", "AUTHORIZED PERSONNEL ONLY"],
    "fr-en": ["SORTIE DE SECOURS", "DÉFENSE DE FUMER", "FERMÉ LE DIMANCHE", "ENTRÉE INTERDITE", "POUSSEZ",
              "TIREZ", "ATTENTION CHIEN MÉCHANT", "STATIONNEMENT INTERDIT", "SOLDES", "PLAT DU JOUR"],
    "es-en": ["SALIDA DE EMERGENCIA", "PROHIBIDO FUMAR", "CERRADO LOS DOMINGOS", "EMPUJE", "TIRE",
              "PROHIBIDO EL PASO", "PISO MOJADO", "SOLO PERSONAL AUTORIZADO", "FARMACIA DE GUARDIA", "SALIDA"],
    "ru-en": ["ВЫХОД", "ВХОД ВОСПРЕЩЁН", "НЕ КУРИТЬ", "ЗАКРЫТО", "ОСТОРОЖНО, ОКРАШЕНО", "АПТЕКА", "КАССА",
              "ТОЛЬКО ДЛЯ ПЕРСОНАЛА", "ЗАПАСНОЙ ВЫХОД", "ПРОДУКТЫ"],
}


def sentence_case(s: str) -> str:
    low = s.lower()
    return low[:1].upper() + low[1:]


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--models", default=str(Path(__file__).resolve().parents[2] / "models" / "release" / "nmt"))
    args = ap.parse_args()
    sys.stdout.reconfigure(encoding="utf-8")
    for pair, lines in SETS.items():
        d = Path(args.models) / pair
        tr = ctranslate2.Translator(str(d), device="cpu", compute_type="int8")
        sp_s = spm.SentencePieceProcessor(model_file=str(d / "source.spm"))
        sp_t = spm.SentencePieceProcessor(model_file=str(d / "target.spm"))
        eos = pair != "en-ko"  # en-ko's converted config adds </s> itself; the others need it appended

        def run(texts):
            toks = [sp_s.encode(t, out_type=str) + (["</s>"] if eos else []) for t in texts]
            res = tr.translate_batch(toks, beam_size=4, max_decoding_length=64)
            return [sp_t.decode(r.hypotheses[0]) for r in res]

        caps = run(lines)
        cased = run([sentence_case(t) for t in lines])
        print(f"== {pair}")
        for src, a, b in zip(lines, caps, cased):
            print(f"{src}\n   CAPS : {a}\n   cased: {b}")


if __name__ == "__main__":
    main()
