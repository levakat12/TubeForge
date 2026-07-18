cmake_minimum_required(VERSION 3.22)

project(TubeForge VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.13
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(JUCE)

juce_add_plugin(TubeForge
    COMPANY_NAME "TubeForge Audio"
    BUNDLE_ID com.tubeforge.audio
    PLUGIN_MANUFACTURER_CODE Tfge
    PLUGIN_CODE Tfg1
    FORMATS VST3 Standalone
    PRODUCT_NAME "TubeForge"
    DESCRIPTION "Neural guitar and bass amplifier"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES Fx Distortion
)

juce_generate_juce_header(TubeForge)

target_sources(TubeForge PRIVATE
    Source/PluginEditor.cpp
    Source/PluginEditor.h
    Source/PluginProcessor.cpp
    Source/PluginProcessor.h
    Source/dsp/AmpEngine.cpp
    Source/dsp/AmpEngine.h
    Source/dsp/Biquad.h
    Source/dsp/RecurrentAmpModel.h
)

target_compile_definitions(TubeForge PUBLIC
    JUCE_REPORT_APP_USAGE=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_WEB_BROWSER=0
)

target_link_libraries(TubeForge PRIVATE
    juce::juce_audio_utils
    juce::juce_dsp
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
)

option(TUBEFORGE_BUILD_TESTS "Build TubeForge DSP tests" ON)

if(TUBEFORGE_BUILD_TESTS)
    enable_testing()

    juce_add_console_app(TubeForgeTests PRODUCT_NAME "TubeForge DSP Tests")

    target_sources(TubeForgeTests PRIVATE
        Tests/AmpEngineTests.cpp
        Source/dsp/AmpEngine.cpp
        Source/dsp/AmpEngine.h
        Source/dsp/Biquad.h
        Source/dsp/RecurrentAmpModel.h
    )

    target_include_directories(TubeForgeTests PRIVATE Source)

    target_compile_definitions(TubeForgeTests PRIVATE
        JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0
    )

    target_link_libraries(TubeForgeTests PRIVATE
        juce::juce_audio_basics
        juce::juce_dsp
        PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
    )

    add_test(NAME TubeForgeDSP COMMAND TubeForgeTests)
endif()
