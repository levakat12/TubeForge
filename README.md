# TubeForge

TubeForge is the Windows-first implementation of the NeuralTone Studio architecture: a low-latency standalone application and VST3 plug-in for guitar and bass tone processing.

The active `0.2.0` codebase implements the Phase 1 core infrastructure. It intentionally processes only input gain, output gain, bypass, and metering; amplifier modelling and ML inference begin in later phases.

## Phase 1 foundation

- One framework-independent C++20 engine shared by standalone and VST3 wrappers.
- Atomic scalar parameters and an immutable snapshot exchange for larger state.
- Allocation-free, `noexcept` audio processing with a test-time heap-allocation detector.
- Versioned JSON project files with explicit v0-to-v1 migration, validation, and relative asset paths.
- Persistent settings storage with a safe default when saved state is invalid.
- Lock-free audio diagnostic events and off-thread structured JSONL logging.
- Callback time, maximum time, deadline, CPU load, dropout, sample-rate, meter, latency, and process-memory diagnostics.
- Background work using `std::jthread` and `std::stop_token`, including exception isolation.
- Project open/save, mode, device status, meters, controls, and diagnostics in the editor.
- Separate libraries and test targets with a Windows GitHub Actions pipeline.

See [Phase 1 coverage](docs/phase-01-coverage.md) for the requirement-by-requirement status.

## Build on Windows

Requirements:

- Visual Studio 2022 Build Tools with Desktop development with C++
- CMake 3.22+
- Git

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

JUCE is fetched at the pinned `8.0.13` tag. Build products are created below `build/TubeForge_artefacts/`; automatic installation to the system VST3 folder remains disabled.

### ASIO

WASAPI is available through the normal Windows JUCE build. The pinned JUCE version includes ASIO interface headers, so the ASIO variant can be compiled with:

```powershell
cmake -S . -B build-asio -G "Visual Studio 17 2022" -A x64 `
  -DTUBEFORGE_ENABLE_ASIO=ON
```

Set `TUBEFORGE_ASIO_SDK_PATH=C:\SDKs\asiosdk` to use an external SDK whose root contains `common\iasiodrv.h`. ASIO source is subject to Steinberg's proprietary ASIO license or GPLv3; compiling support does not by itself settle the license required for distribution.

The standalone window embeds JUCE's device selector in its **Audio settings** tab. It owns device enumeration, channel selection, sample rate, buffer size, persistence, and fallback behavior. A VST3 instance receives those settings from its host.

## Repository targets

| Target | Responsibility |
|---|---|
| `nts_audio_core` | JUCE-independent processing contract and neutral engine |
| `nts_state` | Project schema, migration, validation, paths, settings |
| `nts_diagnostics` | Lock-free events, timing, latency, logging, workers |
| `nts_standalone` | Custom standalone host with embedded device selector |
| `nts_vst3` | JUCE VST3 wrapper |
| `nts_unit_tests` | Deterministic unit tests |
| `nts_integration_tests` | Real-time, state, stress, and boundary tests |
| `nts_device_tests` | Mock-device selection, reconfiguration, loss, and recovery tests |
| `nts_asio_tests` | Optional ASIO backend registration probe when ASIO is enabled |
| `nts_wrapper_tests` | Processor state and repeated editor lifecycle tests |
| `nts_vst3_host_tests` | Actual VST3 scan, instantiation, processing, and editor tests |
| `nts_soak_tests` | True wall-clock continuous-processing soak test |

## Optional long stress test

The normal integration suite runs 20,000 blocks at 96 kHz with a 32-sample buffer. `nts_soak_tests 3600` performs a true one-hour wall-clock run. The latest local acceptance run completed all 3,600 seconds and processed 23,885,199,732 blocks successfully. Weekly CI and manual workflow dispatches register and execute this test with `TUBEFORGE_ENABLE_HOUR_SOAK_TEST=ON`.

## Licensing

JUCE is dual-licensed. Distribution must use a JUCE license compatible with the intended product. The Steinberg ASIO SDK has separate terms and is never fetched automatically.
