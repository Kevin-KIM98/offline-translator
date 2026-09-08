# Gradle entry point: delegates to the repository-root CMake project.
cmake_minimum_required(VERSION 3.18.2)
project(offline_translator_android LANGUAGES C CXX)

set(TRANSLATOR_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(TRANSLATOR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(TRANSLATOR_BUILD_SHARED ON CACHE BOOL "" FORCE)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../.. ${CMAKE_BINARY_DIR}/core)
