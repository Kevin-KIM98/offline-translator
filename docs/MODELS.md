# Model preparation

## 0. LLM backend (any→any, 7 languages)

Since 0.3 the engine can translate with a small instruction-tuned LLM through llama.cpp instead
of (or in addition to) the per-pair Marian models. One GGUF file covers every direction among
Korean, English, Spanish, Vietnamese, Thai, Japanese and Chinese, and the source language may be
left to the model (`"auto"`).

| model | file | size | CPU latency (desktop, 4 threads, 2–3 sentences) | notes |
|---|---|---|---|---|
| Qwen2.5-1.5B-Instruct Q4_K_M | `qwen2.5-1.5b-instruct-q4_k_m.gguf` | 1.12 GB | 1.4–3.4 s | shipped default; fluent, occasional tense/time slips (Thai "พรุ่งนี้" → "오늘") |
| Qwen2.5-3B-Instruct Q4_K_M | `qwen2.5-3b-instruct-q4_k_m.gguf` | 2.10 GB | 3.4–6.1 s | clearly better into Korean from English; same Thai time slips |

Measured with the engine's safeguards on (see below). Samples, 1.5B: ko→th
"สวัสดีครับ กำหนดการประชุมในวันพรุ่งนี้จะเริ่มเวลา 3:00 น. สถานีใต้ดินที่ใกล้ที่สุดอยู่ที่ไหนครับ",
ko→vi "Xin chào. Hội nghị của tôi sẽ bắt đầu vào 3 giờ chiều ngày mai. Ga gần nhất ở đâu?",
ja→zh "你好。明天的会议定在下午3点开始。最近的车站在哪里？". 3B en→ko: "안녕하세요. 내일의 회의는 오후 3시에
시작합니다. 이 제품은 배터리 수명이 길고 화면도 밝아서 외부 사용에도 좋습니다."

**Why small LLMs need guard rails.** Unconstrained, both models regularly drift into Chinese
mid-sentence when the target is Thai or Korean, append translator's notes, or loop
("ครับ ครับ ครับ…"). `LlmEngine` therefore (1) puts a one-shot demonstration in the target script
into the chat, (2) applies a llama.cpp logit bias that forbids every vocabulary token written
in a script foreign to the target (Han/Kana for Korean, Hangul/Han for Thai, …; control/EOS
tokens excluded), (3) adds a repetition penalty and stops at the first line break or a
4× repeated token, (4) strips note markers, and (5) retries once with a stricter instruction
if the output still mixes scripts. With these on, the script mixing disappeared in all tested
directions.

Grab them from `Qwen/Qwen2.5-*-Instruct-GGUF` on Hugging Face and add an `llm` entry to the manifest:

```json
"llm": { "id": "qwen2.5-1.5b-instruct-q4_k_m", "version": "1", "filename": "qwen2.5-1.5b-instruct-q4_k_m.gguf",
         "size_bytes": 1043030016, "sha256": "…", "download_url": "llm_qwen2.5-1.5b-instruct-q4_k_m.gguf" }
```

**How the backends are combined (`translation_backend = auto`)**: a Marian pair is used whenever
one exists for the direction (direct, or X→en→Y through the English models) because it is 5–10×
faster and, for X→English, at least as good; the LLM handles everything else (e.g. en→th, ko→vi,
ja→zh direct). `LlmMode.IF_NEEDED` makes the app download the GGUF only when the chosen languages
actually need it. Force LLM-only with `backend = llm`.

**Teaching it your domain ("학습")**: `scripts/finetune_lora.py` fine-tunes the same Qwen model
with LoRA on parallel sentences (Tatoeba for all 7 languages, plus your own JSONL — glossaries,
corrected app outputs), merges the adapter and exports a quantized GGUF that drops into the
manifest. The training prompt is byte-identical to what `LlmEngine` sends at runtime. It needs a
GPU machine (≈ 12 GB VRAM for 1.5B); nothing is trained on the phone.

