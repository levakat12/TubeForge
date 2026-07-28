# Phase 4 coverage

Scoring: `Complete = 1`, `Partial = 0.5`, `Missing = 0`.

| # | Requirement | Status | Evidence |
|---:|---|---|---|
| 1 | Reusable ML platform | Complete | Framework-independent `nts_ml` package |
| 2 | Multiple models without data/evaluation rewrites | Complete | `AudioModel` protocol and model factory |
| 3 | Typed Python and validation | Complete | Dataclasses, typed arrays, explicit validators |
| 4 | Dataset package | Complete | `nts_ml.datasets` |
| 5 | Alignment package | Complete | `nts_ml.alignment` |
| 6 | Augmentation package | Complete | `nts_ml.augmentation` |
| 7 | Models package | Complete | `nts_ml.models` |
| 8 | Losses package | Complete | `nts_ml.losses` |
| 9 | Training package | Complete | `nts_ml.training` |
| 10 | Evaluation package | Complete | `nts_ml.evaluation` |
| 11 | Export package | Complete | `nts_ml.export` |
| 12 | Schema package | Complete | Session and model schemas |
| 13 | Config directory | Complete | `ml/configs/tiny.toml` |
| 14 | Scripts and tests | Complete | Capture/validate/train/evaluate/export scripts and tests |
| 15 | Session metadata file | Complete | Validated `session.json` |
| 16 | Input take directory | Complete | Paired discovery by take stem |
| 17 | Output take directory | Complete | Missing pair rejection |
| 18 | Calibration directory | Complete | Silence, sweep, impulse checks |
| 19 | Reports directory | Complete | Validation JSON and alignment HTML |
| 20 | Metadata schema version | Complete | Version 1 enforced |
| 21 | UUID session ID | Complete | UUID validation |
| 22 | Sample-rate metadata | Complete | Range and file match validation |
| 23 | Bit-depth metadata | Complete | 16/24/32-bit PCM support |
| 24 | Instrument metadata | Complete | Guitar, bass, other |
| 25 | Input-device metadata | Complete | Neutral free-text device field |
| 26 | Target type/name/settings | Complete | Typed mapping without brand dependency |
| 27 | Input/output calibration | Complete | Finite dB fields |
| 28 | Latency metadata | Complete | Nonnegative sample latency |
| 29 | Notes and no brand requirement | Complete | Neutral templates and guide |
| 30 | Identical sample rate | Complete | Per-take quality check |
| 31 | Identical channel count | Complete | Per-take quality check |
| 32 | No clipped samples | Complete | Input/output peak checks |
| 33 | Nonzero signal | Complete | RMS floor checks |
| 34 | Sufficient duration | Complete | Configurable duration check |
| 35 | Stable latency | Complete | Local and cross-take standard deviation |
| 36 | Acceptable noise floor | Complete | Required silence recording check |
| 37 | No missing output sections | Complete | Active-window gap ratio |
| 38 | Clock drift tolerance | Complete | ppm measurement and rejection |
| 39 | Polarity validation | Complete | Detection plus documented exception |
| 40 | Machine-readable report | Complete | Versioned validation JSON |
| 41 | Impulse-marker alignment | Complete | `impulse_marker_latency` |
| 42 | Cross-correlation alignment | Complete | FFT linear correlation |
| 43 | Generalized correlation | Complete | GCC-PHAT |
| 44 | Local drift correction | Complete | Local lag measurement and resampling correction |
| 45 | Global latency output | Complete | `globalLatencySamples` |
| 46 | Drift output | Complete | `driftPpm` |
| 47 | Confidence output | Complete | Normalized alignment confidence |
| 48 | Low-confidence rejection | Complete | Validator and negative test |
| 49 | Model-history context | Complete | Explicit history frames |
| 50 | Supervised output mask | Complete | Boolean loss mask |
| 51 | Combined context and target load | Complete | Random-access total-sample reads |
| 52 | Valid recurrent-state warmup | Complete | Loss begins after history |
| 53 | Session-level split | Complete | Session IDs are grouping keys |
| 54 | Performance-level split | Complete | Performance IDs are indivisible |
| 55 | 70/15/15 defaults | Complete | Deterministic hash split defaults |
| 56 | Unseen phrases/no leakage | Complete | Ownership assertion and test |
| 57 | Consistent equal gain | Complete | Paired gain augmentation |
| 58 | Paired polarity inversion | Complete | Same sign applied to both |
| 59 | Channel selection | Complete | Validated paired channel selection |
| 60 | Input-only low noise | Complete | Seeded robustness augmentation |
| 61 | Alignment-aware mild jitter | Complete | Common paired crop with returned offset |
| 62 | No arbitrary pitch/time augmentation | Complete | API intentionally omits invalid transforms |
| 63 | TOML configuration | Complete | Standard-library TOML loader |
| 64 | Model configuration | Complete | Type/hidden/layer fields |
| 65 | Data configuration | Complete | Rate/chunk/history fields |
| 66 | Training configuration | Complete | Batch/AdamW/rate/epochs/seed |
| 67 | Composite loss configuration | Complete | All named weights |
| 68 | Resolved configuration saved | Complete | `resolved-config.json` per run |
| 69 | Git commit tracking | Complete | Repository commit capture |
| 70 | Full config tracking | Complete | Sorted JSON in SQLite |
| 71 | Random-seed tracking | Complete | Config and database |
| 72 | Dataset version tracking | Complete | Metadata plus full input/output content SHA-256 versions |
| 73 | Hardware tracking | Complete | OS/machine/processor string |
| 74 | Training-duration tracking | Complete | Monotonic elapsed time |
| 75 | Best-checkpoint tracking | Complete | NPZ path and best validation loss |
| 76 | Validation-metric tracking | Complete | JSON metric map |
| 77 | Exported-model hash tracking | Complete | Manifest hash stored in run |
| 78 | Local SQLite tracking | Complete | Transactional experiment table |
| 79 | Waveform L1 | Complete | Implemented and tested |
| 80 | Waveform L2 | Complete | Implemented and used for reference BPTT |
| 81 | Pre-emphasized loss | Complete | Configurable coefficient |
| 82 | Multi-resolution STFT loss | Complete | Three default resolutions |
| 83 | Spectral convergence | Complete | Frobenius convergence metric |
| 84 | DC penalty | Complete | Mean-output penalty |
| 85 | Loudness difference | Complete | RMS dB difference |
| 86 | Optional perceptual embedding | Complete | Injected embedding callable |
| 87 | Multiple simultaneous metrics | Complete | Composite loss and evaluation aggregate |
| 88 | Fixed 14-clip taxonomy | Complete | Every required musical/diagnostic category |
| 89 | Reproducible evaluation inputs | Complete | Seeded generator for all clip names |
| 90 | Automatic evaluation report | Complete | Per-clip and aggregate JSON |
| 91 | Complete model artifact layout | Complete | Model, manifest, normalization, vectors, license |
| 92 | Manifest contract | Complete | All Phase 4 fields validated |
| 93 | SHA-256 integrity | Complete | Export and registry revalidation |
| 94 | Versioned model registry | Complete | Immutable name/version directories |
| 95 | Normalization and licensing governance | Complete | Required validated files |
| 96 | Export pipeline | Complete | Custom packed float32 recurrent weights selected |
| 97 | Exported test vectors and tolerances | Complete | Input/output vectors plus numeric policy |
| 98 | C++ runtime parity | Complete | Max, RMS, drift, and reset comparison |
| 99 | Automated Phase 4 CI | Complete | Schema/alignment/train/export/parity/determinism/provenance/corpus tests |
| 100 | Diverse rights-cleared real DI corpus | Partial | Provenance schema and strict eight-category corpus audit are complete; no real recordings are present locally |

## Score

- Complete: 99
- Partial: 1
- Missing: 0
- Points: 99.5 / 100
- **Coverage: 99.5%**

The remaining evidence is a real, rights-cleared set of diverse guitar and bass DI performances and
their paired target captures. Synthetic test fixtures validate the infrastructure but are not claimed
as production training data. `phase-04-dataset-guide.md` documents collection, releases, hashing, and
the machine-readable audit command.
