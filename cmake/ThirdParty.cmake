# Third-party engine discovery. Each engine is optional: if its source tree is absent under
# third_party/ the corresponding TRANSLATOR_HAS_* flag is 0 and the core compiles a stub, so
# the pure C++ parts (ModelManager, segmenter, C API, tests) build on any desktop.
#
# Populate third_party/ with scripts/fetch_third_party.sh (or .ps1).

set(TRANSLATOR_TP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

# CMake 4 refuses cmake_minimum_required(< 3.5) found in some vendored sub-projects
# (cpu_features, clog); this keeps them configurable without patching them.
if(NOT DEFINED CMAKE_POLICY_VERSION_MINIMUM)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "Minimum policy version for vendored projects")
endif()

# iOS: never turn third-party executables into .app bundles (breaks their install() rules).
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(CMAKE_MACOSX_BUNDLE OFF)
endif()

set(TRANSLATOR_HAS_RNNOISE 0)
set(TRANSLATOR_HAS_WHISPER 0)
set(TRANSLATOR_HAS_CTRANSLATE2 0)
set(TRANSLATOR_HAS_SENTENCEPIECE 0)
set(TRANSLATOR_HAS_LLAMA 0)

# ---------------------------------------------------------------------------
# ggml options shared by llama.cpp and whisper.cpp (whoever is added first creates the
# `ggml` target; whisper.cpp skips its own copy when the target already exists).
# ---------------------------------------------------------------------------
set(GGML_NATIVE OFF CACHE BOOL "" FORCE)   # never -march=native for mobile
set(GGML_OPENMP OFF CACHE BOOL "" FORCE)
set(GGML_CCACHE OFF CACHE BOOL "" FORCE)
if(APPLE)
    set(GGML_METAL               ${TRANSLATOR_METAL} CACHE BOOL "" FORCE)
    set(GGML_METAL_EMBED_LIBRARY ON                  CACHE BOOL "" FORCE)
    set(GGML_ACCELERATE          ON                  CACHE BOOL "" FORCE)
    set(GGML_BLAS                ON                  CACHE BOOL "" FORCE)
    set(GGML_BLAS_VENDOR         Apple               CACHE STRING "" FORCE)
endif()
if(ANDROID)
    set(GGML_VULKAN ${TRANSLATOR_VULKAN} CACHE BOOL "" FORCE)
    set(GGML_OPENCL ${TRANSLATOR_OPENCL} CACHE BOOL "" FORCE)
endif()

# Recursively set a target property on every buildable target created under `dir`.
function(_translator_set_property_recursive dir prop value)
    get_property(_targets DIRECTORY ${dir} PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_t IN LISTS _targets)
        get_target_property(_type ${_t} TYPE)
        if(NOT _type STREQUAL "INTERFACE_LIBRARY" AND NOT _type STREQUAL "UTILITY")
            set_target_properties(${_t} PROPERTIES ${prop} "${value}")
        endif()
    endforeach()
    get_property(_subdirs DIRECTORY ${dir} PROPERTY SUBDIRECTORIES)
    foreach(_s IN LISTS _subdirs)
        _translator_set_property_recursive(${_s} ${prop} "${value}")
    endforeach()
endfunction()