Everything the devices download is produced by `scripts/prepare_models.py` and published as a
static file tree + `manifest.json`. No server logic is needed — any CDN / object storage works.

```
pip install "ctranslate2>=4.5" "transformers>=4.40" sentencepiece torch huggingface_hub
```

## 1. STT — whisper.cpp ggml models

| model | fp16 | q5_1 | q8_0 | notes |
|---|---|---|---|---|
| `base` | 148 MB | 60 MB | 82 MB | fast, weak on ko/ja/zh |
| `small` | 488 MB | **190 MB** | 264 MB | recommended default for 5-language use |
| `medium` | 1.5 GB | 539 MB | 823 MB | best accuracy, ~3× slower |
| `large-v3-turbo` | 1.6 GB | 574 MB | 874 MB | near-large accuracy, 4 decoder layers |

```
python scripts/prepare_models.py whisper --model small --quant q5_1 --out dist/models
python scripts/prepare_models.py whisper --model small --quant q5_1 --coreml   # + iOS CoreML encoder
```

**iOS CoreML**: whisper.cpp looks for `<model-name>-encoder.mlmodelc` next to the ggml file.
Ship the unzipped `ggml-small-encoder.mlmodelc` directory in `models/stt/` (add it to the
manifest as an NMT-style `files` entry or bundle it in the app). The first launch compiles it
for the device's Neural Engine (takes 10–30 s once). Fallback to Metal/CPU is automatic.

**Android**: whisper.cpp has no NNAPI backend. CPU (NEON, 4 threads) handles `small-q5_1` at
roughly 0.3–0.6× real time on a 2022+ SoC. `-DTRANSLATOR_VULKAN=ON` offloads to the GPU on
Adreno/Mali devices with Vulkan 1.2 (test per device — some drivers are slower than CPU).

## 2. NMT — CTranslate2 INT8 (OPUS-MT / MarianMT)

```
python scripts/prepare_models.py nmt --pairs ko-en en-ko ja-ko zh-ko es-ko --out dist/models
```

Each pair directory contains `model.bin`, `config.json`, `shared_vocabulary.json` (or
`source_vocabulary.json` + `target_vocabulary.json`), `source.spm`, `target.spm`.
Typical INT8 size: 60–90 MB per pair.

### Quick start without Python

Pre-converted CTranslate2 OPUS-MT models are published on Hugging Face and load as-is
(config.json + model.bin + shared_vocabulary.json + source.spm + target.spm), e.g.
`manancode/opus-mt-ko-en-ctranslate2-android` (INT8, 80 MB) or
`gaudi/opus-mt-ko-en-ctranslate2` (float16, 155 MB). Download the five files into
`models/nmt/ko-en/` and run `translator_cli translate --models models --text "..." --src ko --tgt en`.
The engine reads `config.json`'s `add_source_eos` flag and appends `</s>` itself when the
converter did not (the normal case for transformers-converted Marian models).

### What the default manifest ships (`assets/manifest.json`)

| pair | model | size | notes |
|---|---|---|---|
| STT | whisper `small-q5_1` | 190 MB | all seven languages |
| ko-en, ja-en, zh-en, es-en | OPUS-MT base INT8 | ≈ 80 MB each | pre-converted (Hugging Face `jiangzhuo9357/*-ct2`) |
| en-zh, en-es | OPUS-MT base INT8 | ≈ 80 MB each | same source |
| en-ja | `opus-tatoeba-en-ja` INT8 | 78 MB | `opus-mt-en-jap` is trained on Bible text and unusable for modern Japanese |
| en-ko | `opus-mt-tc-big-en-ko` INT8, converted from the original Marian weights | ≈ 215 MB | see below |
| vi-en, en-vi | OPUS-MT base INT8 (`dekthedev/*-ct2-int8`) | 73 MB each | |
| th-en | OPUS-MT base INT8 (converted from `Helsinki-NLP/opus-mt-th-en`) | 82 MB | no Marian en→th exists → the LLM handles it |
| LLM | Qwen2.5-1.5B-Instruct Q4_K_M | 1.12 GB | any direction without a Marian route; downloaded only when needed |

