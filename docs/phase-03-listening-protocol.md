# Phase 3 loudness-matched listening protocol

## Source set

Record dry, unprocessed 24-bit DI performances at 48 kHz for each category:

1. clean guitar arpeggio
2. crunch guitar open chords
3. high-gain rhythm palm mutes
4. lead guitar with bends and sustained notes
5. clean fingerstyle bass
6. picked bass
7. distorted bass with sustained fundamentals
8. slap bass transients

Store pickup type, instrument, interface input, calibration peak/RMS, player, and take identifier with
each source. Do not normalize the source before recording its calibration values.

## Rendering and loudness match

- Render candidates through the deterministic offline graph at identical sample rate and block size.
- Measure gated integrated BS.1770 loudness after discarding the first 500 ms of state warm-up.
- Match candidates to within 0.2 LU using output trim only; never alter drive to match loudness.
- Keep a hidden reference render and randomize A/B assignment independently for every trial.

## ABX session

Use at least 12 randomized trials per comparison and allow instant, click-free switching. The listener
first identifies X as A or B, then scores pick response, low-end stability, chord separation, sustain,
noise, harshness, and overall preference on a seven-point scale. Headphone/speaker model and playback
level are recorded. A result is considered identifiable only when the binomial probability is below
0.05; preference scores are reported separately from identification.

## Acceptance record

For every topology and source category, retain render hashes, loudness values, trial order, responses,
confidence, comments, and aggregate statistics. Human listening results are release evidence and are
not fabricated by the automated test suite.

The executable pack generator and analyzer are documented in `phase-03-abx-tools.md`. Automated tests
verify the BS.1770 matching, blinding, hashing, and statistical calculations with synthetic fixtures;
those tests are infrastructure verification and are not listening evidence.
