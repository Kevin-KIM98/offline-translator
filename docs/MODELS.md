# Model preparation

## 0. LLM backend (any→any, 8 languages)

Since 0.3 the engine can translate with a small instruction-tuned LLM through llama.cpp instead
of (or in addition to) the per-pair Marian models. One GGUF file covers every direction among
Korean, English, Spanish, Vietnamese, Thai, Japanese, Chinese and Indonesian, and the source language may be
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
("ครับ ครับ ครับ…"). `LlmEngine` therefore (1) puts three short demonstrations in the target script
into the chat, (2) applies a llama.cpp logit bias that forbids every vocabulary token written
in a script foreign to the target (Han/Kana for Korean, Hangul/Han for Thai, …; control/EOS
tokens excluded), (3) adds a repetition penalty and stops at the first line break or a
4× repeated token, (4) strips note markers, and (5) retries once with a stricter instruction
if the output still mixes scripts. With these on, the script mixing disappeared in all tested
directions.

The demonstrations are a statement, a request and a count, deliberately unrelated to travel
(`LlmExamples::Diverse`). Up to 0.3.6 there was a single one, "Excuse me, where is the nearest
station?", and the 1.5B model copied it: "화장실이 어디예요?" became "where is the station that has a
toilet", and "천천히 말씀해 주세요" came back as the demonstration itself.

Grab them from `Qwen/Qwen2.5-*-Instruct-GGUF` on Hugging Face and add an `llm` entry to the manifest:

```json
"llm": { "id": "qwen2.5-1.5b-instruct-q4_k_m", "version": "1", "filename": "qwen2.5-1.5b-instruct-q4_k_m.gguf",
         "size_bytes": 1043030016, "sha256": "…", "download_url": "llm_qwen2.5-1.5b-instruct-q4_k_m.gguf" }
```

**How the backends are combined (`translation_backend = auto`)**: a Marian pair is used whenever
one exists for the direction (direct, or X→en→Y through the English models) because it is 5–10×
faster and, for X→English, at least as good. The remaining directions go to the LLM; with the
shipped models that means anything into Thai. When a Marian model can reach English from the
source, the pipeline takes that hop first and hands the LLM English (`llmPivotThroughEnglish`),
because a small LLM reads English far better than Korean. `LlmMode.IF_NEEDED` makes the app
download the GGUF only when the chosen languages need it, together with every Marian model their
routes use. Force LLM-only with `backend = llm`.

Measured into Thai on 30 conversational sentences, plus 10 held-out ones written before any run and
never used to choose a configuration. The sets and the harness are in `tests/eval`; reproduce with
`python tests/eval/run_th_eval.py --set tests/eval/th_conversation.json`.

| Korean → Thai | meaning correct, 30 + 10 | chrF, 30 / 10 |
|---|---|---|
| Qwen2.5-1.5B from Korean, one demonstration (up to 0.3.6) | 17 / 40 | 33.1 / 35.4 |
| Qwen2.5-1.5B from Marian's English, three demonstrations (0.3.7) | 26 / 40 | 39.2 / 34.5 |
| Qwen2.5-3B from Korean, three demonstrations | 31 / 40 | 46.4 / 41.7 |

On the held-out set the English route's chrF did not improve, because of word choice ("lobby"
rendered as "communication room"), while the sentences it got right still rose from 6 to 7 of 10.
Most of its remaining errors are made by the Marian Korean→English step. The 3B model is the larger
gain but doubles the download and the LLM time, so it is offered as a choice rather than the default.

