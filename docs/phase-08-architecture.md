# Phase 8 source separation and reconstruction architecture

## Offline boundary and import

The entire workflow runs on a cancellable `std::jthread`. JUCE decodes WAV, AIFF, FLAC, Ogg, and any common
compressed format enabled by the licensed application build. Audio is resampled to a 48 kHz working rate and
bounded to 20 minutes. The decoded stereo song is cached locally below the TubeForge application-data cache;
it is never loaded by or copied through `processBlock`.

The source/cache identity is a deterministic FNV-1a digest over sample rate, channel samples, chunk settings,
and separation model version. Every result records the cache identity and model version. Cache data remains
local; shared reconstruction files contain only hashes, metadata, embeddings, and editable parameters.

## Separation and target extraction

The primary separator is a local Demucs worker using the six-stem `htdemucs_6s` model. It produces vocals,
drums, bass, other, guitar, and piano stems, records the exact Demucs/model/device version, reports chunk
progress through an atomic sidecar, supports cancellation by terminating the worker, and caches stem WAVs
under the private application-data directory. CUDA is selected when available and CPU inference remains
supported. The source recording is never uploaded.

Bass and guitar now use their direct neural stems. The earlier deterministic filter/onset separator remains as
an explicit availability fallback and records an actionable warning; it is no longer presented as production
ML separation. The runtime is installed with `scripts/setup-ml-separator.ps1`, and production installers can
run the equivalent dependency/model provisioning step. Full stereo, left, right, mid, side, and
dominant-panned estimates remain available for double-tracked or panned material.

The region scorer examines overlapping windows for target energy, vocal/drum leakage, stereo stability,
clipping, room/reverb correlation, duration, and polyphonic density. It recommends the highest-confidence
region and emits warnings for short, clipped, reverberant, dense, or leaking sources. A bounded autocorrelation
pass estimates dominant pitch and tuning offset, while transient density, derivative energy, zero crossings,
and crest factor classify each candidate as clean, crunch, or distorted. The lowest reliable observed pitch
maps to a clearly qualified tuning family estimate; the UI does not claim certainty when an open string was
not observed.

## Reference and reconstruction

Reference normalization removes DC, trims edge silence, records loudness, applies bounded RMS normalization,
estimates broad production-EQ shape, and can reduce a simple room-tail correlation. Absolute loudness is
separated from the tone feature vector while nonlinear and dynamic measurements remain available to Phase 7.

The editable-rig path proceeds in four stages:

1. Phase 7 context and descriptors establish instrument, gain, brightness, tightness, compression, cabinet,
   and bass-split priors.
2. A deterministic pool varies stage count/drive/bias, pre-EQ, tone stack, master, sag, feedback, presence,
   resonance, cabinet cutoff/blend, post EQ, compression-related adaptation, crossover, and clean blend.
3. Every candidate is rendered through the actual Traditional Amp engine using the selected reference as the
   initial adaptation signal, then analyzed again.
4. Candidates are ranked by the hybrid tone metric, recording/loudness similarity, complexity penalty, and
   confidence. Several candidates are returned instead of asserting a single original hardware identity.

The reference embedding is also returned as a versioned 128D neural conditioning vector. A production
reference-conditioned neural runtime/model is not bundled in this phase; the editable rig is the playable path.
Applying a candidate writes its amp parameters through the existing parameter system, so new live DI input
responds to the reconstructed rig without adding playback latency.

## UI and legal boundary

The Song Reconstruction page shows the nine workflow stages, target and stereo choices, progress, cancellation,
and a timestamped list of playable parts with clean/crunch/distorted character, dominant pitch, likely tuning,
cent offset, and confidence. Selecting a part reruns analysis and rig ranking from cached neural stems without
repeating Demucs inference. The page also shows separate tone and recording similarities, candidate selection,
live application, and safe export. Exports serialize the rig preset and provenance only. The interface calls
results plausible virtual approximations and never claims identification of the recording's physical hardware.
