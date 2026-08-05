cmake_minimum_required(VERSION 3.22)

# Refuse to reconfigure an existing build tree with a different CMake than built it.
#
# Two CMake installations are common on a Windows dev box -- one per Visual Studio -- and whichever
# is first on PATH wins. Building with the wrong one is harmless until something forces a
# reconfigure, at which point compiler re-detection fails against the cached toolchain, the
# generator cannot regenerate its project files, and `cmake --build` goes on reporting success while
# silently building nothing new. That failure mode cost real debugging time: source files newer than
# their own object files, a plug-in binary missing changes that had definitely been compiled, and a
# UI bug that looked unfixed because the fix was never in the binary.
#
# Checked before `project()` on purpose: that is where the re-detection blows up, so a guard after it
# never runs. Cache variables are loaded before this file is processed, so the comparison is
# available this early.
if(DEFINED CMAKE_CACHE_MAJOR_VERSION)
    set(TUBEFORGE_CACHED_CMAKE
        "${CMAKE_CACHE_MAJOR_VERSION}.${CMAKE_CACHE_MINOR_VERSION}.${CMAKE_CACHE_PATCH_VERSION}")
    if(NOT TUBEFORGE_CACHED_CMAKE VERSION_EQUAL CMAKE_VERSION)
        message(FATAL_ERROR
            "This build tree was configured by CMake ${TUBEFORGE_CACHED_CMAKE}, but CMake "
            "${CMAKE_VERSION} is running (${CMAKE_COMMAND}).\n"
            "Reconfiguring across versions fails on compiler re-detection and leaves the tree "
            "building stale objects while still reporting success.\n"
            "Either build with the CMake that configured this tree, or delete "
            "${CMAKE_BINARY_DIR}/CMakeCache.txt and configure again from scratch with the one you "
            "want.")
    endif()
endif()

