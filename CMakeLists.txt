cmake_minimum_required(VERSION 3.22)

project(TubeForge VERSION 0.3.0 LANGUAGES CXX)

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

add_library(nts_audio_core STATIC
    engine/audio-core/src/CoreEngine.cpp
)
target_include_directories(nts_audio_core PUBLIC engine/audio-core/include)

add_library(nts_dsp STATIC
    engine/dsp/src/Analysis.cpp
    engine/dsp/src/BassSplit.cpp
    engine/dsp/src/Convolution.cpp
    engine/dsp/src/Dynamics.cpp
    engine/dsp/src/Filters.cpp
    engine/dsp/src/Metering.cpp
    engine/dsp/src/Oversampling.cpp
    engine/dsp/src/Regression.cpp
    engine/dsp/src/Simd.cpp
)
target_include_directories(nts_dsp PUBLIC engine/dsp/include)

add_library(nts_diagnostics STATIC
    engine/diagnostics/src/DiagnosticsCollector.cpp
    engine/diagnostics/src/ProcessMemory.cpp
    engine/diagnostics/src/StructuredLogger.cpp
)
target_include_directories(nts_diagnostics PUBLIC engine/diagnostics/include)
if(MSVC)
    target_compile_options(nts_diagnostics PUBLIC /wd4324)
endif()
if(WIN32)
    target_link_libraries(nts_diagnostics PRIVATE psapi)
endif()

add_library(nts_state STATIC
    engine/state/src/ProjectState.cpp
    engine/state/src/SettingsStore.cpp
)
target_include_directories(nts_state PUBLIC engine/state/include)
target_link_libraries(nts_state PUBLIC juce::juce_core)

add_library(nts_ir STATIC
    engine/ir/src/CabinetIrLoader.cpp
)
target_include_directories(nts_ir PUBLIC engine/ir/include)
target_link_libraries(nts_ir PUBLIC nts_dsp nts_diagnostics juce::juce_audio_formats)

foreach(target nts_audio_core nts_dsp nts_diagnostics nts_state nts_ir)
    target_link_libraries(${target} PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
    )
endforeach()

juce_add_plugin(TubeForge
    COMPANY_NAME "TubeForge Audio"
    BUNDLE_ID com.tubeforge.audio
    PLUGIN_MANUFACTURER_CODE Tfge
    PLUGIN_CODE Tfg1
    FORMATS VST3
    PRODUCT_NAME "TubeForge"
    DESCRIPTION "Real-time guitar and bass tone platform"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES Fx
)

juce_generate_juce_header(TubeForge)

target_sources(TubeForge PRIVATE
    Source/PluginEditor.cpp
    Source/PluginEditor.h
    Source/PluginProcessor.cpp
    Source/PluginProcessor.h
)

target_compile_definitions(TubeForge PUBLIC
    JUCE_REPORT_APP_USAGE=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_WEB_BROWSER=0
)

option(TUBEFORGE_ENABLE_ASIO "Enable ASIO in Windows standalone builds" OFF)
set(TUBEFORGE_ASIO_SDK_PATH "" CACHE PATH
    "Optional external Steinberg ASIO SDK root; empty uses JUCE's bundled ASIO headers")
# Plug-ins always use the host's device; ASIO belongs only to the standalone target.
target_compile_definitions(TubeForge PUBLIC JUCE_ASIO=0 JUCE_ASIO_USE_EXTERNAL_SDK=0)

function(tubeforge_configure_asio target)
    if(TUBEFORGE_ENABLE_ASIO AND WIN32)
        target_compile_definitions(${target} PRIVATE JUCE_ASIO=1)
        if(TUBEFORGE_ASIO_SDK_PATH)
            if(NOT EXISTS "${TUBEFORGE_ASIO_SDK_PATH}/common/iasiodrv.h")
                message(FATAL_ERROR
                    "TUBEFORGE_ASIO_SDK_PATH must contain common/iasiodrv.h")
            endif()
            target_compile_definitions(${target} PRIVATE JUCE_ASIO_USE_EXTERNAL_SDK=1)
            target_include_directories(${target} PRIVATE
                "${TUBEFORGE_ASIO_SDK_PATH}/common"
                "${TUBEFORGE_ASIO_SDK_PATH}/host"
                "${TUBEFORGE_ASIO_SDK_PATH}/host/pc"
            )
        else()
            target_compile_definitions(${target} PRIVATE JUCE_ASIO_USE_EXTERNAL_SDK=0)
        endif()
    else()
        target_compile_definitions(${target} PRIVATE JUCE_ASIO=0 JUCE_ASIO_USE_EXTERNAL_SDK=0)
    endif()
