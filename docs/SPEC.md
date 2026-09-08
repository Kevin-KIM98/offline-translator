# 온디바이스 오프라인 5개국어 음성 통역 시스템 개발 가이드 (원본 명세)

> 이 문서는 프로젝트 착수 시 제공된 원본 명세를 그대로 보존한 것입니다.
> 실제 구현은 이 명세를 기준으로 하되 일부를 조정했습니다 — 조정 내역은 [README.md](../README.md)의 "명세 대비 변경 사항"을 참고하세요.

---

## 1. 시스템 아키텍처 및 기술 스택

모바일 NPU/GPU 가속을 적극 활용하여 지연 시간 1~1.5초 이내를 목표로 구성된 파이프라인입니다.

[마이크 입력 (16kHz PCM)] ➔ [Step 1. RNNoise (C-Engine)] ➔ [Step 2. whisper.cpp (CoreML / NNAPI)] ➔ [Step 3. CTranslate2 + MarianMT/NNSB] ➔ [Step 4. OS Native TTS (AVSpeech / Android TTS)]

### 필수 라이브러리 및 모델 명세

- Noise Reduction: RNNoise (C 기반 RNNoise 수치 모델, 약 100KB, CPU 점유율 1% 미만)
- STT Engine: whisper.cpp (ggml-small.bin 또는 ggml-medium-q4_0.bin, 460MB ~ 1.5GB, NPU 가속 지원)
- NMT Engine: CTranslate2 (MarianMT / Meta NNSB INT8 양자화, 언어쌍당 80MB ~ 150MB)
- TTS Engine: iOS AVSpeechSynthesizer / Android TextToSpeech (OS 내장 음성팩, 기본 탑재)

---

## 2. 모델 다운로드 및 에셋 관리 시스템

### 2.1 로컬 파일 구조

```
<App_Internal_Storage>/
└── models/
    ├── manifest.json # 모델 버전 및 체크섬 관리 파일
    ├── stt/
    │ └── whisper-small-q4.bin # Whisper STT 모델
    └── nmt/
        ├── tokenizer/ # 공통 SentencePiece/BPE 토크나이저
        │ └── source.spm
        ├── ko-en/ # 한국어 -> 영어 NMT
        │ ├── model.bin
        │ └── shared_vocabulary.txt
        ├── en-ko/ # 영어 -> 한국어 NMT
        ├── ja-ko/ # 일어 -> 한국어 NMT
        ├── zh-ko/ # 중어 -> 한국어 NMT
        └── es-ko/ # 스페인어 -> 한국어 NMT
```

### 2.2 모델 무결성 관리 파일 (manifest.json)

```json
{
  "manifest_version": "1.0.0",
  "stt": {
    "id": "whisper-small-q4",
    "filename": "whisper-small-q4.bin",
    "size_bytes": 482344960,
    "sha256": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
    "download_url": "https://assets.yourdomain.com/models/stt/whisper-small-q4.bin"
  },
  "nmt": [
    {
      "pair": "ko-en",
      "dir_name": "ko-en",
      "size_bytes": 89128960,
      "sha256": "4b227777d4dd1fc61c6f884f48641d02b4d121d3fd328cb08b5531caac827a61",
      "download_url": "https://assets.yourdomain.com/models/nmt/ko-en.zip"
    },
    {
      "pair": "en-ko",
      "dir_name": "en-ko",
      "size_bytes": 89128960,
      "sha256": "ef2d127de37b942baad06145e54b0c619a1f22327b2ebbcfbec78f5564afe39d",
      "download_url": "https://assets.yourdomain.com/models/nmt/en-ko.zip"
    }
  ]
}
```

---

## 3. CMake 빌드 환경 구성 (원본 초안)

```cmake
cmake_minimum_required(VERSION 3.18.2)
project(OfflineTranslationEngine CXX C)

set(CMAKE_CXX_STANDARD 17)

# 1. iOS CoreML / Android NNAPI 가속 옵션
if(APPLE)
    add_definitions(-DGGML_USE_COREML)
    find_library(COREML_LIB CoreML REQUIRED)
    find_library(METAL_LIB Metal REQUIRED)
    find_library(ACCELERATE_LIB Accelerate REQUIRED)
elseif(ANDROID)
    add_definitions(-DGGML_USE_NNAPI)
    find_library(LOG_LIB log REQUIRED)
    find_library(ANDROID_LIB android REQUIRED)
endif()

# 2. 헤더 디렉토리 지정
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/rnnoise/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/whisper.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ctranslate2/include
)

# 3. 소스 파일 빌드
file(GLOB_RECURSE SOURCE_FILES
    "src/*.cpp"
    "third_party/rnnoise/src/*.c"
    "third_party/whisper.cpp/whisper.cpp"
    "third_party/whisper.cpp/ggml.c"
    "third_party/whisper.cpp/ggml-alloc.c"
    "third_party/whisper.cpp/ggml-backend.c"
    "third_party/whisper.cpp/ggml-quants.c"
)

add_library(offline_translator SHARED ${SOURCE_FILES})

# 4. 프레임워크 링킹
if(APPLE)
    target_link_libraries(offline_translator PRIVATE ${COREML_LIB} ${METAL_LIB} ${ACCELERATE_LIB})
elseif(ANDROID)
    target_link_libraries(offline_translator PRIVATE ${LOG_LIB} ${ANDROID_LIB})
endif()
```

