#!/usr/bin/env python3
"""
LoRA fine-tuning of the on-device translation LLM (Qwen2.5-Instruct) on parallel sentences,
followed by merge → GGUF → quantize, so the result drops straight into models/llm/.

This is the "learning" step for the 7 languages (ko en es vi th ja zh). It needs a GPU
(≈ 12 GB VRAM for 1.5B in bf16 with LoRA r=16; ≈ 20 GB for 3B) — it is NOT run on device
and NOT run in CI. On-device the app only runs inference.

    pip install "torch>=2.3" "transformers>=4.45" "peft>=0.12" "trl>=0.11" datasets accelerate sentencepiece

    # 1. Build a training set from Tatoeba (Helsinki-NLP/tatoeba_mt) for all language pairs
    python scripts/finetune_lora.py data --languages ko en es vi th ja zh --max-per-pair 20000 --out data/train.jsonl

    #    ...or bring your own JSONL: {"src_lang":"ko","tgt_lang":"en","src":"...","tgt":"..."} per line
    #    (domain phrases, glossaries, corrected outputs collected from the app, ...)

    # 2. Train (LoRA) — the prompt format is exactly what LlmEngine sends at runtime
    python scripts/finetune_lora.py train --base Qwen/Qwen2.5-1.5B-Instruct --data data/train.jsonl --out runs/qwen1.5b-mt

    # 3. Merge + export GGUF (needs a llama.cpp checkout for the converter/quantizer)
    python scripts/finetune_lora.py export --base Qwen/Qwen2.5-1.5B-Instruct --lora runs/qwen1.5b-mt \
        --llama-cpp third_party/llama.cpp --quant Q4_K_M --out dist/models/llm/qwen2.5-1.5b-mt-q4_k_m.gguf

Then reference the GGUF from the manifest ("llm" entry) — see docs/MODELS.md.
"""
import argparse
import json
import os
import random
import subprocess
import sys
from pathlib import Path

LANG_NAMES = {
    "ko": "Korean", "en": "English", "es": "Spanish", "vi": "Vietnamese", "th": "Thai",
    "ja": "Japanese", "zh": "Chinese (Simplified)",
}
# Tatoeba uses ISO-639-3 codes.
TATOEBA = {"ko": "kor", "en": "eng", "es": "spa", "vi": "vie", "th": "tha", "ja": "jpn", "zh": "cmn"}


def instruction(src: str, tgt: str) -> str:
    """Must stay identical to LlmEngine::buildInstruction (src/LlmEngine.cpp)."""
    target = LANG_NAMES.get(tgt, tgt)
    if src in ("", "auto"):
        s = f"You are a professional interpreter. Detect the language of the user's message and translate it into {target}. "
    else:
        s = f"You are a professional interpreter. Translate the user's message from {LANG_NAMES.get(src, src)} into {target}. "
    s += (f"Rules: output ONLY the {target} translation, nothing else; no explanations, no notes, no quotes; "
          "keep the meaning, tone and politeness level; keep numbers, names, times and units unchanged; "
          f"if the message is already in {target}, output it unchanged.")
    return s


def to_messages(ex: dict) -> dict:
    # 30% of examples use "auto" so the model also learns source-language detection.
    src = "auto" if random.random() < 0.3 else ex["src_lang"]
    return {"messages": [
        {"role": "system", "content": instruction(src, ex["tgt_lang"])},
        {"role": "user", "content": ex["src"]},
        {"role": "assistant", "content": ex["tgt"]},
    ]}


# ---------------------------------------------------------------------------

def cmd_data(a: argparse.Namespace) -> None:
    from datasets import load_dataset  # noqa: WPS433

    langs = a.languages
    out = Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    n_total = 0
    with open(out, "w", encoding="utf-8") as fh:
        for i, l1 in enumerate(langs):
            for l2 in langs[i + 1:]:
                pair = "-".join(sorted([TATOEBA[l1], TATOEBA[l2]]))
                try:
                    ds = load_dataset("Helsinki-NLP/tatoeba_mt", pair, split="test")
                except Exception as e:  # pair not available
                    print(f"  skip {l1}-{l2}: {e}", file=sys.stderr)
                    continue
                rows = list(ds)
                random.shuffle(rows)
                rows = rows[: a.max_per_pair]
                for r in rows:
                    s_lang, t_lang = r["sourceLang"], r["targetLang"]
                    inv = {v: k for k, v in TATOEBA.items()}
                    sl, tl = inv.get(s_lang, s_lang), inv.get(t_lang, t_lang)
                    fh.write(json.dumps({"src_lang": sl, "tgt_lang": tl, "src": r["sourceString"], "tgt": r["targetString"]}, ensure_ascii=False) + "\n")
                    fh.write(json.dumps({"src_lang": tl, "tgt_lang": sl, "src": r["targetString"], "tgt": r["sourceString"]}, ensure_ascii=False) + "\n")
                    n_total += 2
                print(f"  {l1}-{l2}: {len(rows)} pairs (both directions)")
    print(f"→ {out}: {n_total} examples")