endfunction()

target_link_libraries(TubeForge PRIVATE
    nts_audio_core
    nts_diagnostics
    nts_state
    juce::juce_audio_utils
    juce::juce_dsp
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
)

juce_add_gui_app(nts_standalone_app
    PRODUCT_NAME "TubeForge"
    COMPANY_NAME "TubeForge Audio"
    BUNDLE_ID com.tubeforge.audio.standalone
)
target_sources(nts_standalone_app PRIVATE
    apps/standalone/Main.cpp
    Source/PluginEditor.cpp
    Source/PluginEditor.h
    Source/PluginProcessor.cpp
    Source/PluginProcessor.h
)
target_include_directories(nts_standalone_app PRIVATE Source)
target_compile_definitions(nts_standalone_app PRIVATE
    JucePlugin_Name="TubeForge"
    JUCE_REPORT_APP_USAGE=0
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
)
target_link_libraries(nts_standalone_app PRIVATE
    nts_audio_core
    nts_diagnostics
    nts_state
    juce::juce_audio_utils
    juce::juce_dsp
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
)
tubeforge_configure_asio(nts_standalone_app)

add_custom_target(nts_vst3 DEPENDS TubeForge_VST3)
add_custom_target(nts_standalone DEPENDS nts_standalone_app)

option(TUBEFORGE_BUILD_TESTS "Build TubeForge test and benchmark targets" ON)
option(TUBEFORGE_ENABLE_HOUR_SOAK_TEST "Register the one-hour wall-clock soak test with CTest" OFF)