**Offering several LLMs.** The manifest's `llm` object is the default; `llm_options` is an array of
further entries of the same shape, and every entry may carry a human-facing `label`. Old readers
ignore both additions. Only one LLM is used at a time: `ModelManager::statusForLanguages(langs,
deep, llmMode, llmId)` lists the selected one (empty or unknown id: the default), and
`llmModelPath(llmId)` gives its path. The C ABI adds `tr_mm_status_for_languages_llm_json` and
`tr_mm_llm_model_path_for`; Kotlin adds `llmId` to `statusForLanguages`, `pipelineConfig` and
`TranslatorSession.prepare`, plus `ModelRepository.llmOptions()`. The app shows the choice under
Settings → Translation LLM and downloads the model when the chosen languages need one; the
shipped manifest offers Qwen2.5 1.5B (default) and Qwen2.5 3B. On the desktop:
`translator_cli status|translate --llm-id qwen2.5-3b-instruct-q4_k_m`, and
`scripts/fetch_models.py --llm-id ...`. `prepare_models.py manifest` writes the first GGUF (by
name) as `llm` and the rest as `llm_options`.

Run through the pipeline's own Auto route rather than as two separate passes, the scores are
identical and every sentence takes `ko-en → llm`. The extra Marian hop raises the median time per
sentence on a desktop CPU from 1.7–2.0 s to 2.1–2.3 s; the 3B model takes 4.6–4.8 s.

Since 0.3.13 `LlmEngine` keeps the KV cache of the prompt prefix between calls (the instruction
and the demonstrations do not change within a direction) and decodes only the sentence: ko→th
median 2.10 s → 1.08 s per sentence on the desktop CPU (`app-1.5b` 841 ms median with the base
ko-en hop, 988 ms with tc-big), the first sentence of a direction unchanged, chrF 39.2 → 39.5.

**Teaching it your domain ("학습")**: `scripts/finetune_lora.py` fine-tunes the same Qwen model
with LoRA on parallel sentences, merges the adapter and exports a quantized GGUF that drops into
the manifest. The training prompt is byte-identical to what `LlmEngine` sends at runtime,
demonstrations included, and the loss is computed on the translation only. Nothing is trained on
the phone. The recipe as it was run on 2026-09-14 on a laptop RTX 4050 with 6 GB (the result was
measured but not shipped — see below):

```
python scripts/finetune_lora.py data-opus --out data/train_opus.jsonl        # opus-100: en→th 12k + 1k each en→ko/ja/zh/vi/id/es
python scripts/filter_parallel.py --in data/train_opus.jsonl --out data/train_filtered.jsonl
python scripts/finetune_lora.py train --base Qwen/Qwen2.5-1.5B-Instruct --data data/train_filtered.jsonl \
    --out runs/thai --load-4bit --batch 4 --grad-accum 4 --max-length 384      # QLoRA, ~1 h, 5.9 GB
python scripts/finetune_lora.py export --base Qwen/Qwen2.5-1.5B-Instruct --lora runs/thai \
    --llama-cpp third_party/llama.cpp --quant Q4_K_M --out dist/llm/qwen2.5-1.5b-th-q4_k_m.gguf
python tests/eval/run_th_eval.py --set tests/eval/th_conversation.json --tuned dist/llm/qwen2.5-1.5b-th-q4_k_m.gguf
```

`filter_parallel.py` matters: opus-100's subtitle pairs are often misaligned ("How could you tell?"
against a Thai line meaning "I have seen this before"). It translates the target side back to
English with the engine's own Marian models and keeps pairs with chrF ≥ 35 — 7,492 of 12,000 for
Thai. `--load-4bit` is QLoRA (NF4 base, bf16 compute, gradient checkpointing); without it 1.5B
needs about 12 GB. One epoch over 11,253 pairs took 73 minutes (690 steps of 16); eval loss
1.35 → 1.28.