project(TubeForge VERSION 0.10.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The project version, reachable from code. `JucePlugin_VersionString` exists only on targets
# juce_add_plugin created, so the shell sources -- which are also compiled into the wrapper tests
# and the standalone app -- cannot use it. Defined globally because a version string costs nothing
# and a per-target definition is the kind of thing that gets missed when a target is added.
add_compile_definitions(TUBEFORGE_VERSION_STRING="${PROJECT_VERSION}")

if(MSVC)
    # Keep optimized Release binaries while emitting separate PDBs for crash-symbol processing.
    add_compile_options($<$<CONFIG:Release>:/Zi>)
    add_link_options($<$<CONFIG:Release>:/DEBUG:FULL>)
endif()

# /W4 is already in force everywhere through juce::juce_recommended_warning_flags; this only
# decides whether those warnings stop the build. Off by default so local work is not blocked
# mid-edit, on in CI so nothing new lands. Scoped to this project's own sources: JUCE vendors
# third-party C (libvorbis, and the FLAC and Ogg codecs) that does not build warning-clean and
# is not ours to fix.
option(TUBEFORGE_WARNINGS_AS_ERRORS "Treat compiler warnings in TubeForge sources as errors" OFF)

function(tubeforge_set_warning_policy target)
    if(NOT TUBEFORGE_WARNINGS_AS_ERRORS)
        return()
    endif()
    if(MSVC)
        target_compile_options(${target} PRIVATE /WX)
    else()
        target_compile_options(${target} PRIVATE -Werror)
    endif()
endfunction()

# The highest-yield check available for this codebase: lock-free ring buffers, double-buffered
# atomic slot swaps in the neural and circuit runtimes, raw pointer arrays into vector storage
# throughout the DSP, and hand-written SSE2 kernels. Any of those can be off by one without a
# single test failing.
#
# Build with a Release configuration. MSVC's Address Sanitizer cannot be combined with the /RTC
# runtime checks that Debug turns on, nor with the whole-program optimisation behind
# juce_recommended_lto_flags -- which is why only the test targets are built in this mode, since
# they are the targets that do not link those flags. MSVC has no UndefinedBehaviorSanitizer; a
# clang leg would be the place to add -fsanitize=undefined.
option(TUBEFORGE_ENABLE_SANITIZERS "Build with Address Sanitizer (Release configurations only)" OFF)
if(TUBEFORGE_ENABLE_SANITIZERS)
    if(MSVC)
        add_compile_options(/fsanitize=address)
        # ASan and incremental linking are mutually exclusive.
        add_link_options(/INCREMENTAL:NO)
        # An ASan-instrumented binary loads clang_rt.asan_dynamic at startup and dies with
        # STATUS_DLL_NOT_FOUND (0xc0000135) if it is not beside the executable or on PATH.
        # It ships next to cl.exe, so copy it rather than making every caller fix PATH.
        get_filename_component(TUBEFORGE_MSVC_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
        find_file(TUBEFORGE_ASAN_RUNTIME clang_rt.asan_dynamic-x86_64.dll
                  HINTS "${TUBEFORGE_MSVC_BIN}" NO_DEFAULT_PATH)
        if(NOT TUBEFORGE_ASAN_RUNTIME)
            message(WARNING "Address Sanitizer runtime not found beside the compiler; "
                            "sanitized executables will not start unless it is on PATH.")
        endif()
    else()
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    endif()
endif()

# Places the sanitizer runtime beside a target's executable. A no-op unless sanitizers are on.
function(tubeforge_stage_sanitizer_runtime target)
    if(NOT TUBEFORGE_ENABLE_SANITIZERS OR NOT MSVC OR NOT TUBEFORGE_ASAN_RUNTIME)
        return()
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${TUBEFORGE_ASAN_RUNTIME}" "$<TARGET_FILE_DIR:${target}>")
endfunction()

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
    engine/dsp/src/Effects.cpp
    engine/dsp/src/Filters.cpp
    engine/dsp/src/Metering.cpp
    engine/dsp/src/Oversampling.cpp
    engine/dsp/src/PitchDetector.cpp
    engine/dsp/src/Regression.cpp
    engine/dsp/src/Simd.cpp
    engine/dsp/src/SimdAvx2.cpp
)
target_include_directories(nts_dsp PUBLIC engine/dsp/include)

# The AVX2 kernel, and only it, is compiled at a wider instruction set. The rest of the engine
# stays at the SSE2 baseline, which is the floor this plug-in has to keep running on -- raising it
# globally would trade a crash on older hardware for a speedup nobody asked for.
#
# The guard is at run time, not build time: Simd.cpp checks CPUID and the OS's YMM state before it
# will dispatch here. That is why this file must hold exactly one function and no static state --
# anything with an initialiser in this translation unit would execute before that check.
if(MSVC)
    set_source_files_properties(engine/dsp/src/SimdAvx2.cpp PROPERTIES
        COMPILE_OPTIONS "/arch:AVX2;/DNTS_BUILD_AVX2")
else()
    set_source_files_properties(engine/dsp/src/SimdAvx2.cpp PROPERTIES
        COMPILE_OPTIONS "-mavx2;-mfma;-DNTS_BUILD_AVX2")
endif()
# Simd.cpp needs to know a real AVX2 kernel exists before it will dispatch to one, but must not
# itself be compiled wide.
set_source_files_properties(engine/dsp/src/Simd.cpp PROPERTIES
    COMPILE_DEFINITIONS "NTS_BUILD_AVX2")

add_library(nts_amp STATIC
    engine/amp/src/TraditionalAmp.cpp
)
target_include_directories(nts_amp PUBLIC engine/amp/include)
target_link_libraries(nts_amp PUBLIC nts_dsp)

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

add_library(nts_ml_runtime STATIC
    engine/ml-runtime/src/PackedTanhModel.cpp
    engine/ml-runtime/src/PackedWaveNetModel.cpp
    engine/ml-runtime/src/NeuralModel.cpp
    engine/ml-runtime/src/NeuralAmpProcessor.cpp
)
target_include_directories(nts_ml_runtime PUBLIC engine/ml-runtime/include)
# For the shared SIMD kernels. Deliberately a link rather than an inline header: the runtime
# instruction-set dispatch this is heading for needs these kernels in translation units of their
# own so a second copy can be compiled at a wider instruction set. Every consumer of
# nts_ml_runtime already has nts_dsp in its dependency graph.
target_link_libraries(nts_ml_runtime PUBLIC nts_dsp)

add_library(nts_nam STATIC
    engine/nam/src/Capture.cpp
    engine/nam/src/CaptureLibrary.cpp
)
target_include_directories(nts_nam PUBLIC engine/nam/include)
target_link_libraries(nts_nam PUBLIC nts_ml_runtime juce::juce_core juce::juce_cryptography)

add_library(nts_pedals STATIC
    engine/pedals/src/PedalBoard.cpp
)
target_include_directories(nts_pedals PUBLIC engine/pedals/include)
target_link_libraries(nts_pedals PUBLIC nts_dsp nts_ml_runtime)

add_library(nts_circuit STATIC
    engine/circuit/src/CircuitTypes.cpp
    engine/circuit/src/Components.cpp
    engine/circuit/src/CircuitGraph.cpp
    engine/circuit/src/CircuitProcessor.cpp
)
target_include_directories(nts_circuit PUBLIC engine/circuit/include)
target_link_libraries(nts_circuit PUBLIC nts_dsp nts_ml_runtime juce::juce_core)

add_library(nts_tone_analysis STATIC
    engine/tone-analysis/src/ToneAnalysis.cpp
    engine/tone-analysis/src/ToneProfileDatabase.cpp
)
target_include_directories(nts_tone_analysis PUBLIC engine/tone-analysis/include)
target_link_libraries(nts_tone_analysis PUBLIC nts_dsp juce::juce_core)

add_library(nts_reconstruction STATIC
    engine/reconstruction/src/SourceReconstruction.cpp
    engine/reconstruction/src/StemRefinement.cpp
)
target_include_directories(nts_reconstruction PUBLIC engine/reconstruction/include)
target_link_libraries(nts_reconstruction PUBLIC nts_amp nts_tone_analysis juce::juce_core)

add_library(nts_assistant STATIC
    engine/assistant/src/ToneAssistant.cpp
)
target_include_directories(nts_assistant PUBLIC engine/assistant/include)
target_link_libraries(nts_assistant PUBLIC nts_amp nts_tone_analysis nts_diagnostics juce::juce_core)

add_library(nts_ecosystem STATIC
    engine/ecosystem/src/TonePackage.cpp
    engine/ecosystem/src/ReleaseServices.cpp
    engine/ecosystem/src/PackageSignature.cpp
)
target_include_directories(nts_ecosystem PUBLIC engine/ecosystem/include)
target_link_libraries(nts_ecosystem PUBLIC nts_amp nts_ml_runtime juce::juce_cryptography)

foreach(target nts_audio_core nts_dsp nts_amp nts_diagnostics nts_state nts_ir nts_ml_runtime nts_nam nts_pedals nts_circuit nts_tone_analysis nts_reconstruction nts_assistant nts_ecosystem)
    target_link_libraries(${target} PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
    )
    # Whole-program optimisation is deliberately NOT applied here, having been tried and measured.
    #
    # The hypothesis was that /GL would let dsp::dotProduct inline into its callers and remove the
    # cross-translation-unit call that forces that kernel to carry a length threshold. Measured on
    # the DSP, amplifier and ML benchmarks, the result was inside run-to-run noise: one amplifier
    # configuration appeared 9% faster and another 7% slower across rebuilds. It is not a win this
    # harness can resolve, and it costs build time on every target, because these libraries are
    # linked PUBLIC and the flag would propagate to all twenty-three test executables.
    #
    # It also cannot simply be switched on: MSVC's Address Sanitizer cannot be combined with /GL,
    # and the ASan configuration works by building the test targets against these libraries
    # precisely because they do not carry the plug-in's whole-program flags. Any future attempt
    # needs the `if(NOT TUBEFORGE_ENABLE_SANITIZERS)` guard and a quieter machine to measure on.
    tubeforge_set_warning_policy(${target})
endforeach()

juce_add_binary_data(TubeForgeAssets
    HEADER_NAME TubeForgeAssets.h
    NAMESPACE TubeForgeAssets
    SOURCES Assets/tubeforge_amp_faceplate.png
)

juce_add_plugin(TubeForge
    COMPANY_NAME "TubeForge Audio"
    BUNDLE_ID com.tubeforge.audio
    PLUGIN_MANUFACTURER_CODE Tfge
    PLUGIN_CODE Tfg1
    FORMATS VST3
    PRODUCT_NAME "TubeForge"
    DESCRIPTION "Real-time guitar and bass tone platform"
    IS_SYNTH FALSE
    # MIDI in only, and only for program change: guitarists switch presets with a foot
    # controller, and a host will not deliver those messages unless the plug-in declares it.
    NEEDS_MIDI_INPUT TRUE
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
    Source/TubeForgeTheme.h
    Source/TubeForgeLookAndFeel.cpp
    Source/TubeForgeLookAndFeel.h
    Source/ui/ModulePage.h
    Source/ui/UiSupport.cpp
    Source/ui/UiSupport.h
    Source/ui/ShellWidgets.cpp
    Source/ui/ShellWidgets.h
    Source/ui/AmplifierPage.cpp
    Source/ui/AmplifierPage.h
    Source/ui/FaceplateArt.cpp
    Source/ui/FaceplateArt.h
    Source/ui/GearPicker.cpp
    Source/ui/GearPicker.h
    Source/ui/ToneShapingPage.cpp
    Source/ui/ToneShapingPage.h
    Source/ui/NeuralCapturePage.cpp
    Source/ui/NeuralCapturePage.h
    Source/ui/PedalArt.cpp
    Source/ui/PedalArt.h
    Source/ui/PedalboardPage.cpp
    Source/ui/PedalboardPage.h
    Source/ui/CapturePicker.cpp
    Source/ui/CapturePicker.h
    Source/ui/CapturesPage.cpp
    Source/ui/CapturesPage.h
    Source/ui/ToneAssistantPage.cpp
    Source/ui/ToneAssistantPage.h
    Source/ui/ProfileLibraryPage.cpp
    Source/ui/ProfileLibraryPage.h
    Source/ui/CabinetPage.cpp
    Source/ui/CabinetPage.h
    Source/ui/TunerPage.cpp
    Source/ui/TunerPage.h
    Source/ui/CircuitPage.cpp
    Source/ui/CircuitPage.h
    Source/ui/ToneAnalyzerPage.cpp
    Source/ui/ToneAnalyzerPage.h
    Source/ui/SongMatchPage.cpp
    Source/ui/SongMatchPage.h
    Source/PluginProcessorAudio.cpp
    Source/PluginProcessorHost.cpp
    Source/PluginProcessorAssets.cpp
    Source/PluginProcessorSupport.cpp
    Source/PluginProcessorInternal.h
    Source/PluginProcessor.h
    Source/StudioServices.cpp
    Source/StudioServices.h
)

target_compile_definitions(TubeForge PUBLIC
    JUCE_REPORT_APP_USAGE=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_WEB_BROWSER=0
)

option(TUBEFORGE_ENABLE_ASIO "Enable ASIO in Windows standalone builds" ON)
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
    TubeForgeAssets
    nts_audio_core
    nts_amp
    nts_ir
    nts_circuit
    nts_tone_analysis
    nts_reconstruction
    nts_assistant
    nts_ecosystem
    nts_ml_runtime
    nts_nam
    nts_pedals
    nts_diagnostics
    nts_state
    juce::juce_audio_utils
    juce::juce_cryptography
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
    Source/TubeForgeTheme.h
    Source/TubeForgeLookAndFeel.cpp
    Source/TubeForgeLookAndFeel.h
    Source/ui/ModulePage.h
    Source/ui/UiSupport.cpp
    Source/ui/UiSupport.h
    Source/ui/ShellWidgets.cpp
    Source/ui/ShellWidgets.h
    Source/ui/AmplifierPage.cpp
    Source/ui/AmplifierPage.h
    Source/ui/FaceplateArt.cpp
    Source/ui/FaceplateArt.h
    Source/ui/GearPicker.cpp
    Source/ui/GearPicker.h
    Source/ui/ToneShapingPage.cpp
    Source/ui/ToneShapingPage.h
    Source/ui/NeuralCapturePage.cpp
    Source/ui/NeuralCapturePage.h
    Source/ui/PedalArt.cpp
    Source/ui/PedalArt.h
    Source/ui/PedalboardPage.cpp
    Source/ui/PedalboardPage.h
    Source/ui/CapturePicker.cpp
    Source/ui/CapturePicker.h
    Source/ui/CapturesPage.cpp
    Source/ui/CapturesPage.h
    Source/ui/ToneAssistantPage.cpp
    Source/ui/ToneAssistantPage.h
    Source/ui/ProfileLibraryPage.cpp
    Source/ui/ProfileLibraryPage.h
    Source/ui/CabinetPage.cpp
    Source/ui/CabinetPage.h
    Source/ui/TunerPage.cpp
    Source/ui/TunerPage.h
    Source/ui/CircuitPage.cpp
    Source/ui/CircuitPage.h
    Source/ui/ToneAnalyzerPage.cpp
    Source/ui/ToneAnalyzerPage.h
    Source/ui/SongMatchPage.cpp
    Source/ui/SongMatchPage.h
    Source/PluginProcessorAudio.cpp
    Source/PluginProcessorHost.cpp
    Source/PluginProcessorAssets.cpp
    Source/PluginProcessorSupport.cpp
    Source/PluginProcessorInternal.h
    Source/PluginProcessor.h
    Source/StudioServices.cpp
    Source/StudioServices.h
)
target_include_directories(nts_standalone_app PRIVATE Source)
target_compile_definitions(nts_standalone_app PRIVATE
    JucePlugin_Name="TubeForge"
    JUCE_REPORT_APP_USAGE=0
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
)
target_link_libraries(nts_standalone_app PRIVATE
    TubeForgeAssets
    nts_audio_core
    nts_amp
    nts_ir
    nts_circuit
    nts_tone_analysis
    nts_reconstruction
    nts_assistant
    nts_ecosystem
    nts_ml_runtime
    nts_nam
    nts_pedals
    nts_diagnostics
    nts_state
    juce::juce_audio_utils
    juce::juce_cryptography
    juce::juce_dsp
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
)
tubeforge_configure_asio(nts_standalone_app)

