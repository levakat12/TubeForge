# Phase 1 coverage

Coverage is scored against 50 observable requirements extracted from the Phase 1 plan. Complete items score 1 point, partial items score 0.5, and missing items score 0.

**Current verified score: 49.5 / 50 = 99%.**

| # | Requirement | Status | Evidence or gap |
|---:|---|:---:|---|
| 1 | JUCE standalone shell | Complete | Custom `nts_standalone_app` / `nts_standalone` builds and launches |
| 2 | JUCE VST3 shell | Complete | VST3 bundle and module manifest build |
| 3 | Shared processing engine | Complete | Both wrappers use `nts_audio_core` |
| 4 | Device enumeration and selection | Complete | Embedded JUCE device selector plus mock-device integration harness |
| 5 | Channel, sample-rate, and buffer selection | Complete | Automated channel and 96 kHz/32-sample reconfiguration |
| 6 | Device persistence, loss recovery, and fallback | Complete | Saved setup, disappearance, recovery, and invalid-configuration tests |
| 7 | ASIO | Complete | `JUCE_ASIO=1` Release standalone builds and launches; external SDK path and CI compile validation are included |
| 8 | WASAPI | Complete | Default Windows JUCE standalone build |
| 9 | CoreAudio abstraction | Partial | JUCE abstraction retained; macOS build and CI validation explicitly deferred |
| 10 | Atomic scalar parameter transport | Complete | `RuntimeParameters` |
| 11 | Immutable large-state transport | Complete | `ImmutableSnapshotExchange` with off-audio reclamation |
| 12 | Persistent application settings | Complete | `SettingsStore` plus standalone device properties |
| 13 | Versioned project JSON | Complete | Schema v1 top-level contract |
| 14 | Explicit migration | Complete | v0-to-v1 migration |
| 15 | Validation and last-known-good activation | Complete | State applies only after full validation |
| 16 | Project-relative asset paths | Complete | Normalization and validation tests |
| 17 | Structured off-thread logging | Complete | JSONL `StructuredLogger` worker |
| 18 | Compact lock-free audio events | Complete | SPSC `AudioEvent` transport |
| 19 | Current and maximum callback duration | Complete | Atomic diagnostics snapshot |
| 20 | Deadline and CPU load | Complete | Per-block deadline calculation |
| 21 | Dropout/xrun detection | Complete | Overrun counter and event |
| 22 | Sample-rate mismatch detection | Complete | Expected/actual rate event |
| 23 | Full latency budget and host reporting | Complete | All named sources tracked; processing latency reported |
| 24 | Model, IR, and job status diagnostics | Complete | Atomic status state, failure counts, and UI readout; the SPSC queue remains audio-producer-only |
| 25 | Process memory diagnostics | Complete | Windows/macOS/Linux non-real-time sampler and UI readout |
| 26 | Background worker and exception isolation | Complete | `jthread`, `stop_token`, failure test |
| 27 | Audio-settings panel | Complete | Custom standalone embeds `AudioDeviceSelectorComponent` in an Audio settings tab |
| 28 | Input/output meters | Complete | Downsampled atomic peak meters |
| 29 | CPU meter | Complete | 20 Hz diagnostics UI |
| 30 | Device status | Complete | Standalone/host ownership status |
| 31 | Mode indicator | Complete | Standalone/VST3 mode label |
| 32 | Project open/save | Complete | Async file dialogs and validated `.tforge` state |
| 33 | Diagnostics view | Complete | CPU, callback, deadline, dropout, latency |
| 34 | Required C++20 features | Complete | span, atomics, concepts, filesystem, chrono, enums, RAII, jthread |
| 35 | Modular CMake targets | Complete | audio core, state, diagnostics, two wrappers, six standard tests, and an optional ASIO probe |
| 36 | No allocation after prepare | Complete | Global allocation instrumentation test |
| 37 | No blocking mutex in processing | Complete | Audio engine uses atomics/fixed stack storage only |
| 38 | Required deterministic unit tests | Complete | Smoothing, migration, latency, queue, paths, validation |
| 39 | Exact project-state round trip | Complete | Equality-based serialization test |
| 40 | Rapid parameter-change survival | Complete | Integration stress test |
| 41 | 32-sample / 96 kHz stress | Complete | 20,000-block default test |
| 42 | VST3 load in a host | Complete | JUCE host scans, instantiates, processes, and opens the built VST3 |
| 43 | Standalone startup with no device | Complete | Zero-channel `AudioDeviceManager` integration test |
| 44 | Device sample-rate change integration | Complete | Mock device changes to 96 kHz/32 samples and publishes the new setup |
| 45 | Repeated disconnect/reconnect stress | Complete | 100 close/restart cycles plus disappearance/return recovery |
| 46 | Repeated preset loading | Complete | 2,000-load integration test |
| 47 | Repeated UI open/close stress | Complete | 250 direct wrapper cycles plus 25 hosted-VST3 editor cycles |
| 48 | 60-minute continuous test | Complete | Local 3,600-second run passed after 23,885,199,732 continuously processed blocks; weekly/manual CI job included |
| 49 | Forced worker-exception test | Complete | Exception containment integration test |
| 50 | Unit/integration CI | Complete | Windows GitHub Actions build, test, artifact upload, plus scheduled soak job |

## Work required for 100%

1. Add and pass an ARM64 macOS/CoreAudio CI build when macOS support is resumed.

Before distributing ASIO-enabled binaries, choose and comply with Steinberg's proprietary or GPLv3 licensing route. This is a release/legal gate rather than an unimplemented Phase 1 runtime feature.

Real-hardware ASIO/WASAPI fault tests and commercial-host matrices remain useful release validation, but are not substitutes for the deterministic Phase 1 tests above.
