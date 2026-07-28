# Phase 8 coverage

Overall implementation coverage: **95%** of the Phase 8 plan.

This is a requirement-weighted implementation estimate, not a source-separation quality or original-hardware
identification claim. The executable fallback and reconstruction tests use controlled synthetic material. No
licensed commercial-song stem corpus or production guitar-separation model was supplied.

| Plan area | Coverage | Evidence |
|---|---:|---|
| Song import and cache | 94% | Worker-thread decode, 48 kHz resample, 20-minute bound, local WAV cache, deterministic source/model key; decoder availability remains build/license dependent |
| Separation architecture | 96% | Local Demucs six-stem model, direct guitar and bass stems, exact model/device metadata, private cache, and explicit deterministic fallback |
| Separation execution | 98% | Cancellable worker process, progress sidecar, deterministic identity, automatic CUDA/CPU execution, consistency metric, and fallback diagnostics |
| Region and stereo workflow | 98% | Timestamped selectable parts, clean/crunch/distorted character, dominant pitch/confidence, likely tuning/cent offset, energy/leakage/stability/clipping/reverb/duration/polyphony confidence, recommendation, and six stereo modes |
| Reference normalization | 91% | Loudness/DC/silence, optional broad room-tail reduction, production-EQ estimate, preserved nonlinear/dynamic analysis |
| Editable rig reconstruction | 93% | Coarse descriptor priors, broad planned parameter coverage, candidate pool, real amp renders, gradient-free ranking, complexity/confidence, multiple playable presets |
| Neural reference mode | 66% | Versioned 128D conditioning vector is generated; a trained reference-conditioned neural runtime/model is not bundled |
| User DI adaptation | 78% | Input/EQ/dynamics/output adaptation schema and live preset response; initial search uses the selected reference until a dedicated user-DI capture is supplied |
| Result schema and legal boundary | 100% | Source hash/region/model/confidence, parameter presets, separate similarities, warnings, and source-audio-free export |
| Product UI | 91% | Nine-stage progress, import, target/stereo, detected-part selection and rebuild, pitch/tuning/gain details, cancellation, candidate comparison/application/export; no waveform dragging or loudness-matched A/B player yet |
| Testing and robustness | 93% | Direct-neural-stem routing, pitch detection and clean/driven classification plus bass-heavy/sparse/dense/transient/stereo/mono/clipping/short-region paths, cancellation, consistency, normalization, candidate generation and safe export |

## Acceptance status

- A user can import a supported song and isolate the bass stem through an offline worker.
- Guitar and bass extraction use direct neural stems when the local Demucs runtime is installed; the earlier
  supported-subset behavior is retained only for explicit fallback operation.
- Region confidence, leakage, clipping, reverb, density, and ambiguity warnings are computed and displayed.
- Users can select a timestamped clean, crunch, or distorted part and rebuild from that cached region; dominant
  pitch and qualified tuning-family estimates are stored in the safe result metadata.
- Reconstruction produces four ranked editable candidates by default.
- Applying a candidate routes new live input through the existing amp engine.
- Shared `.tfreconstruct` profiles contain no source samples or isolated stems.
- Tone similarity and full-recording similarity are separate fields and UI values.
- Dense, clipped, short, reverberant, or leaking references reduce confidence instead of producing false certainty.

## Deliberately incomplete

- The open Demucs runtime/model is provisioned separately rather than embedded in the repository. Release
  packaging still needs to bundle or install the Python/PyTorch runtime and downloaded weights per platform.
- A dedicated dry user-DI capture/import step is still needed for pickup- and technique-specific optimization.
- Waveform-region dragging, stem audition, and loudness-matched reference/render A/B are future UI work; the
  timestamped detected-part selector already provides bounded manual choice.
- Reference-conditioned neural inference is represented by its versioned conditioning vector but awaits a
  compatible trained runtime artifact.
- Reconstruction parameters are plausible virtual configurations and do not identify the original amplifier.
