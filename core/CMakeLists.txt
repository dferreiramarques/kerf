# Kerf core: the sampler engine, with no user interface and no audio-device code.
#
#   cmake -S core -B build/core && cmake --build build/core && ctest --test-dir build/core
#
# The handheld project (../handheld) pulls this in with add_subdirectory.

cmake_minimum_required(VERSION 3.16)
project(kerf_core LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(_kerf_top_level ON)
else()
    set(_kerf_top_level OFF)
endif()

if(_kerf_top_level AND NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# dr_wav.h, dr_mp3.h and dr_flac.h (single-file decoders, public domain / MIT-0).
set(KERF_THIRD_PARTY_DIR "${CMAKE_CURRENT_LIST_DIR}/../third_party" CACHE PATH
    "Folder that holds dr_wav.h, dr_mp3.h, dr_flac.h and font8x8_*.h")

option(KERF_BUILD_CORE_TESTS "Build and register the core tests" ${_kerf_top_level})

find_package(Threads REQUIRED)

add_library(kerf_core STATIC
    src/engine.cpp
    src/project.cpp
    src/sample.cpp
    src/voice.cpp)

target_compile_features(kerf_core PUBLIC cxx_std_20)
target_include_directories(kerf_core PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(kerf_core PRIVATE "${KERF_THIRD_PARTY_DIR}")
target_link_libraries(kerf_core PUBLIC Threads::Threads)

if(MSVC)
    target_compile_options(kerf_core PRIVATE /utf-8 /W3)
else()
    target_compile_options(kerf_core PRIVATE -Wall -Wextra)
endif()

# The tiny test framework, shared with the handheld tests.
add_library(kerf_minitest INTERFACE)
target_include_directories(kerf_minitest INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/tests")

# main() for test programs that only need the framework's runner.
add_library(kerf_test_main STATIC tests/test_main.cpp)
target_link_libraries(kerf_test_main PUBLIC kerf_minitest)
target_compile_features(kerf_test_main PUBLIC cxx_std_20)

if(KERF_BUILD_CORE_TESTS)
    enable_testing()

    add_executable(kerf_core_tests
        tests/test_engine_parity.cpp
        tests/test_core_new.cpp
        tests/test_project.cpp
        tests/test_sequencer.cpp)
    target_link_libraries(kerf_core_tests PRIVATE kerf_core kerf_test_main)

    if(MSVC)
        target_compile_options(kerf_core_tests PRIVATE /utf-8 /W3)
    else()
        target_compile_options(kerf_core_tests PRIVATE -Wall -Wextra)
    endif()

    add_test(NAME kerf_core_tests COMMAND kerf_core_tests)
endif()