---

## 4. C++ 파이프라인 통합 엔진 코드 (원본 초안)

```cpp
// TranslationPipeline.hpp
#pragma once

#include <iostream>
#include <vector>
#include <string>
#include "rnnoise.h"
#include "whisper.h"
#include <ctranslate2/translator.h>

class TranslationPipeline {
private:
    DenoiseState* rnnoise_st;
    whisper_context* whisper_ctx;
    ctranslate2::Translator* nmt_translator;

public:
    TranslationPipeline() : rnnoise_st(nullptr), whisper_ctx(nullptr), nmt_translator(nullptr) {}

    ~TranslationPipeline() {
        if (rnnoise_st) rnnoise_destroy(rnnoise_st);
        if (whisper_ctx) whisper_free(whisper_ctx);
        if (nmt_translator) delete nmt_translator;
    }

    // 파이프라인 및 AI 모델 초기화
    bool initialize(const std::string& whisper_model_path, const std::string& nmt_model_dir) {
        rnnoise_st = rnnoise_create(NULL);
        if (!rnnoise_st) return false;

        struct whisper_context_params cparams = whisper_context_default_params();
        cparams.use_gpu = true;
        whisper_ctx = whisper_init_from_file_with_params(whisper_model_path.c_str(), cparams);
        if (!whisper_ctx) return false;

        nmt_translator = new ctranslate2::Translator(
            nmt_model_dir,
            ctranslate2::Device::CPU,
            0,
            ctranslate2::ComputeType::INT8
        );
        return true;
    }

    // 노이즈 캔슬링 (16kHz PCM, Frame 단위: 480 샘플 = 30ms)
    std::vector<float> denoiseAudio(const std::vector<float>& raw_pcm_frame, float& out_vad_prob) {
        std::vector<float> clean_frame(480);
        out_vad_prob = rnnoise_process_frame(rnnoise_st, clean_frame.data(), raw_pcm_frame.data());
        return clean_frame;
    }

    // STT -> NMT 통합 번역 실행
    std::string processSpeechToTranslation(const std::vector<float>& pcm_16k_buffer, const std::string& target_lang) {
        if (pcm_16k_buffer.empty()) return "";

        // 1. STT (Whisper)
        whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
        wparams.print_progress = false;
        wparams.language = "auto";

        if (whisper_full(whisper_ctx, wparams, pcm_16k_buffer.data(), pcm_16k_buffer.size()) != 0) {
            return "[Error] STT Processing Failed";
        }

        std::string recognized_text = "";
        int n_segments = whisper_full_n_segments(whisper_ctx);
        for (int i = 0; i < n_segments; ++i) {
            recognized_text += whisper_full_get_segment_text(whisper_ctx, i);
        }

        if (recognized_text.empty()) return "";

        // 2. NMT (CTranslate2)
        std::vector<std::string> tokens = {" " + recognized_text};
        ctranslate2::TranslationResult result = nmt_translator->translate_batch({tokens})[0];

        std::string translated_text = "";
        for (const auto& token : result.hypotheses[0]) {
            translated_text += token;
        }

        return translated_text;
    }
};
```

---

## 5. OS Native TTS 연동 (원본 초안)

### iOS (Swift)

```swift
import AVFoundation

class OfflineTTSManager {
    private let synthesizer = AVSpeechSynthesizer()

    func speak(text: String, languageCode: String) {
        let utterance = AVSpeechUtterance(string: text)
        utterance.voice = AVSpeechSynthesisVoice(language: languageCode)
        utterance.rate = AVSpeechUtteranceDefaultSpeechRate

        synthesizer.speak(utterance)
    }
}
```

### Android (Kotlin)

```kotlin
import android.content.Context
import android.speech.tts.TextToSpeech
import java.util.Locale

class OfflineTTSManager(context: Context) : TextToSpeech.OnInitListener {
    private var tts: TextToSpeech = TextToSpeech(context, this)

    override fun onInit(status: Int) {
        if (status == TextToSpeech.SUCCESS) {
            tts.language = Locale.KOREAN
        }
    }

    fun speak(text: String, locale: Locale) {
        tts.language = locale
        tts.speak(text, TextToSpeech.QUEUE_FLUSH, null, null)
    }
}
```

---

## 6. Claude Code 실행 워크플로우

1. 프로젝트 스캐폴딩 생성:
   "이 README.md에 포함된 CMakeLists.txt와 폴더 구조를 기반으로 C++ 라이브러리 빌드 스캐폴딩을 작성해줘."

2. C++ 파이프라인 구현:
   "제공된 TranslationPipeline.hpp 코드를 프로젝트에 추가하고 iOS Objective-C++ 및 Android JNI 래퍼를 구현해줘."

3. 에셋 관리 로직 구현:
   "제공된 manifest.json 명세를 기반으로 앱 최초 실행 시 Whisper 모델 및 5개국어 NMT 모델 파일의 SHA-256 무결성을 검증하고 다운로드하는 ModelManager 클래스를 작성해줘."