def cmd_train(a: argparse.Namespace) -> None:
    import torch
    from datasets import load_dataset
    from peft import LoraConfig
    from transformers import AutoModelForCausalLM, AutoTokenizer
    from trl import SFTConfig, SFTTrainer

    tok = AutoTokenizer.from_pretrained(a.base)
    model = AutoModelForCausalLM.from_pretrained(a.base, torch_dtype=torch.bfloat16, device_map="auto")
    ds = load_dataset("json", data_files=a.data, split="train").shuffle(seed=42)
    ds = ds.map(to_messages, remove_columns=ds.column_names)
    split = ds.train_test_split(test_size=min(0.02, 2000 / max(len(ds), 1)), seed=42)

    lora = LoraConfig(r=a.rank, lora_alpha=2 * a.rank, lora_dropout=0.05, bias="none", task_type="CAUSAL_LM",
                      target_modules=["q_proj", "k_proj", "v_proj", "o_proj", "gate_proj", "up_proj", "down_proj"])
    cfg = SFTConfig(
        output_dir=a.out, num_train_epochs=a.epochs, per_device_train_batch_size=a.batch,
        gradient_accumulation_steps=a.grad_accum, learning_rate=a.lr, lr_scheduler_type="cosine",
        warmup_ratio=0.03, logging_steps=20, eval_strategy="steps", eval_steps=500, save_steps=500,
        save_total_limit=2, bf16=True, max_length=512, packing=False, report_to="none",
    )
    trainer = SFTTrainer(model=model, args=cfg, train_dataset=split["train"], eval_dataset=split["test"],
                         processing_class=tok, peft_config=lora)
    trainer.train()
    trainer.save_model(a.out)
    print(f"→ LoRA adapter saved to {a.out}")


def cmd_export(a: argparse.Namespace) -> None:
    import torch
    from peft import PeftModel
    from transformers import AutoModelForCausalLM, AutoTokenizer

    merged = Path(a.out).with_suffix("").as_posix() + "-merged-hf"
    print(f"→ merging LoRA into {merged}")
    tok = AutoTokenizer.from_pretrained(a.base)
    base = AutoModelForCausalLM.from_pretrained(a.base, torch_dtype=torch.bfloat16)
    model = PeftModel.from_pretrained(base, a.lora).merge_and_unload()
    model.save_pretrained(merged, safe_serialization=True)
    tok.save_pretrained(merged)

    llama = Path(a.llama_cpp)
    conv = llama / "convert_hf_to_gguf.py"
    f16 = Path(a.out).with_suffix("").as_posix() + "-f16.gguf"
    subprocess.run([sys.executable, str(conv), merged, "--outfile", f16, "--outtype", "f16"], check=True)
    quant = next((p for p in [llama / "build/bin/llama-quantize", llama / "build/bin/Release/llama-quantize.exe",
                              llama / "llama-quantize"] if p.exists()), None)
    if quant is None:
        print(f"!! llama-quantize not found under {llama}; build llama.cpp, then run:\n"
              f"   llama-quantize {f16} {a.out} {a.quant}", file=sys.stderr)
        return
    subprocess.run([str(quant), f16, a.out, a.quant], check=True)
    os.remove(f16)
    print(f"→ {a.out}")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    d = sub.add_parser("data", help="build a Tatoeba-based JSONL training set")
    d.add_argument("--languages", nargs="+", default=["ko", "en", "es", "vi", "th", "ja", "zh"])
    d.add_argument("--max-per-pair", type=int, default=20000)
    d.add_argument("--out", default="data/train.jsonl")
    d.set_defaults(fn=cmd_data)

    t = sub.add_parser("train", help="LoRA fine-tune")
    t.add_argument("--base", default="Qwen/Qwen2.5-1.5B-Instruct")
    t.add_argument("--data", required=True)
    t.add_argument("--out", required=True)
    t.add_argument("--rank", type=int, default=16)
    t.add_argument("--epochs", type=float, default=1.0)
    t.add_argument("--batch", type=int, default=8)
    t.add_argument("--grad-accum", type=int, default=4)
    t.add_argument("--lr", type=float, default=2e-4)
    t.set_defaults(fn=cmd_train)

    e = sub.add_parser("export", help="merge LoRA and export a quantized GGUF")
    e.add_argument("--base", default="Qwen/Qwen2.5-1.5B-Instruct")
    e.add_argument("--lora", required=True)
    e.add_argument("--llama-cpp", default="third_party/llama.cpp")
    e.add_argument("--quant", default="Q4_K_M")
    e.add_argument("--out", required=True)
    e.set_defaults(fn=cmd_export)

    a = ap.parse_args()
    random.seed(42)
    a.fn(a)


if __name__ == "__main__":
    main()
