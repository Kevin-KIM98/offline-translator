cmake_minimum_required(VERSION 3.18.2)
project(OfflineTranslationEngine VERSION 0.3.6 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
if(ANDROID OR IOS OR (APPLE AND CMAKE_SYSTEM_NAME STREQUAL "iOS"))
    set(_TRANSLATOR_MOBILE ON)
else()
    set(_TRANSLATOR_MOBILE OFF)
endif()

option(TRANSLATOR_BUILD_SHARED      "Build offline_translator as a shared library"         ON)
option(TRANSLATOR_WITH_RNNOISE      "Enable RNNoise noise suppression"                     ON)
option(TRANSLATOR_WITH_WHISPER      "Enable whisper.cpp STT"                               ON)
option(TRANSLATOR_WITH_CTRANSLATE2  "Enable CTranslate2 NMT"                               ON)
option(TRANSLATOR_WITH_SENTENCEPIECE "Enable SentencePiece tokenization"                   ON)
option(TRANSLATOR_WITH_LLAMA        "Enable llama.cpp LLM translation backend"             ON)
option(TRANSLATOR_COREML            "whisper.cpp: CoreML encoder (iOS/macOS)"              ${APPLE})
option(TRANSLATOR_METAL             "whisper.cpp: Metal backend (iOS/macOS)"               ${APPLE})
option(TRANSLATOR_VULKAN            "whisper.cpp: Vulkan backend (Android GPU)"            OFF)
option(TRANSLATOR_OPENCL            "whisper.cpp: OpenCL backend (Adreno GPUs)"            OFF)
option(TRANSLATOR_BUILD_CLI         "Build the desktop CLI tool"                           ON)
option(TRANSLATOR_BUILD_TESTS       "Build unit tests"                                     ON)
option(TRANSLATOR_STRICT_DEPS       "Fail configure if an enabled dependency is missing"   OFF)

if(_TRANSLATOR_MOBILE)
    set(TRANSLATOR_BUILD_CLI OFF CACHE BOOL "" FORCE)
    set(TRANSLATOR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
endif()

# ---------------------------------------------------------------------------
# Third-party engines (each is optional; missing ones compile to stubs)
# ---------------------------------------------------------------------------
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/ThirdParty.cmake)

# ---------------------------------------------------------------------------
# Usage requirements shared by every flavour of the core
# ---------------------------------------------------------------------------
add_library(translator_deps INTERFACE)
target_include_directories(translator_deps INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_definitions(translator_deps INTERFACE
    TRANSLATOR_HAS_RNNOISE=${TRANSLATOR_HAS_RNNOISE}
    TRANSLATOR_HAS_WHISPER=${TRANSLATOR_HAS_WHISPER}
    TRANSLATOR_HAS_CTRANSLATE2=${TRANSLATOR_HAS_CTRANSLATE2}
    TRANSLATOR_HAS_SENTENCEPIECE=${TRANSLATOR_HAS_SENTENCEPIECE}
    TRANSLATOR_HAS_LLAMA=${TRANSLATOR_HAS_LLAMA})

if(TRANSLATOR_HAS_RNNOISE)
    target_link_libraries(translator_deps INTERFACE translator_rnnoise)
endif()
if(TRANSLATOR_HAS_WHISPER)
    target_link_libraries(translator_deps INTERFACE whisper)
endif()
if(TRANSLATOR_HAS_LLAMA)
    target_link_libraries(translator_deps INTERFACE llama)
endif()
if(TRANSLATOR_HAS_CTRANSLATE2)
    target_link_libraries(translator_deps INTERFACE ctranslate2)
endif()
if(TRANSLATOR_HAS_SENTENCEPIECE)
    target_link_libraries(translator_deps INTERFACE sentencepiece-static)
    target_include_directories(translator_deps INTERFACE ${TRANSLATOR_SENTENCEPIECE_INCLUDE})
endif()

if(APPLE)
    find_library(FOUNDATION_LIB Foundation REQUIRED)
    find_library(ACCELERATE_LIB Accelerate REQUIRED)
    target_link_libraries(translator_deps INTERFACE ${FOUNDATION_LIB} ${ACCELERATE_LIB})
    if(TRANSLATOR_METAL)
        find_library(METAL_LIB Metal REQUIRED)
        find_library(METALKIT_LIB MetalKit REQUIRED)
        target_link_libraries(translator_deps INTERFACE ${METAL_LIB} ${METALKIT_LIB})
    endif()
    if(TRANSLATOR_COREML)
        find_library(COREML_LIB CoreML REQUIRED)
        target_link_libraries(translator_deps INTERFACE ${COREML_LIB})
    endif()
elseif(ANDROID)
    find_library(LOG_LIB log REQUIRED)
    find_library(ANDROID_LIB android REQUIRED)
    target_link_libraries(translator_deps INTERFACE ${LOG_LIB} ${ANDROID_LIB})
elseif(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(translator_deps INTERFACE Threads::Threads)
endif()

# ---------------------------------------------------------------------------
# Core sources (compiled once, linked into both the shared and the static flavour)
# ---------------------------------------------------------------------------
set(TRANSLATOR_SOURCES
    src/Sha256.cpp
    src/MiniJson.cpp
    src/FileUtil.cpp
    src/TextUtil.cpp
    src/Denoiser.cpp
    src/SpeechSegmenter.cpp
    src/SttEngine.cpp
    src/NmtEngine.cpp
    src/LlmEngine.cpp
    src/TranslationPipeline.cpp
    src/ModelManager.cpp
    src/translator_c_api.cpp
)
if(ANDROID)
    # JNI bridge lives in the same .so so the app loads a single library.
    list(APPEND TRANSLATOR_SOURCES platform/android/src/main/cpp/offline_translator_jni.cpp)
endif()

add_library(translator_objs OBJECT ${TRANSLATOR_SOURCES})
target_link_libraries(translator_objs PRIVATE translator_deps)
target_compile_definitions(translator_objs PRIVATE TRANSLATOR_VERSION_STRING="${PROJECT_VERSION}")
if(TRANSLATOR_BUILD_SHARED)
    target_compile_definitions(translator_objs PRIVATE TRANSLATOR_BUILD_SHARED=1)
endif()
if(MSVC)
    target_compile_options(translator_objs PRIVATE /W4 /utf-8 /EHsc)
    target_compile_definitions(translator_objs PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
    target_compile_options(translator_objs PRIVATE -Wall -Wextra -fvisibility=hidden)
endif()

# Shared library: the artifact shipped inside the apps (C ABI exported).
if(TRANSLATOR_BUILD_SHARED)
    add_library(offline_translator SHARED $<TARGET_OBJECTS:translator_objs>)
else()
    add_library(offline_translator STATIC $<TARGET_OBJECTS:translator_objs>)
endif()
target_link_libraries(offline_translator PUBLIC translator_deps)
set_target_properties(offline_translator PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    MACOSX_RPATH ON
    XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC YES)
add_library(translator::core ALIAS offline_translator)

# Static flavour for the CLI / tests (C++ classes are not DLL-exported on Windows).
add_library(translator_static STATIC $<TARGET_OBJECTS:translator_objs>)
target_link_libraries(translator_static PUBLIC translator_deps)
add_library(translator::static ALIAS translator_static)

# ---------------------------------------------------------------------------
# CLI and tests (desktop only)
# ---------------------------------------------------------------------------
function(_translator_tool target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /EHsc)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
    endif()
endfunction()

if(TRANSLATOR_BUILD_CLI)
    add_executable(translator_cli tools/translator_cli.cpp)
    target_link_libraries(translator_cli PRIVATE translator_static)
    _translator_tool(translator_cli)

    # Live microphone for `translator_cli listen`, so the pipeline can be exercised on a
    # desktop before touching a phone. Header-only; absent builds still compile.
    set(_miniaudio_dir ${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniaudio)
    if(EXISTS ${_miniaudio_dir}/miniaudio.h)
        target_compile_definitions(translator_cli PRIVATE TRANSLATOR_HAS_MINIAUDIO=1)
        target_include_directories(translator_cli PRIVATE ${_miniaudio_dir})
        find_package(Threads REQUIRED)
        target_link_libraries(translator_cli PRIVATE Threads::Threads)
        if(APPLE)
            target_link_libraries(translator_cli PRIVATE
                "-framework CoreFoundation" "-framework CoreAudio" "-framework AudioToolbox")
        elseif(UNIX)
            target_link_libraries(translator_cli PRIVATE ${CMAKE_DL_LIBS} m)
        endif()
    else()
        target_compile_definitions(translator_cli PRIVATE TRANSLATOR_HAS_MINIAUDIO=0)
        message(STATUS "miniaudio not found - 'translator_cli listen' will be unavailable")
    endif()
endif()

if(TRANSLATOR_BUILD_TESTS)
    enable_testing()
    add_executable(translator_tests tests/test_core.cpp)
    target_link_libraries(translator_tests PRIVATE translator_static)
    _translator_tool(translator_tests)
    add_test(NAME translator_tests COMMAND translator_tests)
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS offline_translator
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "OfflineTranslationEngine ${PROJECT_VERSION}")
message(STATUS "  RNNoise        : ${TRANSLATOR_HAS_RNNOISE}")
message(STATUS "  whisper.cpp    : ${TRANSLATOR_HAS_WHISPER}  (CoreML=${TRANSLATOR_COREML} Metal=${TRANSLATOR_METAL} Vulkan=${TRANSLATOR_VULKAN} OpenCL=${TRANSLATOR_OPENCL})")
message(STATUS "  CTranslate2    : ${TRANSLATOR_HAS_CTRANSLATE2}")
message(STATUS "  llama.cpp      : ${TRANSLATOR_HAS_LLAMA}")
message(STATUS "  SentencePiece  : ${TRANSLATOR_HAS_SENTENCEPIECE}")
message(STATUS "  Shared library : ${TRANSLATOR_BUILD_SHARED}")
message(STATUS "  CLI / tests    : ${TRANSLATOR_BUILD_CLI} / ${TRANSLATOR_BUILD_TESTS}")
message(STATUS "")