Every other direction pivots through English automatically when both halves exist (ko↔ja, ko↔zh, vi→ko, th→ko, …); directions that cannot be pivoted (anything → Thai, and any pair the LLM-only backend is asked for) go to the LLM.

**tc-big caveat.** The Hugging Face uploads of `opus-mt-tc-big-en-ko` / `-ko-en` carry a
`vocab.json` that does not match their SentencePiece models, so *both* transformers itself and
every CTranslate2 conversion derived from them (including several community `-ct2` repos) emit
`<unk>` garbage. Convert those from the original OPUS-MT release instead:

```
# https://object.pouta.csc.fi/Tatoeba-MT-models/eng-kor/opusTCv20210807-sepvoc_transformer-big_2022-07-28.zip
ct2-marian-converter --model_path model.npz --vocab_paths source.vocab.yml target.vocab.yml \
    --output_dir en-ko --quantization int8
cp source.spm target.spm en-ko/
```

(`python -m ctranslate2.converters.marian` is the same tool.) The server is slow; `curl -r`
range requests in parallel help.

### Pair coverage & pivoting

Helsinki-NLP publishes direct models for most X↔en pairs but not every X↔ko pair. The
engine resolves a route at runtime:

1. direct `src-tgt` directory exists → one hop
2. else for each pivot in `pivotLangs` (default `ko`, `en`): `src-pivot` + `pivot-tgt` → two hops

So for a Korean-centric app it is enough to ship: `ko-en`, `en-ko`, `ja-en`, `en-ja`, `zh-en`,
`en-zh`, `es-en`, `en-es` (8 pairs ≈ 600 MB) — every combination of the five languages is then
reachable via `en`. Add direct `ja-ko` / `zh-ko` pairs when a good model is available to
improve quality and halve latency for those directions.

Override any source repo: `--hf-model ja-ko=Helsinki-NLP/opus-mt-ja-ko en-ko=Helsinki-NLP/opus-mt-tc-big-en-ko`.

### Multilingual models (`opus-mt-en-mul`, NLLB)

Multilingual decoders need a language token. Put it in `pair.json` inside the pair directory:

```json
{ "source_prefix_token": ">>kor<<" }     // OPUS-MT *-mul: token prepended to the SOURCE
{ "target_prefix_token": "kor_Hang" }    // NLLB-style: forced first TARGET token
```

NLLB-200-distilled-600M INT8 (~600 MB) is a single model covering all five languages; use it
by symlinking / copying the same directory to each pair name and setting `target_prefix_token`
per pair (`eng_Latn`, `kor_Hang`, `jpn_Jpan`, `zho_Hans`, `spa_Latn`).

### Shared tokenizer

If several pairs share one SentencePiece model, place it in `models/nmt/tokenizer/source.spm`
(and `target.spm`); pair directories without their own `.spm` files fall back to it.

## 3. Manifest

```
python scripts/prepare_models.py manifest --root dist/models --base-url https://assets.yourdomain.com/models
```

Produces `dist/models/manifest.json` in **files mode** (per-file SHA-256, resumable
downloads, no zip step on device). The README-style zip mode is also accepted — see
[assets/manifest.example.json](../assets/manifest.example.json) for both.

Bump `--model-version` (per model) whenever you replace a file; devices report
`update_available` and re-download only the changed model.

Upload the whole `dist/models/` tree. Serve with `Accept-Ranges: bytes` so interrupted
downloads resume.

## 4. Bundling vs. downloading

* Bundle `manifest.json` in the app (`assets/manifest.json` on Android, `manifest.json` in the
  iOS bundle) so the first launch can show sizes and start downloads offline-initiated.
* Models themselves should be downloaded (App Store 200 MB cellular limit; Play asset packs
  are an alternative for the STT model).
* Storage budget: whisper `small-q5_1` (190 MB) + 8 OPUS pairs (~600 MB) ≈ 0.8 GB.
