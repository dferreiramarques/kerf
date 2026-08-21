cmake_minimum_required(VERSION 3.22)

project(KerfPlugin VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.15 # latest 8.x release as of 2026-08-21; JUCE 9.0.x also exists but wasn't in scope for this port — bump deliberately, not by default
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(JUCE)

juce_add_plugin(Kerf
    COMPANY_NAME "Monco"
    PRODUCT_NAME "Kerf Lite (Beta)"
    PLUGIN_NAME "Kerf Lite (Beta)"
    DESCRIPTION "Free sample slicer instrument plugin"
    PLUGIN_MANUFACTURER_CODE Monc
    PLUGIN_CODE Kerf
    FORMATS VST3
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    COPY_PLUGIN_AFTER_BUILD TRUE
    VST3_CATEGORIES Instrument Sampler
)

target_sources(Kerf PRIVATE
    src/PluginProcessor.cpp
    src/PluginEditor.cpp
    src/Voice.cpp
    src/ParameterLayout.cpp
    src/WaveformDisplay.cpp
    src/Persistence/StatePersistence.cpp
)

target_compile_definitions(Kerf PUBLIC
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    JUCE_VST3_CAN_REPLACE_VST2=0
)

# No juce_gui_extra / WebBrowserComponent - the UI is plain native JUCE widgets (see the plan's
# 2026-08-21 update on why the WebView2 route was dropped after Phase 1). That also means no
# WebView2 NuGet package prerequisite for anyone building this fresh.
target_link_libraries(Kerf
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_formats
        juce::juce_audio_processors
        juce::juce_dsp
        juce::juce_gui_basics
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

# Console-only test runner for the engine's juce::UnitTest suite (SliceBank/Voice/VoiceEngine/
# TransientDetector) - no DAW or plugin host involved, just `cmake --build build --target
# KerfTests` then run the resulting exe. See tests/Main.cpp.
juce_add_console_app(KerfTests
    PRODUCT_NAME "KerfTests"
)

target_sources(KerfTests PRIVATE
    tests/Main.cpp
    tests/EngineTests.cpp
    tests/PersistenceTests.cpp
    src/Voice.cpp
    src/ParameterLayout.cpp
    src/Persistence/StatePersistence.cpp
)

target_compile_definitions(KerfTests PRIVATE
    JUCE_STANDALONE_APPLICATION=1
)

target_link_libraries(KerfTests
    PRIVATE
        juce::juce_core
        juce::juce_audio_basics
        juce::juce_audio_formats
        juce::juce_audio_processors
        juce::juce_dsp
        juce::juce_events
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)