add_custom_target(nts_vst3 DEPENDS TubeForge_VST3)
add_custom_target(nts_standalone DEPENDS nts_standalone_app)

if(WIN32)
    find_program(TUBEFORGE_POWERSHELL_EXECUTABLE powershell REQUIRED)
    add_custom_target(nts_release_bundle
        COMMAND ${TUBEFORGE_POWERSHELL_EXECUTABLE} -NoProfile -ExecutionPolicy Bypass
                -File ${CMAKE_SOURCE_DIR}/packaging/windows/stage-release.ps1
                -BuildDirectory ${CMAKE_BINARY_DIR}
                -Configuration $<CONFIG>
                -OutputDirectory ${CMAKE_BINARY_DIR}/release/TubeForge-${PROJECT_VERSION}-Windows-x64
        DEPENDS TubeForge_VST3 nts_standalone_app
        COMMENT "Staging TubeForge Windows release bundle and SHA-256 manifest"
        VERBATIM
    )
    find_program(TUBEFORGE_ISCC_EXECUTABLE ISCC
        PATHS "C:/Program Files (x86)/Inno Setup 6" "$ENV{ProgramFiles}/Inno Setup 6")
    if(TUBEFORGE_ISCC_EXECUTABLE)
        add_custom_target(nts_windows_installer
            COMMAND ${CMAKE_COMMAND} -E env TUBEFORGE_ISCC=${TUBEFORGE_ISCC_EXECUTABLE}
                    ${TUBEFORGE_POWERSHELL_EXECUTABLE} -NoProfile -ExecutionPolicy Bypass
                    -File ${CMAKE_SOURCE_DIR}/packaging/windows/build-installer.ps1
                    -BuildDirectory ${CMAKE_BINARY_DIR} -Configuration $<CONFIG>
            DEPENDS TubeForge_VST3 nts_standalone_app
            COMMENT "Building TubeForge Windows installer"
            VERBATIM
        )
    endif()
