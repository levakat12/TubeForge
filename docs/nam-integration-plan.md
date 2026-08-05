# NAM capture library integration plan

An implementation-ready plan for turning `amp_learning/` and `pedal_learning/` into product value.
Every item states what is missing, the evidence for it, the design to apply, the files touched, the
tests that prove it, and what "done" means.

Everything in this document has now been implemented; the per-item sections record what was built
and where the plan was wrong. Line references are against the tree at commit `0a97c55`; re-check
them before editing, because they have drifted.

## Status

| Track | Items | Effort | State |
|---|---|---|---|
| 0 — Hygiene and free wins | N0 | 0.25 d | ✅ Complete — corpora ignored; IRs need no code |
| A — Python-only, ships on the current runtime | N1, N2, N3 | 3.5 d | ✅ Complete — parity gate passed at 4.4e-12 |
| B — Native `.nam` playback | N4, N5 | 4.0 d | ✅ Complete — all 354 captures convert and play at ESR 1.8e-12 |
| B — CI gate and docs | N7 | 0.5 d | ✅ Complete — parity and budget enforced by CTest |
| B — Import UX | N6 | 1.0 d | ✅ Complete — zip and `.nam` import in the plug-in, byte-identical to the CLI |

Track A delivers playable models without touching C++. Track B is the headline feature and depends
on N1 for its converter and parity vectors. **N1 is a hard dependency of both tracks — do it first.**

**Coverage: 100% of the planned work by effort** (9.25 of 9.25 days). Measured rather than asserted:

- The plug-in's own converter and `nts-nam-import` write **byte-identical** `model.bin` for all 354
  corpus captures at both tiers: **708 of 708 comparisons identical, 0 failures**, including the
  decimal-to-`float32` conversion of every one of the 12 146 weights in a standard-tier model.
  Enforced by `nts_nam_parity`.
- All 354 captures import through the plug-in's own zip reader and convert into the capture
  library, 22.4 MB on disk, 0 refused.

- `nts_ml.nam` reads all **354** corpus captures at both tiers, 0 rejected.
- Python parity against the reference implementation, one capture per archive × 2 tiers (38
  renders): **worst ESR 4.4e-12**, gate was 1e-6.
- All **354** captures convert to NTSM v3 in 7 s, 0 failures, every artifact passing
  `validate_artifact`.
- The C++ runtime reproduces all **354** converted artifacts against their exported vectors:
  **worst ESR 1.8e-12**, worst absolute difference 2.2e-6 against a 1e-4 tolerance.
- Cost in the Release benchmark gate, p99 over 64-sample blocks at 48 kHz with the real corpus
  geometry: **lite tier 5.2% of budget, standard tier 23.6%** (the recurrent baseline is 5.5% mono,
  10.9% stereo). Scalar, no SIMD.
- `nts-nam-catalogue` indexes all 354 in 1.2 s, recovering the full six-control grid on 30/30
  JCM800 captures.
- A real capture distilled with the shipped config reaches **held-out ESR 0.219** with matched
  output level and 0.89 correlation, in 34 s of training.

