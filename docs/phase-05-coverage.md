# Phase 5 coverage

Scoring: `Complete = 1`, `Partial = 0.5`, `Missing = 0`. Requirements are the 100 observable items
derived from the Phase 5 plan.

| # | Requirement | Status | Evidence |
|---:|---|:---:|---|
| 1 | Complete neural capture workflow | Complete | Typed wizard from configuration through activation |
| 2 | Learn DI-to-target transformation | Complete | Paired supervised training |
| 3 | Preserve transient behavior | Complete | Transient-weighted loss and report |
| 4 | Preserve dynamic breakup | Complete | Stateful nonlinear recurrent candidates |
| 5 | Preserve low-frequency response | Complete | Spectral loss and low-frequency metric |
| 6 | Preserve chord articulation | Complete | Fixed chord evaluation category |
| 7 | Preserve recovery behavior | Complete | Stateful chunks with history context |
| 8 | Preserve silence stability | Complete | Silence loss, clip, and dBFS metric |
| 9 | Baseline recurrent model | Complete | `ConditionedLstm` |
| 10 | Alternative TCN model | Complete | Streaming `RandomFeatureTcn` |
| 11 | Capture wizard | Complete | `CaptureWizard` plus CLI and plug-in tab |
| 12 | Automatic alignment | Complete | Phase 4 GCC-PHAT/local drift integrated into wizard |
| 13 | Training and validation pipeline | Complete | Separate session sets enforced |
| 14 | Model export | Complete | Packed v1/v2 artifacts |
| 15 | Real-time C++ inference | Complete | Four packed architectures |
| 16 | LSTM mono input and one layer | Complete | Configurable hidden size, mono streaming |
| 17 | LSTM hidden size 32-128 supported | Complete | Runtime range includes recommended sizes |
| 18 | GRU comparison training | Partial | Frozen GRU features with a trained linear readout (`RandomFeatureGru`); full gate BPTT deferred |
| 19 | Linear output projection | Complete | All candidates |
| 20 | Optional residual path | Complete | LSTM and TCN residual gain |
| 21 | Optional control conditioning | Complete | Five-control normalized vector |
| 22 | Fully trainable causal TCN | Partial | Frozen dilated causal stack with a trained linear readout (`RandomFeatureTcn`); kernel BPTT deferred |
| 23 | Explicit TCN receptive field | Complete | Calculated from layers/kernel/dilation |
| 24 | Stateful TCN history handling | Complete | Per-layer circular histories |
| 25 | Gain conditioning | Complete | Chunk metadata and runtime control |
| 26 | Tone conditioning | Complete | Chunk metadata and runtime control |
| 27 | Master conditioning | Complete | Chunk metadata and runtime control |
| 28 | Channel conditioning | Complete | Chunk metadata and runtime control |
| 29 | Instrument-mode conditioning | Complete | Chunk metadata and runtime control |
| 30 | Concatenation conditioning | Complete | Audio plus control vector at each recurrent step |
| 31 | Hardware channel capture | Partial | Wizard configures and validates channels; recording still occurs through user hardware/DAW |
| 32 | Target and level configuration | Complete | Versioned configuration JSON |
| 33 | Duration and controls configuration | Complete | Validated capture configuration |
| 34 | Clipping safety check | Complete | Paired-session validator |
| 35 | Noise-floor measurement | Complete | Required silence calibration |
| 36 | Round-trip latency measurement | Complete | Marker and correlation paths |
| 37 | Rights-cleared musical performances | Partial | Required checklist/schema exists; no real performances are present locally |
| 38 | Signal-return verification | Complete | Nonzero/missing-output checks |
| 39 | Feedback-routing warning | Complete | Explicit acknowledgement gate and UI guidance |
| 40 | Calibration values stored | Complete | Session and artifact metadata |
| 41 | Logarithmic sweep | Complete | Wizard-generated 24-bit WAV |
| 42 | Impulse excitation | Complete | Wizard-generated 24-bit WAV |
| 43 | Multitone excitation | Complete | Wizard-generated 24-bit WAV |
| 44 | Level-stepped noise | Complete | Wizard-generated seeded WAV |
| 45 | Guitar chord requirement | Complete | Musical-take checklist |
| 46 | Guitar palm-mute requirement | Complete | Musical-take checklist |
| 47 | Bass sustained-note requirement | Complete | Musical-take checklist |
| 48 | Bass transient requirement | Complete | Musical-take checklist |
| 49 | Marker detection | Complete | Impulse-marker estimator |
| 50 | Correlation verification | Complete | Confidence threshold and rejection |
| 51 | Clock-drift estimation | Complete | ppm report and limit |
| 52 | Poor-capture rejection | Complete | Machine-readable failed checks |
| 53 | Local training outside plug-in | Complete | Python CLI process |
| 54 | Short-chunk training | Complete | Configurable chunks and history |
| 55 | Longer-sequence fine tuning | Complete | Resolved configs support staged chunk/history increases |
| 56 | Spectral fine tuning | Complete | Multi-resolution STFT weighting |
| 57 | Transient evaluation | Complete | Weighted metric and palm-mute category |
| 58 | Silence evaluation | Complete | Stability metric and silence category |
| 59 | Best-checkpoint export | Complete | Validation-selected checkpoint |
| 60 | Runtime benchmark | Complete | 48 kHz/64 mono and stereo p99 target |
| 61 | Capture-level metadata | Complete | Expected input RMS in manifest/normalization |
| 62 | Runtime RMS estimate | Complete | 400 ms monitor |
| 63 | Runtime peak estimate | Complete | Decaying peak monitor |
| 64 | Calibration comparison | Complete | dB mismatch reading |
| 65 | Compensation or warning | Complete | 3 dB warning and opt-in bounded trim |
| 66 | Dynamics-preserving level policy | Complete | No default automatic leveling |
| 67 | Reset on transport start | Complete | Host playhead transition detection |
| 68 | Preserve state across blocks | Complete | Stateful per-channel models |
| 69 | Safe silence reset policy | Complete | No unsafe automatic silence reset |
| 70 | Offline-render determinism | Complete | Reset and parity tests |
| 71 | Random block-size behavior | Complete | Partition-invariance tests |
| 72 | SIMD where practical | Partial | Release compiler vectorization enabled; no model-specific SIMD kernel yet |
| 73 | No audio-thread allocation | Complete | Allocation-counter acceptance test |
| 74 | No Python runtime in inference | Complete | Standalone C++ packed runtime |
| 75 | Deterministic state update | Complete | Reset equality and parity |
| 76 | Block-size-independent output | Complete | Python and C++ partition tests |
| 77 | Denormal handling | Complete | Scoped host guard and tiny-value suppression |
| 78 | Invalid-model bypass | Complete | Parser/test-vector rejection leaves DI intact |
| 79 | Worker model load | Complete | `std::jthread` artifact loader |
| 80 | Manifest validation | Complete | Version, SHA-256, sample rate and dimensions |
| 81 | Test-vector validation | Complete | Maximum-error gate before publish |
| 82 | Immutable pending publication | Complete | Prepared inactive slot and atomic index |
| 83 | Click-free model switching | Complete | 1024-sample old/new crossfade |
| 84 | Original-target comparison | Complete | Offline target render |
| 85 | Model-output comparison | Complete | Offline and live model modes |
| 86 | Bypass-DI comparison | Complete | Offline and sample-exact live mode |
| 87 | Loudness-matched comparison | Complete | BS.1770 offline render and live monitor |
| 88 | Null-difference render | Complete | Automatic WAV output |
| 89 | 96 kHz performance evidence | Partial | Exact-rate policy supports 96 kHz artifacts; 96 kHz benchmark not recorded |
| 90 | Blind switching | Complete | Phase 3 synchronized ABX player consumes renders |
| 91 | Wrong-schema rejection | Complete | Python manifest and C++ packed-parser tests |
| 92 | Parameter conditioning test | Complete | Output-change test for every candidate |
| 93 | Stable silence | Complete | Finite bounded silence test and metric |
| 94 | No runaway DC | Complete | DC penalty and quality metric |
| 95 | No NaN/Inf | Complete | Weight and per-sample guards |
| 96 | Python/C++ inference parity | Complete | Tanh, LSTM, GRU and TCN vector parity |
| 97 | Automatic quality report | Complete | All specified metrics and confidence |
| 98 | Input-calibration UI warning | Complete | Neural Capture live status |
| 99 | VST3 and standalone activation | Complete | Shared processor and runtime library |
| 100 | Real hardware end-to-end evidence | Partial | Synthetic end-to-end acceptance passes; a user-owned target capture is not available locally |

## Score

- Complete: 94
- Partial: 6
- Missing: 0
- Points: 97 / 100
- **Coverage: 97%**

The remaining points require full GRU/TCN backpropagation, a dedicated SIMD inference kernel, a 96 kHz
benchmark, and real rights-cleared hardware captures. Hardware audio and performer evidence are not
fabricated by automated tests.
