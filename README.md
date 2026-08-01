# TubeForge

TubeForge is the Windows-first implementation of the NeuralTone Studio architecture: a low-latency standalone application and VST3 plug-in for guitar and bass tone processing.

Phase 10 turns the Windows-first build into a distributable product: secure shareable `.ntone` profiles,
a local profile browser, signed-update policy, privacy-safe diagnostics, installer staging, release CI, and
compatibility/release documentation.

The active `0.10.0` codebase implements the Phase 1 runtime, Phase 2 DSP foundation, Phase 3 traditional
guitar/bass amplifier, Phase 4 ML platform, Phase 5 neural capture workflow, and Phase 6 physical/hybrid
circuit runtime, Phase 7 tone analysis, Phase 8 source reconstruction, Phase 9 intelligent assistant, and
Phase 10 Windows release ecosystem. Traditional, physical, and deterministic tone-analysis modes
work without ML assets; learned neural components activate only after a validated model is loaded.

**Status: pre-release, not shipped.** The per-phase coverage documents are the authoritative record of what
is actually delivered, and each names its own gaps: Phase 6 at 91%, Phase 7 at 90%, Phase 8 at 95%, Phase 9
at 91%, and Phase 10 at 86%. The feature lists below describe implemented behavior, not release readiness.
Outstanding release gates include a production Authenticode certificate, an exercised update download and
rollback transport, host-owned plug-in crash interception, a consented crash-upload service, and recorded
REAPER and VST3PluginTestHost validation runs. See [Phase 10 coverage](docs/phase-10-coverage.md) for the
full list.

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

## Phase 2 DSP foundation

- Linear/dB gain conversion, linear and logarithmic smoothing, and ramped gain.
- First-order, biquad, state-variable, and Linkwitz-Riley filters with guarded coefficients and automation interpolation.
- Tanh, arctangent, hard, soft, asymmetric, diode, biasable, and envelope-dependent waveshapers.
- 1x/2x/4x/8x oversampling with polyphase symmetric anti-alias FIR filters and exact latency reporting.
- Noise gate, feed-forward compressor, and lookahead peak limiter with internally smoothed automation.
- Direct and uniform partitioned convolution with inactive preparation and atomic response crossfades.
- Asynchronous cabinet WAV decoding, channel conversion, resampling, DC removal, trimming, and normalization.
- FFT, STFT reconstruction, spectrum, waveform, peak, RMS, BS.1770 loudness, crest, clipping, and gain-reduction metering.
- Generic mode crossfades, explicit SSE2 kernels, fixed DI fixtures, hashes, numeric regression metrics, and a complete repeatable performance matrix.

See [Phase 2 coverage](docs/phase-02-coverage.md), [DSP architecture](docs/phase-02-architecture.md), and [benchmark results](docs/phase-02-benchmarks.csv).

## Phase 3 traditional amplifier

- Tight Modern and Vintage Bloom original topologies with guitar and bass variants.
- Profile-aware DI calibration, automatic trim suggestion, gate, pre-EQ, tightness, and pick emphasis.
- Two-to-four responsive oversampled preamp stages with bias shift, memory, transient response, and asymmetric saturation.
- Coupled passive, active, and bass semi-parametric tone stacks.
- Phase-inverter and sagging power-stage approximations with feedback, presence, resonance, and damping.
- Dual-IR cabinet blend with alignment, polarity, metadata, filtering, bypass, and bass DI blend.
- Phase-aligned bass clean/drive split, click-free presets, deterministic offline rendering, and simple/advanced automatable UI pages.

See [Phase 3 coverage](docs/phase-03-coverage.md), [amp architecture](docs/phase-03-architecture.md),
the [listening protocol](docs/phase-03-listening-protocol.md), and the executable
[ABX evidence tools](docs/phase-03-abx-tools.md).

## Phase 4 ML and dataset infrastructure

- Versioned paired-capture sessions with calibration, signal-quality, alignment, drift, polarity, and
  missing-output validation.
- History-aware streaming data, leakage-safe performance splits, paired augmentation, composite losses,
  deterministic training, and SQLite experiment tracking.