What it gave: from clean English the tuned model is better (chrF 41.3 → 45.4 on the 30 sentences,
35.0 → 35.9 held out), but through the app's route, where the English comes from the Korean→English
Marian model, it is not (39.9 → 39.4 and 28.9 → 28.0), and read side by side the two are about even:
the tuned model fixes some sentences ("Where is the restroom?" → ห้องน้ำอยู่ที่ไหน instead of the
base's wrong ที่ไหนบ้าน, "the air conditioner is broken" → แอร์ในห้องเสียแล้ว) and breaks others
("I lost my passport" → ฉันเสียบัตรประจำตัว, "a bad headache" → chest pain), and it answers in the
casual register of the subtitles it learned from (ฉัน, no ครับ), which an interpreter should not.
So it stays a tool, not a shipped model: what would move Korean→Thai is a Marian-quality
Korean→English hop (most remaining errors start there) and a few thousand *conversational, polite*
Thai pairs rather than subtitles. The 3B model (Settings) remains the measured improvement.

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

Measured with `small-q5_1` through the app's streaming path on 140 synthesised clips (10 travel
sentences × 7 languages × 2 voices; `tests/eval/run_stt_eval.py`): character error rate ko 0.0%,
en 0.0%, es 0.1%, ja 2.1%, zh 2.7%, vi 3.9%, th 14.5%; language identification 140/140. Thai is
the weak language of `small` (tone marks, vowel spellings). `medium-q5_0` (539 MB, offered in the
app since 0.3.10) measured on the same clips, one-shot: th 9.1%, vi 0.8%, ja 0.2%, zh 0.0%, ko/en/es
0.0%, at 3.4–4.4 s per utterance against 1.2–1.5 s for `small`. Thai's remaining errors are vowel
and tone spellings of single words. `large-v3-turbo-q5_0` repeated sentences with the adaptive
encoder window and was not adopted. Synthetic voices are cleaner than a phone microphone, so treat
these as upper bounds.

**Offering several speech models.** As for the LLM: the manifest's `stt` object is the default and
`stt_options` lists further whisper files of the same shape, each with a `label`. One is used at a
time: `ModelManager::statusForLanguages(langs, deep, llmMode, llmId, sttId)` lists the selected
one and `sttModelPath(sttId)` gives its path; the C ABI adds `tr_mm_status_for_languages_json2`
and `tr_mm_stt_model_path_for`; Kotlin adds `sttId` to `statusForLanguages`, `pipelineConfig` and
`TranslatorSession.prepare`, plus `ModelRepository.sttOptions()`. The app shows the choice under
Settings → Speech recognition model. Desktop: `translator_cli status|transcribe|speech --stt-id
whisper-medium-q5_0`, `scripts/fetch_models.py --stt-id ...`; `prepare_models.py manifest` writes
the file named by `--stt-default` as `stt` and the other matches of `--stt-glob` as `stt_options`.

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
| STT | whisper `small-q5_1` | 190 MB | all eight languages |
| STT option | whisper `medium-q5_0` | 539 MB | `stt_options`: chosen in Settings; better Thai and Vietnamese, ~3× the time |
| ja-en, zh-en, es-en | OPUS-MT base INT8 | ≈ 80 MB each | pre-converted (Hugging Face `jiangzhuo9357/*-ct2`) |
| ko-en | `opus-mt-tc-big-ko-en` INT8, converted from the original Marian weights | 212 MB | 0.3.13 (manifest entry version 2): chrF 59.1 → 62.9 / 44.2 → 53.9 against the base model on the 30 + 10 test sentences; 420 ms against 233 ms per sentence |
| en-zh, en-es | OPUS-MT base INT8 | ≈ 80 MB each | same source |
| en-ja | `opus-tatoeba-en-ja` INT8 | 78 MB | `opus-mt-en-jap` is trained on Bible text and unusable for modern Japanese |
| en-ko | `opus-mt-tc-big-en-ko` INT8, converted from the original Marian weights | ≈ 215 MB | see below |
| vi-en, en-vi | OPUS-MT base INT8 (`dekthedev/*-ct2-int8`) | 73 MB each | |
| th-en | OPUS-MT base INT8 (converted from `Helsinki-NLP/opus-mt-th-en`) | 82 MB | no Marian en→th exists → the LLM handles it |
| id-en, en-id | OPUS-MT base INT8 (converted from `Helsinki-NLP/opus-mt-id-en` / `-en-id`) | 77 MB each | 0.3.12; every direction reaches Indonesian through English |
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
