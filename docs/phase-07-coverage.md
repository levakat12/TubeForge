# Phase 7 coverage

Overall implementation coverage: **90%** of the Phase 7 plan.

The percentage is a requirement-weighted implementation estimate. It measures shipped infrastructure and
tests, not perceptual accuracy. The included benchmark uses controlled synthetic rig fixtures; no licensed,
multi-performance real-amp corpus was supplied, so real-world retrieval/classification accuracy is not claimed.

| Plan area | Coverage | Evidence |
|---|---:|---|
| Tone feature extractor | 97% | All spectral, dynamic, nonlinear, spatial/production, and context categories; bounded multi-resolution offline analysis |
| Learned tone encoder | 90% | Contrastive learned projection, normalized/versioned 128D output, save/load, C++ projection loading; waveform/CNN alternative not implemented |
| Disentanglement and interpretable heads | 90% | Independently versioned tone/performance/production branches plus normalized ridge heads and residual uncertainty |
| Instrument and technique context | 84% | Guitar/bass, pick/fingerstyle, gain family, register, continuous descriptors, and certainty; real-corpus calibration remains pending |
| Confidence and tone report | 98% | Seven confidence factors, aggregate, per-descriptor uncertainty, leakage/separation/short/OOD warnings, JSON output |
| Similarity metric | 96% | Embedding, spectral, dynamics, transient, low-frequency, and nonlinear terms with separate guitar/bass weighting |
| Tone-profile database | 93% | Required metadata, deterministic JSON round-trip, compare and exact nearest search; session UI does not yet persist automatically |
| Evaluation benchmark | 86% | Nine controlled rig labels, retrieval/family/classification, robustness, calibration, OOD, and ablation APIs/tests; synthetic rather than recorded corpus |
| Product integration and tests | 94% | Async analyzer page, 5 s/30 s, silence rejection, leakage confidence, compare/search, loudness and pitch variation tests |

## Acceptance status

- Synthetic same-rig variants retrieve more closely than a deliberately different nonlinear rig.
- RMS normalization and explicit feature separation keep loudness from dominating the embedding.
- Pitch variation is less influential than the rig/nonlinearity change in the acceptance fixture.
- Bass and guitar similarity paths use different low-frequency, spectral, and transient weights.
- Short or contaminated clips reduce confidence and produce warnings.
- Reports expose instrument, technique, gain category, seven interpretable descriptors, and uncertainty.
- Embedding and analysis formats carry explicit version identifiers.
- File decoding, FFT analysis, projection, and profile search run off the playback callback.
- Profiles compare, serialize, deserialize, and search successfully.

## Deliberately incomplete

- A real licensed corpus covering many performances for all nine rig families is still required to train and
  calibrate a production encoder, classifier, and descriptor heads.
- The shipped C++ projection has a deterministic bootstrap fallback; the learned projection loader and Python
  trainer are complete, but no synthetic weights are mislabeled as a production model.
- Exact search is intentional for the current small collection; approximate nearest-neighbor search is deferred.
- The UI keeps a session database. Automatic on-disk profile-library persistence and user licensing workflow
  are future product work, while the data schema and JSON persistence API are implemented.