Two findings from the implementation are recorded below because they change work still to be done:
see [Priming](#priming-a-discovery-that-changes-n5) and
[Idle DC](#idle-dc-a-discovery-that-changed-n2).

## Inventory (measured, not assumed)

| Folder | `.nam` files | Other |
|---|---|---|
| `amp_learning/` | 169 across 7 amps | 6 cabinet IRs (mono, 0.731 s; 44.1 kHz/32f and 48 kHz/24-bit) |
| `pedal_learning/` | 185 across 12 pedals | — |

All **354** capture files are identical in shape:

- `version: "0.7.0"`, `sample_rate: 48000`, `architecture: "SlimmableContainer"`.
- `config.submodels` holds **two WaveNet submodels**, selected by `max_value` — `0.5` is the lite
  tier (`channels: 3`, 1871 floats), `1.0` is the standard tier (`channels: 8`, 12146 floats).
- One layer array of **23 layers**, `kernel_sizes` 6 except layers 15–16 which are 15,
  `dilations` cycling `1,3,7,17,41,101,239` (three cycles plus a `1,13` pair), `input_size: 1`,
  `condition_size: 1`, `bottleneck == channels`, `layer1x1.active: true`, `head1x1.active: false`,
  head conv `out_channels: 1, kernel_size: 16, bias: true`, LeakyReLU(0.01) throughout.
- `gating_mode: "none"` and `secondary_activation: null` on every layer; **every FiLM block is
  inactive**. No file in either folder uses the v0.7 conditioning extensions.
- `metadata` carries `loudness` (≈ −23 LUFS), `gain`, `gear_make`, `gear_model`, `tone_type`,
  `gear_type`, `modeled_by`, and `training.validation_esr` (≈ 0.002 on the samples checked).

### Derived weight layout (verified)

Per submodel, with `c = channels`, `k_i` the per-layer kernel size, `cond = 1`, `in = 1`,
`hk = 16`:

```
floats = Σ_i ( c·c·k_i  +  c  +  c·cond  +  c·c  +  c )   # dilated conv + bias, input mixin, 1x1 + bias
       + c·in                                              # input rechannel
       + ( c·hk + 1 )                                      # head conv + bias
       + 1                                                 # trailing scalar (head scale)
```

This reproduces **1871** for `c = 3` and **12146** for `c = 8` exactly, on the real files. The
*ordering* of tensors inside each layer is not proven by a size match and must be confirmed against
the reference implementation — see N1's acceptance gate.

### Cost envelope

| Tier | MACs/sample/channel | @48 kHz | Receptive field |
|---|---|---|---|
| lite (`c = 3`) | 1 728 | ≈ 83 MMAC/s | 6 347 samples (132 ms) |
| standard (`c = 8`) | 11 768 | ≈ 565 MMAC/s | 6 347 samples (132 ms) |

The 132 ms receptive field is the number that shapes the runtime design: state is ~6.3 k samples ×
`c` floats per model, and a freshly reset model needs that long before its output is meaningful.

## What the tree supports today

- **IRs: fully supported.** `nts::ir::CabinetIrLoader` and `nts::dsp::prepareImpulseResponse`, wired
  at [PluginProcessor.cpp:722](../Source/PluginProcessor.cpp#L722). No work required.
- **`.nam`: cannot load, at any layer.**
  [PackedTanhModel.cpp:59-79](../engine/ml-runtime/src/PackedTanhModel.cpp#L59) accepts NTSM
  architecture codes 1–4 only (`tanhRnn`, `lstm`, `gru`, `causalTcn` —
  [PackedTanhModel.h:11](../engine/ml-runtime/include/nts/ml/PackedTanhModel.h#L11)); WaveNet is not
  among them, and the loader in
  [PluginProcessor.cpp:843-903](../Source/PluginProcessor.cpp#L843) expects an artifact *directory*
  (`model.bin`, `manifest.json`, `normalization.json`, `test-vectors/`), not a JSON file.

## Findings from implementation

Three things were wrong or unknown when this plan was written. They are recorded here rather than
quietly fixed, because two of them change work that has not been done yet.

### Priming: a discovery that changes N5

A WaveNet's silent state is **not** an all-zero history. Every layer has a convolution bias, so
silence propagates a non-zero constant up the stack. The reference implementation gets the right
state for free by zero-padding its input; a streaming implementation must prime deliberately by
running a receptive field of silence through itself on reset.

This was found as a parity failure and is worth the detail: with zeroed buffers the whole-signal
ESR was **1.0e-5**, but split by region it was 1.0e-3 over the first 1 000 samples and **4.1e-14**
after the receptive field. The model was exactly right and merely started from a state it can never
reach from audio.

**Consequence for N5.** The C++ WaveNet must prime on `reset()`, or it will disagree with its own
exported test vectors at the start of the buffer and
[NeuralAmpProcessor.cpp:83-87](../engine/ml-runtime/src/NeuralAmpProcessor.cpp#L83) will refuse to
load the model. Priming costs a 6 347-sample run, which is far too slow for the audio thread — it
belongs on the worker before the slot is published, which is the same place N5 already needs to
pre-roll history for the crossfade. One mechanism satisfies both.

### Idle DC: a discovery that changed N2

Rendering silence through a real capture produces a pure DC offset — measured at −62 to −78 dBFS
with an AC component below −195 dB. Writing that to `calibration/silence.wav` would report a
constant offset as a noise floor and fail the Phase 4 gate for the wrong reason, so the DC is
removed from the calibration signal and recorded in `targetSettings.idleDcOffset` instead.

### Latency is per-capture, and it is not zero

The plan asserted rendered pairs have zero latency. They do not: the capture includes a mic'd
cabinet, and its propagation delay is part of the model. Measured from the impulse response —
**Marshall JCM800 8 samples, Fulltone OCD 13, Fender Super Reverb 29** — and cross-correlation
agrees exactly. Sessions now measure and declare it. This is not bookkeeping: the training pipeline
aligns pairs using session latency, so a student told the delay is zero would spend capacity
learning a delay it models poorly.

A related consequence: the Phase 4 `alignment_confidence` limit does not transfer to rendered
material. A mic'd cabinet removes most of the DI's spectrum, so a perfect render of a dark clean
Fender scores **0.17** where the gate wants 0.70. `rendered_limits()` waives that one check and
`validate_sessions` replaces it with a stricter requirement recorded material could not meet —
drift and inter-take latency deviation of exactly zero.

## Constraints that apply to every item

1. **Sample rate is a hard gate.**
   [NeuralAmpProcessor.cpp:80](../engine/ml-runtime/src/NeuralAmpProcessor.cpp#L80) rejects any model
   whose rate differs from the device rate. All 354 captures are 48 kHz, so at 44.1 kHz they are
   unusable until either the artifact is produced at both rates (Track A can do this trivially) or
   the runtime resamples around the model (Track B must decide — see Open decisions).
2. **Licensing.** These are third-party captures (`modeled_by: spooky`, SignalArt, AZG and others).
   Local use is one thing; shipping distilled derivatives or converted artifacts is another.
   `export_model` already takes a `license_text` ([packed.py:45](../ml/nts_ml/export/packed.py#L45)) —
   populate it from `metadata.modeled_by` and leave redistribution off by default.
3. **Repo hygiene.** ~50 MB of zips sit untracked and *unignored*; a single `git add -A` commits
   them. N0 fixes this first.
4. **Level convention.** NAM captures assume a calibrated input level and record `metadata.loudness`
   and `metadata.gain`; the runtime's calibration monitor defaults to −21 dB RMS
   ([NeuralAmpProcessor.h:36](../engine/ml-runtime/include/nts/ml/NeuralAmpProcessor.h#L36)). Every
   path below must carry NAM's figure into `normalization.json.inputRmsDb` rather than accept the
   default.

---

## N0 — Ignore the corpora, adopt the IRs ✅

**State.** `.gitignore` covers both corpora. The IRs need no code; loading them is a user action.

**Problem.** 50 MB of third-party binaries are one `git add -A` away from entering history, and six
usable cabinet IRs are sitting unused inside zips.

**Design.** Add `/amp_learning/` and `/pedal_learning/` to `.gitignore`. Extract the two "Mix Ready"
archives to the user cabinet folder; prefer the `- 48 24` variants at 48 kHz to avoid resampling.

**Files.** `.gitignore`.

**Tests.** `git status --porcelain` shows neither folder. Manual: load `VOX AC30 BLUE 1.wav` through
the Cabinet page and confirm `AssetLoadStatus::ready`.

**Done.** Corpora untrackable by accident; both IR sets audibly loading.

---

## N1 — `nts_ml.nam`: reader and reference renderer ✅

**State.** Shipped as `ml/nts_ml/nam/{reader,wavenet,metadata}.py` with `ml/tests/test_nam_reader.py`
(12 tests). The acceptance gate passed at **4.4e-12 worst ESR** across one capture per archive at
both tiers, against `neural-amp-modeler` 0.13. The weight ordering guessed in this plan was
correct; the priming behaviour it did not anticipate is written up above. Block-size invariance
holds to 4.5e-8 (float32 rounding) across block sizes 1 to 4096.

**Problem.** Nothing in the repo can read a `.nam`, and both tracks need one: N2 needs it to render
teacher targets, N4 needs it to convert weights, and N5 needs it to generate parity vectors.

**Evidence.** The format is fully characterised above, and uniform across all 354 files, so a
targeted reader is a small, closed piece of work — no need to support the whole v0.7 surface.

**Design.** New package `ml/nts_ml/nam/`:

- `reader.py` — parse the JSON, validate `version`/`architecture`/`sample_rate`, select a submodel by
  `max_value` (`1.0` standard, `0.5` lite), and **reject anything this reader does not model**:
  active FiLM blocks, `gating_mode != "none"`, non-null `secondary_activation`, `head1x1.active`,
  `groups != 1`, non-LeakyReLU activations. Fail loudly rather than render silently-wrong audio.
- `wavenet.py` — NumPy forward pass (no torch dependency; `nts-ml` is numpy-only by design,
  [pyproject.toml:10](../ml/pyproject.toml#L10)). Dilated causal conv per layer, LeakyReLU(0.01),
  input mixin from the condition channel, 1×1 with residual accumulation, 16-tap head conv,
  head scale. Block-streaming with explicit ring-buffer state so it can double as the C++ oracle.
- `metadata.py` — typed view over `metadata` (name, gear make/model, tone type, gear type, loudness,
  gain, ESR, modeled_by).

**Acceptance gate — this is the item that de-risks everything downstream.** The tensor ordering
inside a layer is a guess until proven. Install `neural-amp-modeler` as a dev-only extra, render a
fixed 10 s DI through one capture with both the reference implementation and `wavenet.py`, and
require **ESR < 1e-6**. Do not start N2 or N4 until that passes; if ordering is wrong, the error
shows up here rather than as a mysteriously bad amp three items later.

**Files.** `ml/nts_ml/nam/{__init__,reader,wavenet,metadata}.py`, `ml/pyproject.toml` (dev extra),
`ml/tests/test_nam_reader.py`.

**Tests.** Round-trip: parse → shape assertions matching the derived layout formula (both tiers).
Rejection tests for each unsupported feature flag. Streaming/one-shot equivalence: block sizes 1, 64,
1024 produce identical output to within 1e-9. Reference-parity test, skipped when the optional
dependency is absent so CI stays numpy-only.

**Done.** ESR < 1e-6 against the reference on at least one amp and one pedal capture; unsupported
configurations rejected with named errors.

---

## N2 — Distillation: `.nam` → NTSM artifact via the existing pipeline ✅

**State.** Shipped as `ml/nts_ml/nam/distill.py`, CLI `nts-nam-distill`, config
`ml/configs/nam-distill-lstm.toml`, tests in `ml/tests/test_nam_distill.py`. Rendered sessions pass
the Phase 4 gate, and the end-to-end path (render → sessions → train → `validate_artifact`) runs in
CI on a synthetic fixture and has been run on a real corpus capture. Rendering measures **~24x
realtime** at the standard tier in NumPy, so a 10-minute DI corpus renders in about 25 seconds.
Two assumptions in the original write-up were wrong and are corrected above (latency, idle DC).

**Problem.** The runtime cannot run WaveNet, but it *can* run LSTM/GRU/TCN today. A capture used as a
teacher gives a student the runtime already supports.

**Design.** New CLI `nts-nam-distill` in [cli.py](../ml/nts_ml/cli.py):

```
DI corpus ──► nam.wavenet render ──► session packages ──► nts-tiny-train ──► model.bin + manifest
```

1. Render the DI corpus through the standard submodel, applying `metadata.gain`.
2. Emit session packages in the existing layout — `session.json`, `input/*.wav`, `output/*.wav`
   ([package.py:36-54](../ml/nts_ml/datasets/package.py#L36)) — with `targetType: "amp"` (or
   `"pedal"`), `targetName` from metadata, `latencySamples: 0`, and
   `inputCalibrationDb` derived from `metadata.loudness`.
3. Split the corpus across **at least two sessions**: `tiny_train` rejects fewer by design, to
   prevent validation leakage ([cli.py:36-38](../ml/nts_ml/cli.py#L36)). Split by *source material*,
   not by slicing one file, or the split is cosmetic.
4. Train with `ml/configs/phase5-lstm.toml` and export through the unchanged `export_model`.

Because pairs are rendered rather than recorded, they are sample-aligned and noise-free — the Phase 4
quality gate should pass trivially. If any check fails, that is a bug in the renderer, not a reason
to relax the limits.

**Files.** `ml/nts_ml/nam/distill.py`, `ml/nts_ml/cli.py`, `ml/pyproject.toml` (script entry),
`ml/configs/nam-distill-lstm.toml`, `ml/tests/test_nam_distill.py`.

**Tests.** End-to-end on a tiny synthetic `.nam` fixture (2 layers, `c = 2`) so it runs in CI in
seconds: distill → `validate_artifact` passes → `PackedTanhModel` loads the result and matches the
exported test vectors. Session packages must pass `validate_session` unmodified.

**Done.** A distilled Deluxe Reverb artifact loads in the plugin and passes test-vector validation.
Report ESR against the teacher in the run report; expect it to be far above NAM's own ~0.002,
especially on the high-gain Badlander and 5150 captures — that gap is the honest cost of this track
and should be recorded, not hidden.

---

## N3 — Metadata catalogue as a tone-matching corpus ✅

**State.** Shipped as `ml/nts_ml/nam/catalogue.py` and CLI `nts-nam-catalogue`. Indexes all 354
captures in 1.2 s with 0 rejections, recovering the complete six-control grid on **30 of 30**
JCM800 captures, plus stage tags (111), wattage (63), channel (67), microphones (27) and boost (9)
across the rest. Consumption by `ToneProfileDatabase` is not wired up — the catalogue exists and is
schema-stable; the C++ side of that is not part of Track A.

**Problem.** 354 labelled captures — 30 of them a gain × master-volume grid on one JCM800, 89 an OCD
sweep — are a reference corpus the tone analyser and rig reconstruction currently do not have.

**Design.** `nts-nam-catalogue` walks both folders, and for each capture emits a row combining
`metadata` fields with settings parsed from the filename convention
(`JCM800 2203 - P5 B5 M5 T5 MV6 G5`, `5150 MXR/Maxon/no boost`, mic label). Optionally render a fixed
probe signal per capture and store the feature fingerprint used by `ToneProfileDatabase`, so rig
candidate scoring can rank against measured hardware rather than synthesised references.

**Files.** `ml/nts_ml/nam/catalogue.py`, `ml/nts_ml/cli.py`, `ml/tests/test_nam_catalogue.py`;
consumed by `engine/tone-analysis/src/ToneProfileDatabase.cpp`.

**Tests.** Filename-parser table test over the real names in both folders, including the ones that
do not follow the convention (they must degrade to "unknown", not crash). Catalogue schema round-trip.

**Done.** A JSON catalogue of all 354 captures, with parsed settings for the JCM800, 5150 and OCD
series, loadable by the tone analyser.

---

## N4 — NTSM v3: WaveNet architecture code ✅

**State.** Shipped as `pack_wavenet`/`export_wavenet` in `ml/nts_ml/export/packed.py`,
`ml/nts_ml/nam/convert.py`, CLI `nts-nam-import`, tests in `ml/tests/test_nam_convert.py`.
All 354 corpus captures convert in 7 s with 0 failures and pass `validate_artifact`. Conversion is
idempotent and keyed by source digest, so re-importing the same capture reuses its artifact.

**Problem.** The packed format cannot express WaveNet. The v2 header is a fixed `<4s11I`
([packed.py:15](../ml/nts_ml/export/packed.py#L15)) with a closed `floatCount` validation chain
([PackedTanhModel.cpp:66-84](../engine/ml-runtime/src/PackedTanhModel.cpp#L66)); there is nowhere to
put 23 per-layer kernel sizes and dilations.

**Design.** Version 3 header, architecture code `5 = waveNet`, keeping v1/v2 parsing untouched:

```
magic "NTSM", version=3, architecture=5, sampleRate, inputChannels=1,
channels, outputChannels=1, controlCount=0, layerCount, flags, headKernel, reserved
then layerCount × { kernelSize, dilation } as u32 pairs, then the float payload
```

`floatCount` is computed from that table with the formula derived above and must equal the payload
size exactly, preserving the existing "reject on any mismatch" posture. Converter
`nts-nam-import` writes the full artifact directory — `model.bin`, `manifest.json` (with SHA-256),
`normalization.json` (`inputRmsDb` from `metadata.loudness`), `license.txt` (from
`metadata.modeled_by`), and `test-vectors/` whose `output.f32` is produced by **N1's verified
renderer**. That makes the plugin's existing validation
([NeuralAmpProcessor.cpp:83-87](../engine/ml-runtime/src/NeuralAmpProcessor.cpp#L83)) a genuine
correctness gate on the C++ WaveNet: if N5 is wrong anywhere, the model refuses to load.

**Files.** `ml/nts_ml/export/packed.py`, `ml/nts_ml/nam/convert.py`, `ml/nts_ml/cli.py`,
`ml/nts_ml/schemas/model.py`, `ml/tests/test_nam_convert.py`.

**Tests.** Header round-trip; truncated/oversized payload rejection; layer table bounds
(`layerCount ≤ 64`, `kernelSize ∈ [2, 64]`, `dilation ≤ 4096`); `validate_artifact` acceptance.

**Done.** Every one of the 354 captures converts without error at both tiers, and each artifact
passes `validate_artifact`.

---

## N5 — C++ WaveNet runtime ✅

**State.** Shipped as `engine/ml-runtime/{include/nts/ml,src}/PackedWaveNetModel.*` and
`NeuralModel.*`, with `NeuralAmpProcessor::Slot` now holding `NeuralModel`. `nts_ml_runtime_tests`
covers load/reject, block-size invariance, reset determinism, stationarity under silence, dispatch,
staging through the processor, and zero heap allocation on the audio path. All green through the
project's own CTest.

Two design points resolved differently from the sketch below:

- **Priming is snapshotted, not re-run.** `load` primes once and stores the resulting buffers;
  `reset` is then a copy, so the audio thread never pays the 6 347-sample warm-up. That also removes
  the need for a separate crossfade pre-roll: an incoming model is already at its silent steady
  state when the slot is published.
- **No virtual interface.** `NeuralModel` holds both implementations by value and dispatches on the
  header version. A `unique_ptr<Base>` per channel per slot would have made `Slot` non-copyable, put
  an allocation between staging and activation, and added an indirect call per block for a choice
  fixed at load time.

**Verified against real data.** All 354 converted captures reproduce their exported test vectors at
**worst ESR 1.8e-12** (worst absolute difference 2.2e-6 against the 1e-4 artifact tolerance). Cost
at 64-sample blocks, 48 kHz, scalar and unoptimised: standard tier **17.1% of one core**, lite tier
**5.5%**. Not yet done: the ASan leg has not been run, and there is no SIMD.

**Problem.** `NeuralAmpProcessor::Slot` holds `std::array<PackedTanhModel, 2>` concretely
([NeuralAmpProcessor.h:67](../engine/ml-runtime/include/nts/ml/NeuralAmpProcessor.h#L67)); there is
no seam for a second model type, and no WaveNet inference exists.

**Design.** Two separable pieces:

1. **Seam.** Introduce `nts::ml::NeuralModel` (`load`, `reset`, `processSample`, `process`,
   `setControls`, `sampleRate`, `memoryBytes`) implemented by both `PackedTanhModel` and the new
   `PackedWaveNetModel`, with a factory that dispatches on the header's architecture code. Keep the
   allocation discipline: everything sized in `load`, nothing allocated in `process`.
2. **Inference.** Per layer: dilated causal conv over a ring buffer sized `(k−1)·d + 1`, input mixin
   from the condition channel, LeakyReLU(0.01), 1×1 with residual accumulation into the trunk, then
   the 16-tap head conv, bias, and head scale. State totals ≈ 6.3 k × `channels` floats.

Two behaviours need explicit decisions rather than defaults:

- **Crossfade.** `fadeSamples` is 1024 ([NeuralAmpProcessor.h:81](../engine/ml-runtime/include/nts/ml/NeuralAmpProcessor.h#L81)),
  but a reset WaveNet needs 6 347 samples of history before its output is trustworthy. Either
  pre-roll the incoming model's history on the worker thread before publishing the slot
  (preferred — it keeps the audio thread unchanged), or lengthen the fade for WaveNet slots.
- **Determinism.** Parity with the Python renderer must hold in float32 with a fixed summation
  order. Do not reorder accumulations for speed before the parity test is green; optimise after.

**Files.** `engine/ml-runtime/include/nts/ml/{NeuralModel,PackedWaveNetModel}.h`,
`engine/ml-runtime/src/PackedWaveNetModel.cpp`, `NeuralAmpProcessor.{h,cpp}`, `CMakeLists.txt`.

**Tests.** `Tests/MlRuntimeTests.cpp`: load/reject matrix for the v3 header; block-size invariance
(1, 32, 64, 1024 identical); state reset determinism; `Tests/MlRuntimeBenchmarks.cpp` extended with a
WaveNet budget. `nts_ml_runtime_parity` gains a WaveNet case driven by N1's vectors
([CMakeLists.txt:468-478](../CMakeLists.txt#L468)).

**Done.** A real converted capture loads and passes test-vector validation in the plugin; lite tier
holds a full block budget with headroom at 48 kHz/64 samples on the CI reference machine; ASan leg
clean.

---

## N6 — Import and library UX ◐ partial

**State.** Option 1 below now works: the artifact loader accepted only manifest format versions 1
and 2 and so rejected every converted capture outright
([PluginProcessorAssets.cpp:166](../Source/PluginProcessorAssets.cpp#L166)); it now accepts 3. The
existing chooser already selects directories, so a converted capture loads through the unchanged
path — manifest schema, SHA-256, and test-vector validation all applying as they do to a trained
model. Widening that accept-list is needed under *every* option below, so it commits to none of them.

**Now covered.** `testNeuralArtifactLoading` in `Tests/WrapperTests.cpp` writes artifact directories
and drives `TubeForgeAudioProcessor` through its background loader: a packed WaveNet loads and
passes audio, a manifest declaring a schema this build does not know is refused, and a model whose
bytes do not match its manifest digest is refused. The artifact-loading path had **no** C++ coverage
at all before this — the plug-in side of every model format, not just WaveNet, was untested.

The acceptance assertion was verified by mutation: reverting the version gate to `> 2` makes it fail
with `a converted NAM capture loads through the artifact path`, so it is load-bearing rather than
decorative.

**Still not done:** tier selection in the UI, capture metadata in the library row, and digest-keyed
caching of imported captures.

### The design question, and how it was resolved

**The plan's design did not survive contact with the validation model, and the resolution is
better than any of the three options it listed.** The plan said the plug-in should convert a
`.nam` on its background worker. It can — a JSON reader plus the v3 packer is straightforward —
but a conversion done inside the plug-in cannot produce *independent* test vectors. Rendering the
expected output with the same C++ code the vectors are meant to police makes
[NeuralAmpProcessor.cpp:83-87](../engine/ml-runtime/src/NeuralAmpProcessor.cpp#L83) a tautology,
and that check was the strongest correctness guarantee in the feature.

The three ways forward were:

1. **Import the artifact, not the capture.** The file chooser accepts a converted artifact
   directory. No new C++, parity guarantee intact, costs the user a command line.
2. **Convert in the plug-in, and be explicit about what is checked.** Best UX; apparently the
   weakest guarantee; needs a C++ NAM reader duplicating the Python one, with the duplication
   itself a maintenance risk.
3. **Ship a bundled converter.** Preserves the guarantee, but makes the plug-in depend on an
   external interpreter.

**What was built is (2), with the guarantee restored by a different gate.** Conversion is a pure
format transform — `pack_wavenet` copies the weight vector through verbatim and reinterprets
nothing — so the two converters can be compared *directly* rather than through the audio they
eventually produce. `nts_nam_parity` requires the C++ converter to write **byte-identical**
`model.bin` to `nts-nam-import` for every capture, and byte identity is a strictly stronger claim
than a rendering tolerance. It also transfers the whole existing chain to the in-plug-in path
untouched: Python renderer checked against upstream `neural-amp-modeler`, C++ runtime checked
against the Python renderer, C++ converter checked against the Python converter.

Measured, at the time of writing: **708 of 708 comparisons byte-identical** — all 354 corpus
captures at both tiers, zero failures, including the decimal-to-`float32` conversion of every one
of the 12 146 weights in a standard-tier model. The duplicated reader is therefore pinned to the
Python one by construction rather than by discipline.

What an in-plug-in conversion still cannot do is produce independent vectors, so it does not
pretend to: its artifacts carry `"testVectorSource": "runtime"` in the manifest, and the vectors
prove the model loads, primes and runs deterministically rather than proving parity. The
distinction is recorded in the artifact rather than only in a comment.

**Problem.** Once conversion works, users still have no way to get a `.nam` into the plugin —
`NeuralCapturePage`'s `loadModel` chooses an artifact directory, not a capture file.

**Design.** Accept `.nam` in the file chooser; on selection, run the conversion on the existing
background worker (reusing the N4 layout, so the runtime path is identical to any other artifact) and
cache the result under the user data folder keyed by source SHA-256. Surface tier selection
(lite/standard) and show `gear_make`, `tone_type` and `validation_esr` from metadata in the library
row.

**Files.** `Source/ui/NeuralCapturePage.{h,cpp}`, `Source/ui/ProfileLibraryPage.cpp`,
`Source/PluginProcessor.cpp`, `Source/StudioServices.cpp`.

**Tests.** `Tests/IntegrationTests.cpp`: import a fixture `.nam`, assert `AssetLoadStatus::ready` and
a populated status string; assert a corrupt file yields `failed` with a specific message and no
crash.

**Done.** Drag a `.nam` in from `amp_learning/`, hear it, see its metadata.

---

## N7 — Performance gate and documentation ✅

**State.** `nts_ml_runtime_cli` now loads through `NeuralModel`, so `parity_driver.py` drives every
architecture through one executable, and it gained a WaveNet case gated on the tolerances the
artifact itself declares. That closes the loop: the Python renderer is checked against upstream
`neural-amp-modeler` in `test_nam_reader`, and the C++ runtime is checked against the Python
renderer in CTest — so the runtime is tied to upstream without CI needing torch.
`nts_ml_runtime_benchmarks` gained both tiers at the real corpus geometry, with the lite tier held
to 25% of budget; `Tests/PackedFixtures.h` holds the shared fixture builder.
Docs: [nam-capture-guide.md](nam-capture-guide.md) and a section in
[format-compatibility.md](format-compatibility.md).

**One caveat worth recording.** The performance gate fails in a **Debug** build, on the pre-existing
recurrent stereo case at 103% of budget — before any WaveNet code runs. It is a Release measurement
and passes there. Nothing to fix, but a Debug CTest run will show one red suite.

**Design.** Add the WaveNet case to the benchmark acceptance gate at both tiers, and document the
format, the conversion CLI, and the licensing position in
`docs/format-compatibility.md` plus a new `docs/nam-capture-guide.md`. Record measured cost against
the 83 / 565 MMAC/s envelope above.

**Done.** CI fails if WaveNet inference regresses past budget; guide covers import, tiers, sample
rate, and redistribution.

---

## Sequencing

| Order | Item | Days | Depends on | State |
|---|---|---|---|---|
| 1 | N0 hygiene + IRs | 0.25 | — | ✅ |
| 2 | **N1 reader/renderer + parity gate** | 1.0 | — | ✅ |
| 3 | N2 distillation | 1.5 | N1 | ✅ |
| 4 | N3 catalogue | 1.0 | N1 | ✅ |
| 5 | N4 NTSM v3 + converter | 1.0 | N1 | ✅ |
| 6 | N5 C++ WaveNet runtime | 3.0 | N4 | ✅ |
| 7 | N6 import UX | 1.0 | N5 | ✅ |
| 8 | N7 gate + docs | 0.5 | N5 | ✅ |

**Nothing remains.** N6 landed as option (2) above — a C++ `.nam` reader and v3 packer, in-process
zip extraction, digest-keyed conversion, a capture library with a page of its own and pickers on the
Pedals and Neural Capture pages, and gear-type classification — with the parity guarantee restored by
`nts_nam_parity` rather than given up. N7's benchmark budget, `nts_ml_runtime_parity` WaveNet case and
guide were already in place; the guide now also covers importing without the command line.

**Total ≈ 9.25 days.** Track A (N0–N3) is 3.75 days and ships playable models on the current
runtime. Track B (N4–N7) is 5.5 days and makes the whole NAM ecosystem loadable exactly.

## Open decisions for the author

1. **44.1 kHz.** Distil at both rates (cheap, Track A only), resample around the model in the runtime
   (adds latency and colour), or keep the hard rejection and document 48 kHz as a requirement?
2. **Default tier.** Ship lite (`c = 3`, ~83 MMAC/s) as the default with standard as an opt-in, or
   the reverse? This is a CPU-versus-accuracy call that should be made against measurement on the
   target machine, not assumed.
3. **Redistribution.** Are converted or distilled artifacts ever shipped in a tone package, or is the
   feature strictly "import your own"? This decides whether `TonePackage` needs provenance fields for
   third-party captures.
4. **Scope of the reader.** N1 deliberately rejects the v0.7 conditioning extensions no file here
   uses (FiLM, gating, `head1x1`). Accepting them later is additive; assuming them now is wasted work.
5. **`metadata.gain`.** The plan originally said to apply it when rendering. The implementation does
   not: what the field means is not documented in the format, applying it would silently rescale
   every distilled target, and level matching is already handled properly by the measured input and
   output calibration figures. It is carried into `targetSettings.captureGain` so the decision stays
   reversible. Confirm or overrule.
