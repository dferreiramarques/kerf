# Kerf for handhelds: the 640x480 pad-driven interface, the SDL2 program ("kerf") and the tests.
#
#   cmake -S handheld -B build/handheld
#   cmake --build build/handheld
#   ctest --test-dir build/handheld --output-on-failure
#
# Needs a C++20 compiler and, for the program itself, the SDL2 development files
# (Debian/Ubuntu: libsdl2-dev; MSYS2: mingw-w64-ucrt-x86_64-SDL2). Without SDL2, use
# -DKERF_BUILD_HOST=OFF: you still get the headless runner and most of the tests.

cmake_minimum_required(VERSION 3.16)
project(kerf_handheld LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(KERF_THIRD_PARTY_DIR "${CMAKE_CURRENT_LIST_DIR}/../third_party" CACHE PATH
    "Folder that holds dr_wav.h, dr_mp3.h, dr_flac.h and font8x8_*.h")

option(KERF_BUILD_HOST  "Build the SDL2 program 'kerf' (needs the SDL2 development files)" ON)
option(KERF_BUILD_TESTS "Build the tests and register them with ctest" ON)

set(KERF_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")

# The sampler engine (its tests follow KERF_BUILD_TESTS). enable_testing() has to come before
# add_subdirectory(), or ctest does not see the tests of the subdirectory.
if(KERF_BUILD_TESTS)
    enable_testing()
endif()
set(KERF_BUILD_CORE_TESTS ${KERF_BUILD_TESTS})
add_subdirectory("${KERF_REPO_ROOT}/core" "${CMAKE_CURRENT_BINARY_DIR}/core")

if(MSVC)
    set(KERF_WARNINGS /utf-8 /W3)
else()
    set(KERF_WARNINGS -Wall -Wextra)
endif()

# ------------------------------------------------------------------------------ the interface

add_library(kerf_ui STATIC
    src/app.cpp
    src/app_editor.cpp
    src/app_draw.cpp
    src/app_sequencer.cpp
    src/canvas.cpp
    src/png_writer.cpp
    src/waveform_cache.cpp
    src/input.cpp
    src/config.cpp
    src/file_browser.cpp
    src/script_runner.cpp
    src/app_record.cpp
    src/audio_input.cpp
    src/recorder.cpp
    src/record_session.cpp
    src/project_store.cpp
    src/project_keeper.cpp)

target_include_directories(kerf_ui PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(kerf_ui PRIVATE "${KERF_THIRD_PARTY_DIR}")
target_link_libraries(kerf_ui PUBLIC kerf_core)
target_compile_options(kerf_ui PRIVATE ${KERF_WARNINGS})

# Draws screens without a window or a sound card and runs test scripts (see tests/scripts).
add_executable(kerf_headless src/main_headless.cpp)
target_link_libraries(kerf_headless PRIVATE kerf_ui)
target_compile_options(kerf_headless PRIVATE ${KERF_WARNINGS})

# ---------------------------------------------------------------------------------------- SDL2

set(KERF_SDL2_TARGET "")

if(KERF_BUILD_HOST OR KERF_BUILD_TESTS)
    find_package(SDL2 CONFIG QUIET)

    if(TARGET SDL2::SDL2)
        set(KERF_SDL2_TARGET SDL2::SDL2)
    elseif(SDL2_FOUND)
        # Old SDL2 packages: variables only, no imported target.
        add_library(kerf_sdl2 INTERFACE)
        target_include_directories(kerf_sdl2 INTERFACE ${SDL2_INCLUDE_DIRS})
        target_link_libraries(kerf_sdl2 INTERFACE ${SDL2_LIBRARIES})
        set(KERF_SDL2_TARGET kerf_sdl2)
    else()
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(KERF_SDL2PKG IMPORTED_TARGET sdl2)
        endif()
        if(TARGET PkgConfig::KERF_SDL2PKG)
            set(KERF_SDL2_TARGET PkgConfig::KERF_SDL2PKG)
        endif()
    endif()

    if(NOT KERF_SDL2_TARGET)
        if(KERF_BUILD_HOST)
            message(FATAL_ERROR
                "SDL2 development files not found.\n"
                "Install them (Debian/Ubuntu: libsdl2-dev, MSYS2: mingw-w64-ucrt-x86_64-SDL2), or point "
                "CMAKE_PREFIX_PATH / SDL2_DIR at them, or configure with -DKERF_BUILD_HOST=OFF.")
        else()
            message(WARNING "SDL2 not found: the SDL input tests are skipped.")
        endif()
    endif()
endif()

# On Windows a program that uses SDL2 needs SDL2.dll next to it; copy it after linking.
function(kerf_copy_sdl_dll target)
    if(WIN32 AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.21)
        get_target_property(_kerf_sdl_type ${KERF_SDL2_TARGET} TYPE)
        if(_kerf_sdl_type STREQUAL "SHARED_LIBRARY")
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        $<TARGET_RUNTIME_DLLS:${target}> $<TARGET_FILE_DIR:${target}>
                COMMAND_EXPAND_LISTS)
        endif()
    endif()
endfunction()

if(KERF_SDL2_TARGET)
    # SDL events -> the handheld's buttons and sticks; the system's microphone for the Record screen.
    add_library(kerf_sdl_input STATIC src/sdl_input.cpp src/sdl_audio_input.cpp)
    target_link_libraries(kerf_sdl_input PUBLIC kerf_ui ${KERF_SDL2_TARGET})
    target_compile_options(kerf_sdl_input PRIVATE ${KERF_WARNINGS})
endif()

# --------------------------------------------------------------------------- the program ("kerf")

if(KERF_BUILD_HOST AND KERF_SDL2_TARGET)
    # WIN32 = no console window on Windows. The program has its own main() (SDL_MAIN_HANDLED).
    add_executable(kerf WIN32
        src/main.cpp
        src/midi_input.cpp
        src/usb_audio_input.cpp)
    target_link_libraries(kerf PRIVATE kerf_sdl_input)
    target_compile_options(kerf PRIVATE ${KERF_WARNINGS})

    if(WIN32)
        target_link_libraries(kerf PRIVATE winmm shell32)
        if(MSVC)
            target_link_options(kerf PRIVATE /ENTRY:mainCRTStartup)
        endif()
    endif()

    # Starts straight from the build folder on Windows.
    kerf_copy_sdl_dll(kerf)

    # The sample folder the file browser opens first.
    add_custom_command(TARGET kerf POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/samples" "$<TARGET_FILE_DIR:kerf>/samples")
endif()

# ----------------------------------------------------------------------------------------- tests

if(KERF_BUILD_TESTS)
    add_executable(kerf_ui_tests tests/test_ui_modules.cpp)
    target_link_libraries(kerf_ui_tests PRIVATE kerf_ui kerf_test_main)
    target_compile_options(kerf_ui_tests PRIVATE ${KERF_WARNINGS})
    add_test(NAME kerf_ui_tests COMMAND kerf_ui_tests)

    # USB MIDI without a kernel driver: descriptors, event packets and the search in sysfs.
    add_executable(kerf_usb_midi_tests tests/test_usb_midi.cpp)
    target_link_libraries(kerf_usb_midi_tests PRIVATE kerf_ui kerf_test_main)
    target_compile_options(kerf_usb_midi_tests PRIVATE ${KERF_WARNINGS})
    add_test(NAME kerf_usb_midi_tests COMMAND kerf_usb_midi_tests)

    # USB audio input without a kernel driver: descriptors (Audio Class 1 and 2), the choice of
    # format and rate, class requests, sample decoding.
    add_executable(kerf_usb_audio_tests tests/test_usb_audio.cpp)
    target_link_libraries(kerf_usb_audio_tests PRIVATE kerf_ui kerf_test_main)
    target_compile_options(kerf_usb_audio_tests PRIVATE ${KERF_WARNINGS})
    add_test(NAME kerf_usb_audio_tests COMMAND kerf_usb_audio_tests)

    # The Linux side of it (usbfs), against a pretend kernel.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(kerf_usb_io_tests tests/test_usb_midi_io.cpp src/midi_input.cpp)
        target_link_libraries(kerf_usb_io_tests PRIVATE kerf_ui kerf_test_main)
        target_compile_options(kerf_usb_io_tests PRIVATE ${KERF_WARNINGS})
        add_test(NAME kerf_usb_io_tests COMMAND kerf_usb_io_tests)

        add_executable(kerf_usb_audio_io_tests tests/test_usb_audio_io.cpp)
        target_link_libraries(kerf_usb_audio_io_tests PRIVATE kerf_ui kerf_test_main)
        target_compile_options(kerf_usb_audio_io_tests PRIVATE ${KERF_WARNINGS})
        add_test(NAME kerf_usb_audio_io_tests COMMAND kerf_usb_audio_io_tests)

        # The USB microphone as an input of the Record screen: finding it, opening it, streaming, unplugging.
        add_executable(kerf_usb_audio_input_tests tests/test_usb_audio_input.cpp src/usb_audio_input.cpp)
        target_link_libraries(kerf_usb_audio_input_tests PRIVATE kerf_ui kerf_test_main)
        target_compile_options(kerf_usb_audio_input_tests PRIVATE ${KERF_WARNINGS})
        add_test(NAME kerf_usb_audio_input_tests COMMAND kerf_usb_audio_input_tests)
    endif()

    # The Record screen's parts: the ring of frames, the choice of channels, the meters, the take and
    # its WAV file, the hub of inputs and the session (all with a pretend input).
    add_executable(kerf_recording_tests tests/test_recording.cpp)
    target_link_libraries(kerf_recording_tests PRIVATE kerf_ui kerf_test_main)
    target_compile_options(kerf_recording_tests PRIVATE ${KERF_WARNINGS})
    add_test(NAME kerf_recording_tests COMMAND kerf_recording_tests)

    # The Record screen itself: buttons in, takes saved in a folder and loaded into the slicer.
    add_executable(kerf_app_record_tests tests/test_app_record.cpp)
    target_link_libraries(kerf_app_record_tests PRIVATE kerf_ui kerf_test_main)
    target_compile_options(kerf_app_record_tests PRIVATE ${KERF_WARNINGS})
    add_test(NAME kerf_app_record_tests COMMAND kerf_app_record_tests)

    # Projects (the .kerf file next to a sample): the files on disk and how a save is made safe, ...
    add_executable(kerf_project_store_tests tests/test_project_store.cpp)
    target_link_libraries(kerf_project_store_tests PRIVATE kerf_ui kerf_test_main)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # The tests watch fsync and rename and make the calls that write fail, through the linker's --wrap; see tests/store_hooks.h.
        target_sources(kerf_project_store_tests PRIVATE tests/store_hooks.cpp)
        target_link_options(kerf_project_store_tests PRIVATE -Wl,--wrap=fwrite -Wl,--wrap=fflush -Wl,--wrap=fsync -Wl,--wrap=fclose -Wl,--wrap=rename)
        target_compile_definitions(kerf_project_store_tests PRIVATE KERF_STORE_HOOKS=1)
    endif()
    target_compile_options(kerf_project_store_tests PRIVATE ${KERF_WARNINGS})
    add_test(NAME kerf_project_store_tests COMMAND kerf_project_store_tests)

    # ... when the work is saved, brought back, kept as a backup or set aside (with a clock of its own), ...
    add_executable(kerf_project_keeper_tests tests/test_project_keeper.cpp)
    target_link_libraries(kerf_project_keeper_tests PRIVATE kerf_ui kerf_test_main)
    target_compile_options(kerf_project_keeper_tests PRIVATE ${KERF_WARNINGS})
    add_test(NAME kerf_project_keeper_tests COMMAND kerf_project_keeper_tests)

    # ... and the application as a person uses it: restore on load, autosave, the header, Settings, the library.
    add_executable(kerf_app_project_tests tests/test_app_project.cpp)
    target_link_libraries(kerf_app_project_tests PRIVATE kerf_ui kerf_test_main)
    target_compile_options(kerf_app_project_tests PRIVATE ${KERF_WARNINGS})
    add_test(NAME kerf_app_project_tests COMMAND kerf_app_project_tests)

    if(KERF_SDL2_TARGET)
        add_executable(kerf_sdl_tests tests/test_sdl_input.cpp tests/test_sdl_audio_input.cpp)
        target_link_libraries(kerf_sdl_tests PRIVATE kerf_sdl_input kerf_minitest)
        target_compile_options(kerf_sdl_tests PRIVATE ${KERF_WARNINGS})
        kerf_copy_sdl_dll(kerf_sdl_tests)
        add_test(NAME kerf_sdl_tests COMMAND kerf_sdl_tests)
        # Virtual joysticks only: no window, no sound card.
        set_tests_properties(kerf_sdl_tests PROPERTIES
            ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
    endif()

    # End-to-end scripts: they load the demo sample, press buttons and push sticks, and check the
    # result. They use paths relative to the repository root.
    foreach(_script slices_and_undo stick_editing menus_and_dialogs quit_combo pages_and_sequencer sequencer_follows_slices)
        add_test(NAME script_${_script}
            COMMAND kerf_headless --script "${CMAKE_CURRENT_SOURCE_DIR}/tests/scripts/${_script}.txt"
            WORKING_DIRECTORY "${KERF_REPO_ROOT}")
    endforeach()

    # The Record screen's scripts save takes, so each one gets a folder of its own that starts out
    # empty (run_in_empty_folder.cmake). The first two record from pretend inputs.
    foreach(_script record_and_load record_inputs_and_channels record_without_input)
        set(_options "")
        if(NOT _script STREQUAL "record_without_input")
            set(_options "--fake-input")
        endif()
        add_test(NAME script_${_script}
            COMMAND ${CMAKE_COMMAND}
                    "-DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}"
                    "-DHEADLESS=$<TARGET_FILE:kerf_headless>"
                    "-DSCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/scripts/${_script}.txt"
                    "-DFOLDER=${CMAKE_CURRENT_BINARY_DIR}/script-folders/${_script}"
                    "-DOPTIONS=${_options}"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/scripts/run_in_empty_folder.cmake"
            WORKING_DIRECTORY "${KERF_REPO_ROOT}")
    endforeach()

    # Projects, from the first take to a second run of Kerf: the first script records a sample and works
    # on it, the second (a new program, in the same folder) must find all of it back. The last one runs
    # with the projects off, as the other scripts do, and must leave nothing behind.
    add_test(NAME script_project_save_and_restore
        COMMAND ${CMAKE_COMMAND}
                "-DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}"
                "-DHEADLESS=$<TARGET_FILE:kerf_headless>"
                "-DSCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/scripts/project_save.txt"
                "-DSCRIPT2=${CMAKE_CURRENT_SOURCE_DIR}/tests/scripts/project_restore.txt"
                "-DFOLDER=${CMAKE_CURRENT_BINARY_DIR}/script-folders/project_save_and_restore"
                "-DOPTIONS=--fake-input;--projects"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/scripts/run_in_empty_folder.cmake"
        WORKING_DIRECTORY "${KERF_REPO_ROOT}")

    # The disk will not take the project: the folder the test makes where the part file goes makes the save fail.
    add_test(NAME script_project_problem
        COMMAND ${CMAKE_COMMAND}
                "-DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}"
                "-DHEADLESS=$<TARGET_FILE:kerf_headless>"
                "-DSCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/scripts/project_problem.txt"
                "-DFOLDER=${CMAKE_CURRENT_BINARY_DIR}/script-folders/project_problem"
                "-DOPTIONS=--fake-input;--projects"
                "-DMAKE_FOLDERS=rec-001.wav.kerf.tmp"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/scripts/run_in_empty_folder.cmake"
        WORKING_DIRECTORY "${KERF_REPO_ROOT}")

    # The checks of the runner itself: a wrong check must fail the run, and say so (FAILS_WITH).
    foreach(_case
            "wrong_project_check|the project is 'none', wanted saved"
            "wrong_file_check_missing|file nothing-like-this.kerf does not exist"
            "wrong_file_check_there|file demo_beat.wav exists")
        string(REPLACE "|" ";" _parts "${_case}")
        list(GET _parts 0 _script)
        list(GET _parts 1 _regex)
        add_test(NAME script_${_script}_fails
            COMMAND ${CMAKE_COMMAND}
                    "-DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}"
                    "-DHEADLESS=$<TARGET_FILE:kerf_headless>"
                    "-DSCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/scripts/${_script}.txt"
                    "-DFOLDER=${CMAKE_CURRENT_BINARY_DIR}/script-folders/${_script}"
                    "-DFAILS_WITH=${_regex}"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/scripts/run_in_empty_folder.cmake"
            WORKING_DIRECTORY "${KERF_REPO_ROOT}")
    endforeach()

    add_test(NAME script_project_off
        COMMAND ${CMAKE_COMMAND}
                "-DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}"
                "-DHEADLESS=$<TARGET_FILE:kerf_headless>"
                "-DSCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/scripts/project_off.txt"
                "-DFOLDER=${CMAKE_CURRENT_BINARY_DIR}/script-folders/project_off"
                "-DOPTIONS=--fake-input"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/scripts/run_in_empty_folder.cmake"
        WORKING_DIRECTORY "${KERF_REPO_ROOT}")
endif()