if(TUBEFORGE_BUILD_TESTS)
    enable_testing()

    juce_add_console_app(nts_unit_tests PRODUCT_NAME "TubeForge Unit Tests")
    target_sources(nts_unit_tests PRIVATE Tests/UnitTests.cpp)
    target_include_directories(nts_unit_tests PRIVATE Tests)
    target_link_libraries(nts_unit_tests PRIVATE
        nts_audio_core
        nts_diagnostics
        nts_state
        juce::juce_audio_basics
    )

    juce_add_console_app(nts_integration_tests PRODUCT_NAME "TubeForge Integration Tests")
    target_sources(nts_integration_tests PRIVATE Tests/IntegrationTests.cpp)
    target_include_directories(nts_integration_tests PRIVATE Tests)
    if(MSVC)
        target_compile_definitions(nts_integration_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    target_link_libraries(nts_integration_tests PRIVATE
        nts_audio_core
        nts_diagnostics
        nts_state
        juce::juce_audio_basics
    )

    juce_add_console_app(nts_device_tests PRODUCT_NAME "TubeForge Device Tests")
    target_sources(nts_device_tests PRIVATE Tests/DeviceManagerTests.cpp)
    target_include_directories(nts_device_tests PRIVATE Tests)
    target_link_libraries(nts_device_tests PRIVATE
        juce::juce_audio_devices
        juce::juce_gui_basics
    )

    if(TUBEFORGE_ENABLE_ASIO AND WIN32)
        juce_add_console_app(nts_asio_tests PRODUCT_NAME "TubeForge ASIO Tests")
        target_sources(nts_asio_tests PRIVATE Tests/AsioTests.cpp)
        target_include_directories(nts_asio_tests PRIVATE Tests)
        tubeforge_configure_asio(nts_asio_tests)
        target_link_libraries(nts_asio_tests PRIVATE juce::juce_audio_devices)
        add_test(NAME nts_asio_tests COMMAND nts_asio_tests)
    endif()

    juce_add_console_app(nts_wrapper_tests PRODUCT_NAME "TubeForge Wrapper Tests")
    target_sources(nts_wrapper_tests PRIVATE
        Tests/WrapperTests.cpp
        Source/PluginEditor.cpp
        Source/PluginEditor.h
        Source/PluginProcessor.cpp
        Source/PluginProcessor.h
    )
    target_include_directories(nts_wrapper_tests PRIVATE Tests Source)
    target_compile_definitions(nts_wrapper_tests PRIVATE
        JucePlugin_Name="TubeForge"
        JUCE_REPORT_APP_USAGE=0
        JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0
    )
    target_link_libraries(nts_wrapper_tests PRIVATE
        nts_audio_core
        nts_diagnostics
        nts_state
        juce::juce_audio_utils
        juce::juce_dsp
    )

    juce_add_console_app(nts_vst3_host_tests PRODUCT_NAME "TubeForge VST3 Host Tests")
    target_sources(nts_vst3_host_tests PRIVATE Tests/VST3HostTests.cpp)
    target_include_directories(nts_vst3_host_tests PRIVATE Tests)
    target_compile_definitions(nts_vst3_host_tests PRIVATE
        JUCE_PLUGINHOST_VST3=1
        JUCE_PLUGINHOST_VST=0
        JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0
    )
    target_link_libraries(nts_vst3_host_tests PRIVATE juce::juce_audio_utils)
    add_dependencies(nts_vst3_host_tests TubeForge_VST3)

    juce_add_console_app(nts_soak_tests PRODUCT_NAME "TubeForge Soak Tests")
    target_sources(nts_soak_tests PRIVATE Tests/SoakTests.cpp)
    target_include_directories(nts_soak_tests PRIVATE Tests)
    target_link_libraries(nts_soak_tests PRIVATE nts_audio_core)

    juce_add_console_app(nts_dsp_tests PRODUCT_NAME "TubeForge DSP Tests")
    target_sources(nts_dsp_tests PRIVATE Tests/DspTests.cpp)
    target_include_directories(nts_dsp_tests PRIVATE Tests)
    if(MSVC)
        target_compile_definitions(nts_dsp_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    target_link_libraries(nts_dsp_tests PRIVATE nts_dsp)

    juce_add_console_app(nts_ir_tests PRODUCT_NAME "TubeForge IR Tests")
    target_sources(nts_ir_tests PRIVATE Tests/IrTests.cpp)
    target_include_directories(nts_ir_tests PRIVATE Tests)
    target_link_libraries(nts_ir_tests PRIVATE nts_ir juce::juce_audio_formats)

    juce_add_console_app(nts_dsp_benchmarks PRODUCT_NAME "TubeForge DSP Benchmarks")
    target_sources(nts_dsp_benchmarks PRIVATE Tests/DspBenchmarks.cpp)
    target_include_directories(nts_dsp_benchmarks PRIVATE Tests)
    target_link_libraries(nts_dsp_benchmarks PRIVATE nts_dsp)

    add_test(NAME nts_unit_tests COMMAND nts_unit_tests)
    add_test(NAME nts_integration_tests COMMAND nts_integration_tests)
    add_test(NAME nts_device_tests COMMAND nts_device_tests)
    add_test(NAME nts_wrapper_tests COMMAND nts_wrapper_tests)
    add_test(NAME nts_vst3_host_tests COMMAND nts_vst3_host_tests
             "${CMAKE_BINARY_DIR}/TubeForge_artefacts/$<CONFIG>/VST3/TubeForge.vst3")
    add_test(NAME nts_dsp_tests COMMAND nts_dsp_tests)
    add_test(NAME nts_ir_tests COMMAND nts_ir_tests)
    add_test(NAME nts_dsp_benchmark_acceptance COMMAND nts_dsp_benchmarks)
    if(TUBEFORGE_ENABLE_HOUR_SOAK_TEST)
        add_test(NAME nts_hour_soak COMMAND nts_soak_tests 3600)
        set_tests_properties(nts_hour_soak PROPERTIES LABELS soak TIMEOUT 3900)
    endif()
endif()
