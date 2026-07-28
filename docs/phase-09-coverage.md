# Phase 9 coverage

Overall implementation coverage: **91%** of the Phase 9 plan.

This is a requirement-weighted implementation estimate. Automated labelled scenarios validate deterministic
technical rules and parameter safety; they do not establish subjective usefulness across all musicians or rigs.
No expert-labelled recommendation corpus or user-study data was supplied.

| Plan area | Coverage | Evidence |
|---|---:|---|
| Live signal diagnostics | 93% | Input/output level, crest, clipping, noise proxy, intermittency, zero crossings, phase, latency, active-pickup and calibration-related evidence |
| Tone-problem classification | 94% | Input, gain, spectral, dynamic and routing rules including bass split, gate, cabinet and phase cases; duplicated external host cabinets remain inferential |
| Recommendation engine and goals | 97% | Structured rule actions and all nine requested goal modes with bounded real parameter mappings and expected effects |
| Safe parameter application | 100% | Fixed allow-list schema, value/delta/stale-state validation, no arbitrary graph access, explicit preview before mutation |
| Preview, confidence and rollback | 97% | Complete before/after snapshots, smoothed live preview, accept/reject, exact rollback and multi-step undo history |
| Explanation layer | 100% | Stable diagnosis, evidence, confidence, beginner/advanced text, exact affected parameters and alternatives/expected effects |
| Local personalization | 94% | Instrument/gain/brightness/clean blend/cabinet and acceptance patterns, versioned local JSON, enable/disable and clear controls |
| Real-time constraints | 94% | Fixed SPSC summary frames, no audio storage/allocation/locks, off-thread aggregation, 750 ms rate limit, preview feedback-loop suppression |
| Learned recommendation path | 55% | Preference-informed ranking and validated action boundary are ready; supervised ranker training awaits expert/acceptance data as planned for later |
| Evaluation | 89% | Eight labelled scenarios, clean false-positive case, corrective direction, schema attacks, exact rollback, preference round-trip and queue tests; no human usefulness study |

## Acceptance status

- Common synthetic technical problems are detected with stable problem identifiers.
- Suggestions cannot apply without Preview and can be rejected or undone to the exact prior values.
- Every suggestion identifies confidence, evidence, exact changes, expected effect, and both explanation levels.
- Unknown, stale, non-finite, oversized, or out-of-range changes are rejected before parameter publication.
- Beginner and advanced explanations originate from the same rule and therefore do not contradict each other.
- Audio-thread work is limited to compact summary measurement and a lock-free queue push.
- Corrective directions are tested for labelled scenarios such as mud, clipping, gain, bass fundamentals and phase.
- Personalization is local, optional, and clearable.

## Deliberately incomplete

- Recommendation usefulness and acceptance rate need a structured musician/expert user study.
- A learned ranking or parameter predictor needs synthetic sweeps plus expert-labelled accepted/rejected examples;
  the current production path remains interpretable rules and never fabricates learned weights.
- Live spectral diagnosis uses the latest Phase 7 analysis while the callback supplies bounded scalar summaries;
  a future background downsample/STFT stream could update spectral evidence continuously.
- External duplicated cabinet processing cannot be known reliably from inside a plug-in and is reported only as
  a low-confidence cabinet-chain possibility when the measured rolloff supports it.