- Two training backends behind one interface. The default NumPy trainer has no dependency beyond
  NumPy and is bit-exact for a given seed. An optional PyTorch backend (`training.backend = "torch"`)
  trains the same weights roughly two orders of magnitude faster on a GPU and is what makes a
  full-length capture practical; it is reproducible to float32 tolerance rather than bit-exactly,
  because cuDNN's recurrent backward pass accumulates nondeterministically. Both produce the same
  checkpoint format, and `nts_ml_torch_parity` holds a torch-trained model to the same C++ runtime
  tolerance as a NumPy-trained one.
- Fixed evaluation taxonomy, packed versioned model artifacts, SHA-256 validation, and C++ runtime parity.
- Per-take rights provenance with immutable audio hashes, performer-release checks, duplicate detection,
  and strict coverage of eight guitar/bass performance categories.

See [Phase 4 coverage](docs/phase-04-coverage.md), [ML architecture](docs/phase-04-architecture.md), and
the [dataset and corpus-audit guide](docs/phase-04-dataset-guide.md).

## Phase 5 neural amplifier capture

- A conditioned LSTM trained end to end with backpropagation through time, plus GRU-shaped and
  dilated causal-TCN feature extractors whose recurrent and convolutional weights stay at their
  seeded values while only a linear readout is fitted. The latter two are echo-state models, named
  `RandomFeatureGru` and `RandomFeatureTcn` to say so; use the LSTM when the recurrence itself has to
  learn. See [Phase 5 coverage](docs/phase-05-coverage.md) rows 18 and 22.
- Safety-gated capture wizard with prepared excitation, alignment, drift rejection, separate validation,
  automatic training/export, quality reporting, and atomic activation pointers.
- Packed C++ inference with artifact SHA/test-vector validation, transport reset handling, input-level
  warnings, allocation-free processing, arbitrary block sizes, and click-free model replacement.
- Neural Capture UI for artifact loading, traditional/neural selection, bypass/model comparison, and
  bounded opt-in calibration compensation.

See [Phase 5 coverage](docs/phase-05-coverage.md), [architecture](docs/phase-05-architecture.md), and the
[capture guide](docs/phase-05-capture-guide.md).

## Phase 6 physical and hybrid circuit modelling

- Stable typed graph IDs for input, filter, triode, tone stack, phase inverter, power, feedback,
  transformer, cabinet, and output nodes, with validated ports and parameter schemas.
- Worker-compiled immutable mono runtimes (one per channel), preallocated buffers, latency calculation,
  block-boundary publication, and a 2048-sample graph crossfade.
- Electrical 12AX7/12AT7/12AU7/6V6/EL34 definitions and static, stateful gray-box, bounded numerical,
  and packed-neural nonlinear backends behind one component interface.
- Component-value-derived passive tone controls, stateful supply sag, local delayed feedback,
  phase-inverter, power-topology, output-transformer, and reactive cabinet approximations.
- A Physical Circuit engine mode, macro-controlled simple view, engineering schematic/response view,
  copied per-stage telemetry, validity warnings, stable JSON round-trip, and v0-to-v1 migration.

These are bounded virtual approximations. Tube choices and voltage values are not electrical safety advice
and do not imply that equivalent substitutions are safe in physical hardware. See
[Phase 6 coverage](docs/phase-06-coverage.md) and [architecture](docs/phase-06-architecture.md).

## Phase 7 tone analysis and embedding

- Offline 5-to-60-second analysis for isolated stems, paired captures, plugin renders, physical-amp
  recordings, and user clips without work on the real-time playback thread.
- Multi-resolution spectral, dynamic, nonlinear, spatial/production, and instrument-context features.
- Versioned normalized 128-dimensional embeddings with a loadable learned projection and a deterministic
  bootstrap projection when no trained artifact is installed.
- Contrastive rig training, tone/performance/production disentanglement, interpretable ridge heads,
  domain-distance confidence, calibration, OOD, retrieval, robustness, and ablation evaluation tools.
- Guitar/bass-aware hybrid similarity, JSON tone reports with descriptor uncertainty, and searchable
  profiles carrying source, quality, tags, model version, and licensing metadata.
- A Tone Analyzer studio page for loading clips, reviewing descriptors and warnings, and finding the
  closest profile in the current session.

See [Phase 7 coverage](docs/phase-07-coverage.md) and [architecture](docs/phase-07-architecture.md).

## Phase 8 source separation and reconstruction

- WAV, AIFF, FLAC, Ogg, and decoder-available MP3 song import, 48 kHz resampling, and a bounded local
  decoded-audio cache, all outside the playback callback.