function(_translator_force_msvc_dll_runtime dir)
    _translator_set_property_recursive(${dir} MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endfunction()

function(_translator_dep_missing name hint)
    if(TRANSLATOR_STRICT_DEPS)
        message(FATAL_ERROR "${name} not found under third_party/. ${hint}")
    else()
        message(WARNING "${name} not found under third_party/ — building stub. ${hint}")
    endif()
endfunction()

# Sub-projects must build static so the app ships a single library. whisper.cpp / ggml use
# option(BUILD_SHARED_LIBS ...) under an old policy version, so the cache entry must exist.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build third-party engines as static libraries" FORCE)
set(_translator_saved_bsl OFF)

# ---------------------------------------------------------------------------
# RNNoise (compiled from source; ~20 C files)
# ---------------------------------------------------------------------------
if(TRANSLATOR_WITH_RNNOISE)
    set(_rn ${TRANSLATOR_TP_DIR}/rnnoise)
    if(EXISTS ${_rn}/include/rnnoise.h)
        file(GLOB _rn_src ${_rn}/src/*.c)
        # Exclude tools / demos / SIMD RTCD trees that need extra flags.
        # rnnoise_data_little.c defines the same symbols as rnnoise_data.c (smaller model);
        # pick one with TRANSLATOR_RNNOISE_LITTLE.
        option(TRANSLATOR_RNNOISE_LITTLE "Use the smaller RNNoise model (rnnoise_data_little.c)" OFF)
        if(TRANSLATOR_RNNOISE_LITTLE)
            list(FILTER _rn_src EXCLUDE REGEX "(dump_|write_weights|demo|rnnoise_train|/rnnoise_data\\.c$)")
        else()
            list(FILTER _rn_src EXCLUDE REGEX "(dump_|write_weights|demo|rnnoise_train|rnnoise_data_little)")
        endif()
        if(NOT EXISTS ${_rn}/src/rnnoise_data.c AND NOT EXISTS ${_rn}/src/rnn_data.c)
            _translator_dep_missing("RNNoise model weights (src/rnnoise_data.c)"
                "Run third_party/rnnoise/download_model.sh (scripts/fetch_third_party does this).")
        else()
            add_library(translator_rnnoise STATIC ${_rn_src})
            # cmake/shim/rnnoise provides os_support.h (Opus header referenced by vec.h but
            # not shipped with rnnoise).
            target_include_directories(translator_rnnoise
                PUBLIC ${_rn}/include
                PRIVATE ${_rn}/src ${CMAKE_CURRENT_SOURCE_DIR}/cmake/shim/rnnoise)
            target_compile_definitions(translator_rnnoise PRIVATE RNNOISE_BUILD _USE_MATH_DEFINES)
            if(NOT MSVC)
                target_compile_options(translator_rnnoise PRIVATE -O3 -w)
            else()
                target_compile_options(translator_rnnoise PRIVATE /w)
                target_compile_definitions(translator_rnnoise PRIVATE restrict=__restrict)
            endif()
            set(TRANSLATOR_HAS_RNNOISE 1)
        endif()
    else()
        _translator_dep_missing("RNNoise" "git clone https://github.com/xiph/rnnoise third_party/rnnoise")
    endif()
endif()

# ---------------------------------------------------------------------------
# llama.cpp (+ ggml) — LLM translation backend. Added BEFORE whisper.cpp so both share one ggml.
# ---------------------------------------------------------------------------
if(TRANSLATOR_WITH_LLAMA)
    set(_ll ${TRANSLATOR_TP_DIR}/llama.cpp)
    if(EXISTS ${_ll}/CMakeLists.txt)
        set(LLAMA_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
        set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(LLAMA_BUILD_SERVER   OFF CACHE BOOL "" FORCE)
        set(LLAMA_BUILD_TOOLS    OFF CACHE BOOL "" FORCE)
        set(LLAMA_BUILD_COMMON   OFF CACHE BOOL "" FORCE)
        set(LLAMA_CURL           OFF CACHE BOOL "" FORCE)
        add_subdirectory(${_ll} ${CMAKE_BINARY_DIR}/third_party/llama.cpp EXCLUDE_FROM_ALL)
        _translator_set_property_recursive(${_ll} POSITION_INDEPENDENT_CODE ON)
        set(TRANSLATOR_HAS_LLAMA 1)
    else()
        _translator_dep_missing("llama.cpp" "git clone --branch b5030 https://github.com/ggml-org/llama.cpp third_party/llama.cpp")
    endif()
endif()

# ---------------------------------------------------------------------------
# whisper.cpp (+ ggml) — official CMake project
# ---------------------------------------------------------------------------
if(TRANSLATOR_WITH_WHISPER)
    set(_wh ${TRANSLATOR_TP_DIR}/whisper.cpp)
    if(EXISTS ${_wh}/CMakeLists.txt)
        set(WHISPER_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
        set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(WHISPER_BUILD_SERVER   OFF CACHE BOOL "" FORCE)
        set(WHISPER_CURL           OFF CACHE BOOL "" FORCE)
        if(APPLE)
            set(WHISPER_COREML           ${TRANSLATOR_COREML} CACHE BOOL "" FORCE)
            set(WHISPER_COREML_ALLOW_FALLBACK ON              CACHE BOOL "" FORCE)
        endif()
        add_subdirectory(${_wh} ${CMAKE_BINARY_DIR}/third_party/whisper.cpp EXCLUDE_FROM_ALL)
        _translator_set_property_recursive(${_wh} POSITION_INDEPENDENT_CODE ON)
        set(TRANSLATOR_HAS_WHISPER 1)
    else()
        _translator_dep_missing("whisper.cpp" "git clone --branch v1.7.5 https://github.com/ggml-org/whisper.cpp third_party/whisper.cpp")
    endif()
endif()

# ---------------------------------------------------------------------------
# CTranslate2 — official CMake project (needs its submodules: ruy, cpu_features, spdlog)
# ---------------------------------------------------------------------------
if(TRANSLATOR_WITH_CTRANSLATE2)
    set(_ct ${TRANSLATOR_TP_DIR}/ctranslate2)
    if(EXISTS ${_ct}/CMakeLists.txt)
        if(NOT EXISTS ${_ct}/third_party/ruy/CMakeLists.txt)
            message(FATAL_ERROR "CTranslate2 submodules missing: run 'git submodule update --init --recursive' in third_party/ctranslate2")
        endif()
        set(BUILD_CLI            OFF  CACHE BOOL   "" FORCE)
        set(BUILD_TESTS          OFF  CACHE BOOL   "" FORCE)
        set(WITH_MKL             OFF  CACHE BOOL   "" FORCE)
        set(WITH_DNNL            OFF  CACHE BOOL   "" FORCE)
        set(WITH_CUDA            OFF  CACHE BOOL   "" FORCE)
        set(WITH_CUDNN           OFF  CACHE BOOL   "" FORCE)
        set(WITH_OPENBLAS        OFF  CACHE BOOL   "" FORCE)
        set(WITH_RUY             ON   CACHE BOOL   "" FORCE)   # ARM int8/float GEMM
        # cpuinfo (via ruy) builds command-line tools by default; on iOS CMake turns those into
        # app bundles and its install() rules fail. We only need the library.
        set(CPUINFO_BUILD_TOOLS      OFF CACHE BOOL "" FORCE)
        set(CPUINFO_BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
        set(CPUINFO_BUILD_MOCK_TESTS OFF CACHE BOOL "" FORCE)
        set(CPUINFO_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
        set(CPUINFO_BUILD_PKG_CONFIG OFF CACHE BOOL "" FORCE)
        # Without OpenMP CTranslate2 falls back to a thread_local BS::thread_pool, which was
        # observed to deadlock on MSVC with intra-op threads > 1. Use the compiler's OpenMP
        # everywhere it exists (MSVC, GCC/Clang, Android NDK libomp); iOS toolchains lack it.
        if(APPLE)
            set(OPENMP_RUNTIME   NONE CACHE STRING "" FORCE)   # Apple clang has no OpenMP
        else()
            set(OPENMP_RUNTIME   COMP CACHE STRING "" FORCE)
        endif()
        set(ENABLE_CPU_DISPATCH  OFF  CACHE BOOL   "" FORCE)
        set(ENABLE_PROFILING     OFF  CACHE BOOL   "" FORCE)
        if(APPLE)
            set(WITH_ACCELERATE  ON   CACHE BOOL   "" FORCE)
        endif()
        add_subdirectory(${_ct} ${CMAKE_BINARY_DIR}/third_party/ctranslate2 EXCLUDE_FROM_ALL)
        if(MSVC)
            # CTranslate2 forces the static CRT (/MT); everything else here uses /MD.
            _translator_force_msvc_dll_runtime(${_ct})
        endif()
        # Its static archives end up inside our shared library → must be PIC (Android linker
        # rejects absolute relocations otherwise).
        _translator_set_property_recursive(${_ct} POSITION_INDEPENDENT_CODE ON)
        set(TRANSLATOR_HAS_CTRANSLATE2 1)
    else()
        _translator_dep_missing("CTranslate2" "git clone --recursive --branch v4.5.0 https://github.com/OpenNMT/CTranslate2 third_party/ctranslate2")
    endif()
endif()

# ---------------------------------------------------------------------------
# SentencePiece — official CMake project, static, with built-in protobuf-lite
# ---------------------------------------------------------------------------
if(TRANSLATOR_WITH_SENTENCEPIECE)
    set(_sp ${TRANSLATOR_TP_DIR}/sentencepiece)
    if(EXISTS ${_sp}/CMakeLists.txt)
        set(SPM_ENABLE_SHARED        OFF CACHE BOOL "" FORCE)
        set(SPM_USE_BUILTIN_PROTOBUF ON  CACHE BOOL "" FORCE)
        set(SPM_ENABLE_TCMALLOC      OFF CACHE BOOL "" FORCE)
        set(SPM_BUILD_TEST           OFF CACHE BOOL "" FORCE)
        set(SPM_ENABLE_NFKC_COMPILE  OFF CACHE BOOL "" FORCE)
        # sentencepiece's iOS branch calls set_xcode_property() from a third-party toolchain
        # file we don't use; provide a no-op so its CLI tools configure.
        if(NOT COMMAND set_xcode_property)
            function(set_xcode_property)
            endfunction()
        endif()
        add_subdirectory(${_sp} ${CMAKE_BINARY_DIR}/third_party/sentencepiece EXCLUDE_FROM_ALL)
        _translator_set_property_recursive(${_sp} POSITION_INDEPENDENT_CODE ON)
        if(ANDROID)
            # protobuf-lite's default log handler calls __android_log_write.
            target_link_libraries(sentencepiece-static INTERFACE log)
        endif()
        set(TRANSLATOR_SENTENCEPIECE_INCLUDE ${_sp}/src)
        set(TRANSLATOR_HAS_SENTENCEPIECE 1)
    else()
        _translator_dep_missing("SentencePiece" "git clone --branch v0.2.0 https://github.com/google/sentencepiece third_party/sentencepiece")
    endif()
endif()

set(BUILD_SHARED_LIBS ${_translator_saved_bsl})
