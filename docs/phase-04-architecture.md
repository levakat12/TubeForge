# Phase 4 ML and dataset architecture

## Layers

`ml/nts_ml` is a typed, NumPy-based Python package. It deliberately keeps the paired-audio,
alignment, evaluation, artifact, and governance layers independent from a specific deep-learning
framework. A new model can implement the `AudioModel` protocol and continue using the same dataset,
evaluation, export, and registry paths.

- `schemas`: validated camel-case session and model manifests backed by dataclasses.
- `datasets`: 16/24/32-bit PCM access, capture-package discovery, machine-readable quality reports,
  session/performance splits, and random-access streamed chunks.
- `alignment`: impulse, cross-correlation, GCC-PHAT, local latency measurement, clock-drift
  correction, polarity handling, and an HTML inspection report.
- `augmentation`: deterministic paired gain, polarity, channel, input-noise, and aligned jitter.
- `models`: a small recurrent reference architecture plus a model protocol/factory.
- `losses`: waveform, pre-emphasis, spectral, DC, loudness, and optional embedding losses.
- `training`: TOML resolution, deterministic AdamW/BPTT reference training, checkpoints, and SQLite
  experiment governance.
- `evaluation`: the fixed 14-clip taxonomy and automatic JSON metric reports.
- `export`: immutable model artifacts, SHA-256 validation, test vectors, and a versioned registry.

## History-aware streaming

`StreamingPairedDataset` indexes WAV paths and frame offsets, not decoded recordings. Each item reads
only `history_samples + chunk_samples`. The output read starts at the validated session latency, the
history region warms recurrent state, and the boolean loss mask supervises only the prediction
region. Chunks never cross take boundaries. Splits operate on session/performance identities before
chunking, preventing adjacent phrases from leaking into validation or test data.

## Packed runtime format

The custom packed path was selected from the Phase 4 candidate export paths because it has no runtime
framework dependency and maps directly to real-time C++. `model.bin` contains a little-endian `NTSM`
header followed by float32 input, recurrent, bias, output, and output-bias arrays. The artifact also
contains a validated manifest, normalization data, license text, and fixed float32 test vectors.

`nts_ml_runtime` loads and validates weights outside the audio callback. `processSample()` performs no
allocation or locking. `nts_ml_runtime_parity` exports in Python, runs the compiled C++ executable,
and checks maximum error, RMS error, accumulated drift, and reset determinism.

## Reproducibility and governance

Every training run saves `resolved-config.json`, uses an explicit NumPy seed, and records its commit,
configuration, seed, dataset version, hardware, elapsed time, checkpoint, metrics, and exported hash
in SQLite. Registry versions are immutable and include a license file so provenance is part of the
artifact rather than an external convention.

The CI suite validates schemas and alignment, runs deterministic tiny training, creates and validates
an export, generates a fixed evaluation report, and executes Python/C++ parity.