- Local Demucs six-stem neural separation into vocals, drums, bass, other, guitar, and piano, with a direct
  guitar target, deterministic private cache, progress/cancellation, automatic CUDA use, and an explicit
  deterministic DSP fallback when the optional ML runtime is unavailable.
- Direct guitar/bass selection, full/left/right/mid/side/panned stereo views, timestamped playable-part
  selection, clean/crunch/distorted classification, dominant-pitch confidence, likely tuning family/cent
  offset, automatic recommendation, and leakage/artifact warnings.
- Loudness/DC/silence reference normalization that preserves nonlinear/dynamic tone behavior and optionally
  reduces a broad room-tail estimate.
- Staged coarse classification, plausible rig generation, real traditional-amp rendering, Phase 7 comparison,
  complexity-aware ranking, separate tone/recording scores, and several editable playable candidates.
- A Song Reconstruction page with target/stereo and detected-part selection, progress/cancellation, candidate
  comparison, live preset application, and parameter-only safe export.

See [Phase 8 coverage](docs/phase-08-coverage.md) and [architecture](docs/phase-08-architecture.md).

## Phase 9 intelligent tone assistant

- Allocation-free audio-thread summary frames carrying live input/output peak, RMS, clipping, zero-crossing,
  correlation, sample-rate, and latency evidence into a bounded lock-free queue.
- Off-thread aggregation and interpretable rules for input, gain-structure, spectral, dynamic, routing, phase,
  cabinet, bass-split, gate, and latency problems.
- Diagnose, tight rhythm, clean bass support, aggressive picked bass, smooth lead, warm clean, less harsh,
  preserve low end, and reference-match goals mapped to constrained real parameters.
- Every recommendation includes a diagnosis, confidence, measured evidence, beginner and advanced explanations,
  exact changes, expected effect, and a measurable-problem/style-preference distinction.
- A fixed parameter schema enforces value and delta bounds; arbitrary graph mutation is impossible. Preview,
  accept, reject, and undo preserve exact parameter snapshots and use the amp engine's smoothing/crossfades.
- Optional personalization stores instrument, gain, brightness, clean blend, cabinet use, and accepted/rejected
  actions locally. It can be disabled or cleared from the Tone Assistant page.

See [Phase 9 coverage](docs/phase-09-coverage.md) and [architecture](docs/phase-09-architecture.md).

## Phase 10 release and ecosystem

- Versioned directory-based `.ntone` packages with UUID/compatibility/license metadata, exact hashes, optional
  RSASSA-PKCS1-v1_5 signatures over SHA-256 with a minimum 2048-bit modulus, strict path/type/size allowlists,
  and neural runtime validation.
- Local Profile Library UI for search, instrument/favorite filters, compatibility and trust warnings,
  import/export, favorites, last-used ordering, and current-rig application.
- Standalone-only verified update decisions, plugin-installer-only policy, sanitized consent-aware crash reports,
  and telemetry that is off by default with a compile-time metadata allowlist.
- Reproducible Windows staging with symbols and SHA-256 manifests, optional signed Inno Setup installer,
  Debug/Release CI, host validation, privacy/security/licensing policy, and user/developer/research guides.

See [Phase 10 coverage](docs/phase-10-coverage.md), [package format](docs/phase-10-package-format.md), and
[release process](docs/release-process.md).

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

The standalone window embeds JUCE's device selector in its **Audio settings** tab. It owns device enumeration, channel selection, sample rate, buffer size, persistence, and fallback behavior. On startup it requests the smallest driver buffer that provides at least a 0.65 ms callback budget, avoiding unstable 16/48-sample modes while preserving sub-millisecond buffering. The plug-in defaults to 1x oversampling for zero algorithmic latency; 2x/4x/8x remain available when additional anti-aliasing is preferred. A VST3 instance receives device settings from its host and reports processing-latency changes asynchronously.

## Repository targets