endif()

option(TUBEFORGE_BUILD_TESTS "Build TubeForge test and benchmark targets" ON)
option(TUBEFORGE_ENABLE_HOUR_SOAK_TEST "Register the one-hour wall-clock soak test with CTest" OFF)

if(TUBEFORGE_BUILD_TESTS)
    enable_testing()

    add_executable(nts_ml_runtime_tests Tests/MlRuntimeTests.cpp)
    target_include_directories(nts_ml_runtime_tests PRIVATE Tests)
    target_link_libraries(nts_ml_runtime_tests PRIVATE nts_ml_runtime)
    add_test(NAME nts_ml_runtime_tests COMMAND nts_ml_runtime_tests)

    add_executable(nts_ml_runtime_cli Tests/MlRuntimeCli.cpp)
    target_link_libraries(nts_ml_runtime_cli PRIVATE nts_ml_runtime)

    add_executable(nts_ml_runtime_benchmarks Tests/MlRuntimeBenchmarks.cpp)
    target_link_libraries(nts_ml_runtime_benchmarks PRIVATE nts_ml_runtime)
    add_test(NAME nts_ml_runtime_performance COMMAND nts_ml_runtime_benchmarks)

    add_executable(nts_pedal_tests Tests/PedalTests.cpp)
    target_include_directories(nts_pedal_tests PRIVATE Tests)
    target_link_libraries(nts_pedal_tests PRIVATE nts_pedals)
    add_test(NAME nts_pedal_tests COMMAND nts_pedal_tests)

    add_executable(nts_nam_tests Tests/NamTests.cpp)
    target_include_directories(nts_nam_tests PRIVATE Tests)
    target_link_libraries(nts_nam_tests PRIVATE nts_nam)
    add_test(NAME nts_nam_tests COMMAND nts_nam_tests)

    # Emits the converter's model.bin for one capture so the parity driver can compare it, byte
    # for byte, with what nts-nam-import writes for the same file.
    add_executable(nts_nam_cli Tests/NamCli.cpp)
    target_link_libraries(nts_nam_cli PRIVATE nts_nam)

    add_executable(nts_circuit_tests Tests/CircuitTests.cpp)
    target_include_directories(nts_circuit_tests PRIVATE Tests)
    target_link_libraries(nts_circuit_tests PRIVATE nts_circuit)
    add_test(NAME nts_circuit_tests COMMAND nts_circuit_tests)

    add_executable(nts_tone_analysis_tests Tests/ToneAnalysisTests.cpp)
    target_include_directories(nts_tone_analysis_tests PRIVATE Tests)
    target_link_libraries(nts_tone_analysis_tests PRIVATE nts_tone_analysis)
    add_test(NAME nts_tone_analysis_tests COMMAND nts_tone_analysis_tests)

    add_executable(nts_reconstruction_tests Tests/ReconstructionTests.cpp)
    target_include_directories(nts_reconstruction_tests PRIVATE Tests)
    target_link_libraries(nts_reconstruction_tests PRIVATE nts_reconstruction)
    add_test(NAME nts_reconstruction_tests COMMAND nts_reconstruction_tests)

    add_executable(nts_assistant_tests Tests/AssistantTests.cpp)
    target_include_directories(nts_assistant_tests PRIVATE Tests)
    target_link_libraries(nts_assistant_tests PRIVATE nts_assistant)
    add_test(NAME nts_assistant_tests COMMAND nts_assistant_tests)

    add_executable(nts_ecosystem_tests Tests/EcosystemTests.cpp)
    target_include_directories(nts_ecosystem_tests PRIVATE Tests)
    target_link_libraries(nts_ecosystem_tests PRIVATE nts_ecosystem)
    add_test(NAME nts_ecosystem_tests COMMAND nts_ecosystem_tests)

    # These test targets compile only TubeForge sources, so the warning policy applies
    # cleanly. The juce_add_console_app targets below are deliberately left out: they
    # compile JUCE's vendored codec C (libvorbis and friends), which does not build
    # warning-clean and is not ours to fix.
    foreach(target nts_ml_runtime_tests nts_ml_runtime_cli nts_ml_runtime_benchmarks
                   nts_pedal_tests nts_nam_tests nts_nam_cli nts_circuit_tests
                   nts_tone_analysis_tests nts_reconstruction_tests nts_assistant_tests
                   nts_ecosystem_tests)
        tubeforge_set_warning_policy(${target})
    endforeach()

    # The suites the sanitizer leg runs. Excludes the soak and VST3 host tests, whose runtimes
    # are impractical under ASan's slowdown.
    foreach(target nts_ml_runtime_tests nts_pedal_tests nts_nam_tests nts_circuit_tests
                   nts_tone_analysis_tests nts_reconstruction_tests nts_assistant_tests
                   nts_ecosystem_tests)
        tubeforge_stage_sanitizer_runtime(${target})
    endforeach()

    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME nts_ml_python_tests
            COMMAND ${Python3_EXECUTABLE} -m unittest discover -s ${CMAKE_SOURCE_DIR}/ml/tests -p "test_*.py")
        add_test(NAME nts_ml_runtime_parity
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/ml/tests/parity_driver.py
                    $<TARGET_FILE:nts_ml_runtime_cli>)
        # Same contract for a torch-trained model. Reports a skip and passes when the optional
        # backend is absent, so it is safe to register unconditionally.
        add_test(NAME nts_ml_torch_parity
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/ml/tests/torch_parity_driver.py
                    $<TARGET_FILE:nts_ml_runtime_cli>)
        # The plug-in carries its own .nam reader and packer so it can open a capture without an
        # interpreter present. This is what stops that duplicate from drifting: the two converters
        # must write byte-identical models for every capture, synthetic and real.
        add_test(NAME nts_nam_parity
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/ml/tests/nam_parity_driver.py
                    $<TARGET_FILE:nts_nam_cli>)
        # ml/tests does not need to be on the path: Python puts a script's own directory on
        # sys.path, which is how the existing drivers reach their fixture builders.
        set_tests_properties(nts_ml_python_tests nts_ml_runtime_parity nts_ml_torch_parity
                             nts_nam_parity PROPERTIES
            ENVIRONMENT "PYTHONPATH=${CMAKE_SOURCE_DIR}/ml")
    endif()

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
    target_include_directories(nts_device_tests PRIVATE Tests apps/standalone)
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

    # The plug-in shell -- editor, pages, and the four processor units -- as one list, so a
    # target that needs to instantiate TubeForgeAudioProcessor does not have to repeat it.
    set(TUBEFORGE_SHELL_SOURCES
    Source/PluginEditor.cpp
    Source/PluginEditor.h
    Source/TubeForgeTheme.h
    Source/TubeForgeLookAndFeel.cpp
    Source/TubeForgeLookAndFeel.h
    Source/ui/ModulePage.h
    Source/ui/UiSupport.cpp
    Source/ui/UiSupport.h
    Source/ui/ShellWidgets.cpp
    Source/ui/ShellWidgets.h
    Source/ui/AmplifierPage.cpp
    Source/ui/AmplifierPage.h
    Source/ui/FaceplateArt.cpp
    Source/ui/FaceplateArt.h
    Source/ui/GearPicker.cpp
    Source/ui/GearPicker.h
    Source/ui/ToneShapingPage.cpp
    Source/ui/ToneShapingPage.h
    Source/ui/NeuralCapturePage.cpp
    Source/ui/NeuralCapturePage.h
    Source/ui/PedalArt.cpp
    Source/ui/PedalArt.h
    Source/ui/PedalboardPage.cpp
    Source/ui/PedalboardPage.h
    Source/ui/CapturePicker.cpp
    Source/ui/CapturePicker.h
    Source/ui/CapturesPage.cpp
    Source/ui/CapturesPage.h
    Source/ui/ToneAssistantPage.cpp
    Source/ui/ToneAssistantPage.h
    Source/ui/ProfileLibraryPage.cpp
    Source/ui/ProfileLibraryPage.h
    Source/ui/CabinetPage.cpp
    Source/ui/CabinetPage.h
    Source/ui/TunerPage.cpp
    Source/ui/TunerPage.h
    Source/ui/CircuitPage.cpp
    Source/ui/CircuitPage.h
    Source/ui/ToneAnalyzerPage.cpp
    Source/ui/ToneAnalyzerPage.h
    Source/ui/SongMatchPage.cpp
    Source/ui/SongMatchPage.h
    Source/PluginProcessorAudio.cpp
    Source/PluginProcessorHost.cpp
    Source/PluginProcessorAssets.cpp
    Source/PluginProcessorSupport.cpp
    Source/PluginProcessorInternal.h
    Source/PluginProcessor.h
    Source/StudioServices.cpp
    Source/StudioServices.h
    )
    set(TUBEFORGE_SHELL_LIBRARIES
        TubeForgeAssets
        nts_audio_core
        nts_amp
        nts_ir
        nts_circuit
        nts_tone_analysis
        nts_reconstruction
        nts_assistant
        nts_ecosystem
        nts_ml_runtime
        nts_nam
        nts_pedals
        nts_diagnostics
        nts_state
        juce::juce_audio_utils
        juce::juce_cryptography
        juce::juce_dsp
    )

    juce_add_console_app(nts_wrapper_tests PRODUCT_NAME "TubeForge Wrapper Tests")
    target_sources(nts_wrapper_tests PRIVATE Tests/WrapperTests.cpp ${TUBEFORGE_SHELL_SOURCES})
    target_include_directories(nts_wrapper_tests PRIVATE Tests Source)
    target_compile_definitions(nts_wrapper_tests PRIVATE
        JucePlugin_Name="TubeForge"
        JUCE_REPORT_APP_USAGE=0
        JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0
        # Test-only: lets the suite pump the message queue so asynchronous work -- a MIDI
        # program change latched on the audio thread, for instance -- can be observed landing.
        JUCE_MODAL_LOOPS_PERMITTED=1
    )
    target_link_libraries(nts_wrapper_tests PRIVATE ${TUBEFORGE_SHELL_LIBRARIES})

    # Renders the drawn amplifier and pedal faces to PNGs. Not a test -- it asserts nothing and
    # is deliberately left out of the `check` target -- but the only way to review procedural
    # artwork is to look at it, and it links the two art files alone rather than the whole shell.
    juce_add_console_app(nts_art_renderer PRODUCT_NAME "TubeForge Art Renderer")
    target_sources(nts_art_renderer PRIVATE
        Tests/ArtRenderer.cpp
        Source/ui/FaceplateArt.cpp
        Source/ui/FaceplateArt.h
        Source/ui/PedalArt.cpp
        Source/ui/PedalArt.h
        Source/TubeForgeTheme.h)
    target_include_directories(nts_art_renderer PRIVATE Source)
    target_compile_definitions(nts_art_renderer PRIVATE
        JUCE_REPORT_APP_USAGE=0
        JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0
    )
    target_link_libraries(nts_art_renderer PRIVATE nts_amp nts_pedals juce::juce_gui_basics)

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
    target_link_libraries(nts_soak_tests PRIVATE nts_audio_core nts_amp)

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

    juce_add_console_app(nts_amp_tests PRODUCT_NAME "TubeForge Traditional Amp Tests")
    target_sources(nts_amp_tests PRIVATE Tests/AmpTests.cpp)
    target_include_directories(nts_amp_tests PRIVATE Tests)
    target_link_libraries(nts_amp_tests PRIVATE nts_amp)

    juce_add_console_app(nts_dsp_benchmarks PRODUCT_NAME "TubeForge DSP Benchmarks")
    target_sources(nts_dsp_benchmarks PRIVATE Tests/DspBenchmarks.cpp)
    target_include_directories(nts_dsp_benchmarks PRIVATE Tests)
    target_link_libraries(nts_dsp_benchmarks PRIVATE nts_dsp)

    # End-to-end amplifier cost across oversampling factor, cabinet length and whether the
    # controls are moving. Not a CTest gate: it is a measurement tool, and the acceptance
    # thresholds live on the per-component benchmarks.
    juce_add_console_app(nts_amp_benchmarks PRODUCT_NAME "TubeForge Amp Benchmarks")
    target_sources(nts_amp_benchmarks PRIVATE Tests/AmpBenchmarks.cpp)
    target_include_directories(nts_amp_benchmarks PRIVATE Tests)
    target_link_libraries(nts_amp_benchmarks PRIVATE nts_amp)

    # Whole-plug-in cost through the real processBlock, per engine mode and block size. This is
    # the only figure that includes the shared front end, pedals, gate, effects, metering and
    # display feeds together, so it is the one that answers "will this run on that machine".
    juce_add_console_app(nts_plugin_benchmarks PRODUCT_NAME "TubeForge Plugin Benchmarks")
    target_sources(nts_plugin_benchmarks PRIVATE Tests/PluginBenchmarks.cpp ${TUBEFORGE_SHELL_SOURCES})
    target_include_directories(nts_plugin_benchmarks PRIVATE Tests Source)
    target_compile_definitions(nts_plugin_benchmarks PRIVATE
        JucePlugin_Name="TubeForge"
        JUCE_REPORT_APP_USAGE=0
        JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0
    )
    target_link_libraries(nts_plugin_benchmarks PRIVATE ${TUBEFORGE_SHELL_LIBRARIES})

    add_test(NAME nts_unit_tests COMMAND nts_unit_tests)
    tubeforge_stage_sanitizer_runtime(nts_unit_tests)
    add_test(NAME nts_integration_tests COMMAND nts_integration_tests)
    add_test(NAME nts_device_tests COMMAND nts_device_tests)
    add_test(NAME nts_wrapper_tests COMMAND nts_wrapper_tests)
    add_test(NAME nts_vst3_host_tests COMMAND nts_vst3_host_tests
             "${CMAKE_BINARY_DIR}/TubeForge_artefacts/$<CONFIG>/VST3/TubeForge.vst3")
    add_test(NAME nts_dsp_tests COMMAND nts_dsp_tests)
    tubeforge_stage_sanitizer_runtime(nts_dsp_tests)
    add_test(NAME nts_ir_tests COMMAND nts_ir_tests)
    add_test(NAME nts_amp_tests COMMAND nts_amp_tests)
    tubeforge_stage_sanitizer_runtime(nts_amp_tests)
    add_test(NAME nts_dsp_benchmark_acceptance COMMAND nts_dsp_benchmarks)
    if(TUBEFORGE_ENABLE_HOUR_SOAK_TEST)
        add_test(NAME nts_hour_soak COMMAND nts_soak_tests 3600)
        set_tests_properties(nts_hour_soak PROPERTIES LABELS soak TIMEOUT 3900)
    endif()
endif()
