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

## Training backends

The NumPy trainer is the reference implementation: no dependency beyond NumPy, and bit-exact for a
given seed. Its cost is a Python loop over samples, so a realistic capture is impractical on it.

An optional PyTorch backend is selected with `training.backend`, which accepts `numpy` (default),
`torch`, or `auto`. It exists because the recurrence maps exactly onto `torch.nn.LSTM`: the gate
blocks are ordered input, forget, cell, output in both, so the matrices transfer without
permutation, and the NumPy model's single bias becomes torch's input-side bias with the hidden-side
bias zeroed. Measured agreement between the two forward passes is ~2.7e-7 relative, which is float32
rounding.

Measured speedup on an RTX 5080, one epoch, 32-unit state:

| Chunk samples | Total audio | NumPy | Torch | Speedup |
|---:|---:|---:|---:|---:|
| 2048 | 0.17 s | 0.48 s | 0.01 s | 35x |
| 8192 | 0.68 s | 1.91 s | 0.02 s | 117x |
| 32768 | 2.73 s | 7.68 s | 0.05 s | 151x |
| 32768 (16 chunks) | 10.92 s | 25.99 s | 0.16 s | 164x |
| 131072 | 21.85 s | 55.33 s | 0.30 s | 184x |

### What hardware this actually needs

The headline speedup conflates two separate wins, and separating them decides the advice. Same
workload, 1.37 s of audio, two epochs, 32-unit state:

| Backend | Time | vs NumPy |
|---|---:|---:|
| NumPy | 6.44 s | 1.0x |
| torch, CPU | 0.27 s | 24x |
| torch, RTX 5080 | 0.05 s | 136x |

Extrapolated to five minutes of paired audio over 40 epochs: **NumPy ~7.9 hours, torch on CPU
~20 minutes, torch on an RTX 5080 ~3 minutes.**

So roughly 24x of the gain comes from leaving the per-sample Python loop, and only ~5.6x from the
GPU. **A weak GPU, or no usable GPU at all, costs far less than it looks.** Anyone without CUDA
should still set `backend = "torch"`; the CPU path alone turns an overnight job into a coffee break.

The reason the GPU contributes so little is that an LSTM is a sequential recurrence — no amount of
parallelism removes the dependency from one timestep to the next. Measured on the RTX 5080 at
state 32:

- Each timestep costs a near-constant **0.6–1.0 µs** whatever the sequence length, which is dispatch
  and dependency latency rather than arithmetic.
- Widening the batch from 1 to 64 — **64x the work** — costs only **2.9x the time**. The device is
  better than 90% idle at ordinary batch sizes.
- State 16 and state 32 take the same time despite 4x the arithmetic.

A card with a fraction of the SMs runs a latency-bound workload at close to the same speed, so the
practical range for any CUDA GPU is bounded below by the CPU figure and above by the numbers here:
a factor of about six, not of a hundred. Clock speed matters more than core count.

Two things do degrade on a smaller card:

- **VRAM sets the maximum batch and chunk length**, because backpropagation through time keeps
  activations for the whole sequence. Measured peaks: 144 MiB at batch 4 x 8192 samples, 504 MiB at
  batch 8 x 16384, 1.9 GiB at batch 16 x 32768. The defaults (`chunk_samples = 4096`,
  `batch_size = 4`) stay under 200 MiB and fit any card with 2 GB. Reduce batch size before chunk
  length if memory is tight.
- **Large states fall off a cliff.** Time is flat to state 64, rises to 37 ms at 128, then jumps to
  457 ms at 256 — cuDNN dropping out of its persistent-RNN path, which needs the weights to fit in
  registers across the whole device. That threshold arrives *earlier* on a smaller GPU. Staying
  within the documented 32–128 range avoids it.

Two caveats worth knowing:

- **Reproducibility is weaker.** Seeded torch runs agree to about 1e-7, not bit-exactly, because
  cuDNN's LSTM backward pass accumulates in a nondeterministic order. Train on the NumPy backend
  where bit-exact reproduction is required.
- **cuDNN has a sequence-length ceiling.** It handles 32768 timesteps and fails at 65536 with
  `CUDNN_STATUS_NOT_SUPPORTED` on contiguous input. Longer sequences are fed through in windows with
  the state carried across, which is numerically equivalent and keeps the fused kernel; disabling
  cuDNN instead was measured to be no faster than the NumPy trainer.


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