| Target | Responsibility |
|---|---|
| `nts_audio_core` | JUCE-independent processing contract and neutral engine |
| `nts_dsp` | Reusable filters, dynamics, nonlinear, oversampling, convolution, analysis, metering, and regression DSP |
| `nts_amp` | Traditional guitar/bass preamp, tone stack, power amp, cabinet, presets, and offline graph |
| `nts_circuit` | Typed physical/hybrid component graph, validation, worker compilation, telemetry, and graph serialization |
| `nts_tone_analysis` | Offline tone features, confidence/reporting, versioned embeddings, similarity, and profile search |
| `nts_reconstruction` | Stem separation, region confidence, reference normalization, rig search, and safe result export |
| `nts_assistant` | Live signal summaries, diagnostic rules, musical goals, validated actions, explanations, and local preferences |
| `nts_ecosystem` | Secure `.ntone` packages, profile library, updates, crash privacy, telemetry policy, and format versions |
| `nts_ir` | Background cabinet audio decoding and offline IR preparation |
| `nts_state` | Project schema, migration, validation, paths, settings |
| `nts_diagnostics` | Lock-free events, timing, latency, logging, workers |
| `nts_ml_runtime` | Allocation-free packed recurrent-model runtime shared by parity tests and future neural processing |
| `nts_standalone` | Custom standalone host with embedded device selector |
| `nts_vst3` | JUCE VST3 wrapper |
| `nts_unit_tests` | Deterministic unit tests |
| `nts_integration_tests` | Real-time, state, stress, and boundary tests |
| `nts_device_tests` | Mock-device selection, reconfiguration, loss, and recovery tests |
| `nts_asio_tests` | Optional ASIO backend registration probe when ASIO is enabled |
| `nts_wrapper_tests` | Processor state and repeated editor lifecycle tests |
| `nts_ml_runtime_tests` | Packed-model validation and reset behavior |
| `nts_circuit_tests` | Circuit components, graph validation/migration/swap, hybrid runtime, telemetry, and real-time allocation tests |
| `nts_tone_analysis_tests` | Phase 7 analysis, invariance, confidence, serialization, comparison, and search tests |
| `nts_reconstruction_tests` | Phase 8 separation consistency, cache/cancellation, regions, stereo, reconstruction, and safe-export tests |
| `nts_assistant_tests` | Phase 9 labelled diagnostics, false positives, action safety, rollback, preferences, goals, and summary-queue tests |
| `nts_ecosystem_tests` | Phase 10 package traversal/hash/signature/model/license, browser, update, crash, and telemetry tests |
| `nts_ml_python_tests` | Dataset, alignment, training, export, evaluation, registry, and determinism suite |
| `nts_ml_runtime_parity` | Python export versus compiled C++ output tolerance test |
| `nts_vst3_host_tests` | Actual VST3 scan, instantiation, processing, and editor tests |
| `nts_soak_tests` | True wall-clock continuous-processing soak test |
| `nts_dsp_tests` | Phase 2 response, stability, aliasing, convolution, analysis, regression, and allocation tests |
| `nts_ir_tests` | Real asynchronous cabinet WAV decode/preparation test |
| `nts_amp_tests` | Phase 3 calibration, topology, sag, cabinet, preset, regression, real-time, and CPU tests |
| `nts_dsp_benchmarks` | Required sample-rate/block-size mono/stereo performance matrix |

Run the repeatable DSP benchmark matrix with:

```powershell
.\build\nts_dsp_benchmarks_artefacts\Release\nts_dsp_benchmarks.exe
```

## Optional long stress test

The normal integration suite runs 20,000 blocks at 96 kHz with a 32-sample buffer. `nts_soak_tests 3600` performs a true one-hour wall-clock run.

The soak drives the traditional amplifier and the core engine together, so oversampling, waveshaping, tone stack, sag, and convolution are all under continuous load, with parameter automation, periodic preset crossfades, and periodic state resets applied while audio keeps flowing. It checks that output stays finite, stays bounded, and stays non-silent. A 20-second local run processes roughly 424,000 blocks, about seven times real time; a full hour is on the order of 76 million blocks.

Earlier revisions of this test exercised only `nts_audio_core`, which is input gain, output gain, and metering. The much larger block counts previously quoted here reflected that gain-only path and did not indicate amplifier stability. Weekly CI and manual workflow dispatches register and execute this test with `TUBEFORGE_ENABLE_HOUR_SOAK_TEST=ON`.

## Licensing

JUCE is dual-licensed. Distribution must use a JUCE license compatible with the intended product. The Steinberg ASIO SDK has separate terms and is never fetched automatically. Asset redistribution is governed by per-package metadata and underlying terms; see [third-party notices](THIRD_PARTY_NOTICES.md).
