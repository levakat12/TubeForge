# Phase 7 tone-analysis architecture

## Runtime boundary

Tone analysis is deliberately offline. The editor asks the processor to decode a selected audio file on a
`std::jthread`; the worker copies at most 60 seconds into analysis-owned memory and never calls or blocks
`processBlock`. Results are published through a small mutex-protected snapshot used by the message-thread UI.
This keeps the audio callback independent of file I/O, FFT work, model projection, and database search.

## Analysis pipeline

1. Validate sample rate, duration, finite samples, and reject silence.
2. Remove DC and normalize RMS so the embedding and similarity are not dominated by input loudness.
3. Extract bounded multi-resolution spectra at 512, 4096, and 8192 samples plus frame-level dynamics.
4. Derive spectral, dynamic, nonlinear, spatial/production, and instrument-context measurements.
5. Convert measurements into seven user-facing descriptors with explicit uncertainty.
6. Build a 64-value feature vector that excludes absolute loudness and avoids register dominance.
7. Project and L2-normalize a versioned 128-dimensional tone embedding.
8. Compare or search using the embedding plus interpretable distances with different bass/guitar weights.

Spectral measurements cover low extension, low-mid buildup, mids, attack, rolloff, resonances, centroid,
slope, and flatness. Dynamics cover crest, transients, compression, attack/release, sustain, gain-reduction
proxy, and loudness range. Nonlinear measurements cover eight harmonic bands, odd/even balance,
intermodulation, saturation onset, asymmetry, and level-dependent spectrum. Stereo analysis estimates width,
room, double tracking, pan, modulation, delay, and correlation.

The initial C++ context classifier and descriptors are measurable, documented proxies. They do not present
subjective amp settings as exact measurements. Aggregate confidence combines signal/leakage, classifier
certainty, duration, spectral coverage, polyphony, separation quality, and model-domain proximity. Each
descriptor carries uncertainty, and the report warns about short clips, leakage, separation artifacts,
limited coverage, and out-of-domain material.

## Learned encoder tooling

`ml/nts_ml/tone` provides a multi-resolution audio representation, contrastive supervised projection,
128-dimensional normalized embeddings, model save/load, and separate tone, performance, and production
branches. Interpretable ridge heads predict normalized descriptors or binary instrument/technique targets
and return residual-based uncertainty. The C++ encoder accepts an exported row-major projection and
propagates its model version into every profile.

The benchmark API covers the nine planned rig families through labeled controlled fixtures and reports
same-rig/family retrieval, instrument accuracy, pitch/loudness/short/leakage robustness, calibration error,
OOD rejection, and spectral ablation impact. Real licensed recordings should replace the synthetic fixture
before model-quality claims or shipping a learned projection.

## Profiles and similarity

Each profile stores its ID, embedding, full descriptive features, instrument, source type, capture quality,
model version, user tags, and licensing metadata. JSON serialization is deterministic and round-trips those
fields. Search is exact cosine/hybrid search because the current collection is small; an approximate index
should only be introduced after measurement shows the collection size justifies it.

Similarity combines embedding, multi-resolution spectral, dynamics, transient, low-frequency, and nonlinear
distances. Bass comparison increases the low-frequency contribution; guitar comparison emphasizes embedding,
spectrum, and transient character. The UI exposes the closest current-session profile rather than silently
turning the score into a claim of amp identity.
