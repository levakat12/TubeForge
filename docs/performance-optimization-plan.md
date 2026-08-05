# TubeForge performance optimization plan

A sequenced, implementation-ready plan for reducing audio-thread CPU cost, with the explicit goal of
making the plug-in usable on low-end Windows machines (dual-core ~2.0 GHz, SSE2-only, integrated
graphics, 4 GB RAM) at 48 kHz with a 128–256 sample block.

Every item states what is slow, the evidence for it, the design to apply, the files touched, how it
is measured, and what "done" means.

Items marked done in the status table below are implemented and measured; everything else is
design only. Line references are against the tree at commit `f55407d` and have already drifted in
the files that were edited — re-check them before working on an item.

## Status

| Workstream | Items | State |
|---|---|---|
| P — Measurement harness | P1, P2 | ✅ P1, P2 done |
| Q — Traditional amplifier hot path | Q1 → Q5 | ✅ Q1 → Q5 done |
| R — Neural runtime | R1 → R4 | ✅ R1, R2, R3, R4 done (R2 opt-in, default off) |
| S — Shared DSP primitives | S1 → S3 | ✅ S1, S2, S3 done |
| T — Always-on per-block cost | T1 → T4 | ✅ T1 → T4 done |
| U — Build configuration and dispatch | U1, U2 | ✅ U1 done; U2 measured and rejected |
| V — Quality tiers | V1, V2 | ✅ V1, V2 done |

**Ordering constraint:** P1 lands before anything else. Every item below claims a speedup, and a
claim without a baseline number is not a result.

### Measured result of the first milestone

End-to-end traditional amplifier, stereo, 48 kHz, 128-sample block, controls settled. Measured with
`nts_amp_benchmarks` by building the same harness against the tree with and without the changes.
Percentages are of the callback budget for one instance.

| Configuration | Before | After | Speedup |
|---|---|---|---|
| 1× oversampling, 4096-tap cabinet | 655.7 µs (24.6%) | 72.0 µs (2.7%) | **9.1×** |
| 2× oversampling, 384-tap cabinet | 191.1 µs (7.2%) | 71.9 µs (2.7%) | **2.7×** |
| 4× oversampling, 384-tap cabinet | 225.2 µs (8.4%) | 88.1 µs (3.3%) | **2.6×** |
| 4× oversampling, 4096-tap cabinet | 748.6 µs (28.1%) | 95.8 µs (3.6%) | **7.8×** |
| 8× oversampling, 384-tap cabinet | 353.3 µs (13.2%) | 107.0 µs (4.0%) | **3.3×** |
| 8× oversampling, 4096-tap cabinet | 908.9 µs (34.1%) | 131.3 µs (4.9%) | **6.9×** |

The 4096-tap rows are the cliff this plan was written around, and they confirm the estimate that
identified it: a user-loaded cabinet impulse cost 28% of the callback budget for a single instance
at the default oversampling factor. It now costs 3.6%, plus 128 samples of latency the host
compensates for.

Those rows moved twice: Q1's vector rewrite took them to about 5%, and Q5's partitioned path took
them the rest of the way once the cabinet learned to switch implementation.

Component level, from `nts_dsp_benchmarks` at the same configuration:

| Kernel | Before | After | Speedup |
|---|---|---|---|
| `direct_convolver` (256 taps) | 27.4 µs | 3.8 µs | **7.2×** |
| `oversampler4x` | 9.60 µs | 5.88 µs | **1.6×** |
| `oversampler8x` | 20.91 µs | 9.88 µs | **2.1×** |
| `biquad` | 0.66 µs | 0.63 µs | — (see Q4) |

Neural runtime, from `nts_ml_runtime_benchmarks`:

| Model | Before | After | Speedup |
|---|---|---|---|
| `recurrent64`, mono | 69.4 µs | 32.0 µs | **2.2×** |
| `recurrent64`, duplicated stereo | 133.9 µs | 32.3 µs | **4.1×** |
| `wavenet8` (8 channels) | 261.1 µs | 193.0 µs | **1.4×** |
| `wavenet3` (3 channels) | 66.2 µs | 67.9 µs | — (see R1) |

All 23 CTest suites pass, including the three Python-parity suites — which matters for R1 and R3,
since both change floating-point accumulation order in the inference path — and four new tests
written for these changes.

---

## Where the time goes

Measured statically by reading the callback in `Source/PluginProcessorAudio.cpp:134` and costing the
inner loops. Estimated cycles per **stereo output sample** at 48 kHz, traditional engine, four
preamp stages at 4× oversampling, default 384-tap cabinet impulses:

| Component | Est. cycles/sample | Share | Root cause |
|---|---|---|---|
| Cabinet convolution | ~2400–3000 | ~46% | 1536 MACs in `double`, branchy reverse ring walk |
| Saturator `std::tanh` | ~1300–1900 | ~29% | 64 transcendental calls/sample, half redundant |
| Oversampling filters | ~800–1000 | ~16% | 528 MACs in `double`, integer modulo per sample |
| Biquads (~25 instances) | ~500–750 | ~11% | state reloaded from arrays every sample |

Total ≈ 250–300 Mcycles/s per instance. On a 2.0 GHz core that is 12–15% of one core — survivable,
but only at the *default* cabinet length.

**The cliff is user-loaded impulse responses.** `maximumIrLength = 4096`
(`engine/amp/include/nts/amp/TraditionalAmp.h:293`). A 4096-tap stereo IR pair takes the cabinet from
1536 to 16 384 MACs per sample — roughly 1.2 Gcycles/s, over half a low-end core for a single
instance, before the rest of the chain. Any user who loads a real cabinet IR falls off this cliff.

The neural engine has a separate profile: it is dominated by transcendental calls in the LSTM gate
activations and by an unvectorizable strided weight layout in the WaveNet trunk. See workstream R.

---

## How to use this document

Items are ordered so that each one can land, be measured, and be shipped independently. Within a
workstream the ordering is deliberate — later items assume earlier ones.

### Conventions to preserve

- **Allocation-free and `noexcept` on the audio thread.** Every optimization here is a change to
  code that runs in `processBlock`. No item may introduce an allocation, a lock, or a system call on
  that path. The existing heap-allocation detector in `Tests/TestHarness.h` is the guard.
- **Output-preserving unless stated.** Items Q1, Q2, Q4, Q5, S1, S2, S3, T1–T4 are exactly
  output-preserving to within float rounding, and the existing regression fixtures and hashes must
  continue to pass unchanged. Items Q3, R2, R3 and everything in V **change the output** and are
  gated behind a quality tier or an explicit approximation flag.
- **No new latency.** Reported latency is compensated by the host and consumed by the dry-path delay
  in `PluginProcessorAudio.cpp:352`. Any item that would change `latencySamples()` says so
  explicitly and is scoped accordingly.
- **SSE2 is the floor.** The baseline binary must run on a pre-AVX CPU. Wider instruction sets are
  reachable only through the runtime dispatch in U1.

### Verification loop

For each item:

1. `cmake --build build --config Release`
2. `ctest --test-dir build -C Release` — all 23 suites must stay green.
3. `nts_dsp_benchmarks` and `nts_ml_runtime_benchmarks` — record the before and after numbers in the
   item's entry.
4. For output-preserving items, the DSP regression hashes in `Tests/DspTests.cpp` and the amp
   fixtures in `Tests/AmpTests.cpp` must be **bit-identical or within the existing tolerance**. A
   changed hash on an item marked output-preserving is a bug, not a new baseline.

---

## Workstream P — Measurement harness

### P1. Establish a CPU baseline that a change can be measured against

**What is missing.** `Tests/DspBenchmarks.cpp` and `Tests/MlRuntimeBenchmarks.cpp` exist and the
Phase 2 matrix is recorded in `docs/phase-02-benchmarks.csv`, but there is no end-to-end
*plug-in-level* figure: no "what fraction of the block deadline does one instance consume, per engine,
per oversampling factor, per IR length". Every claim in this plan is currently an estimate from
reading loops.

**Design.** Add a benchmark that drives `TubeForgeAudioProcessor::processBlock` directly with a fixed
DI fixture, and reports cycles-per-sample and percent-of-realtime for a matrix of:

- engine mode ∈ {traditional, neural, physical circuit}
- block size ∈ {64, 128, 256, 512}
- oversampling ∈ {1×, 2×, 4×, 8×}
- cabinet IR length ∈ {384 (default), 1024, 4096}

`DiagnosticsCollector` already computes callback time, maximum time, deadline and CPU load — reuse
its numbers rather than inventing a second timer. Emit a CSV in the same shape as
`docs/phase-02-benchmarks.csv` so the two can be compared over time.

**Files.** `Tests/PluginBenchmarks.cpp` (new), `CMakeLists.txt`,
`docs/performance-baseline.csv` (new).

**Done when.** The CSV exists, is committed, and the 4096-tap row shows the cliff described above —
confirming or correcting the estimate. If the estimate is wrong, **this document is corrected before
any optimization work begins.**

**Done, across two tools.** `nts_amp_benchmarks` covers the amplifier across oversampling factor,
cabinet length and whether the controls are moving. `nts_plugin_benchmarks` drives the real
`TubeForgeAudioProcessor::processBlock` across engine mode, block size, effect engagement and
editor state — the only figure that includes the shared front end, pedals, gate, effects, metering
and display feeds together. The plug-in shell's source list was factored into
`TUBEFORGE_SHELL_SOURCES` rather than copied a fourth time.

**The most important thing P1 established is the noise floor.** Repeated runs of the same binary on
this machine vary by about ±10% — for example `traditional_os4_editor_nofx` at a 512-sample block
measured 378.1, 351.6 and 375.3 µs across three runs. Anything worth less than roughly 15% of a
block **cannot be resolved here**, which is why U2 was rejected and why T1, T2 and T4 are recorded
as structural rather than measured. Large effects are unaffected: the 2.6–6.3× amplifier results
sit far outside that band.

**Two gaps remain.** The neural scenario measures only 4.9 µs because no model is staged, so it is
currently timing an empty path plus its surrounding chain; staging a fixture model would make that
row meaningful. And nothing yet drives a physical-circuit graph beyond the default.

### P2. A regression gate so wins do not silently erode

**What is missing.** Nothing stops a later feature from putting a `std::pow` back into a per-sample
loop.

**Design.** Extend the existing benchmark acceptance gate (the one A1 established in
`docs/improvement-plan.md`) with a per-engine ceiling drawn from P1's baseline plus a tolerance band
wide enough to absorb CI machine variance — start at 25% and tighten once the variance is known.
Advisory on pull requests, failing on `main`.

**Files.** `CMakeLists.txt`, `.github/workflows/`.

**Done when.** A deliberately introduced `std::pow` in a per-sample loop fails the gate locally.

**Done, as ratios rather than ceilings.** The per-engine microsecond budget this entry proposed was
the wrong instrument: an absolute figure encodes the machine it was recorded on and fails the build
on any slower one, and P1 measured this machine's own run-to-run spread at ±10%. Two ratios between
implementations measured in the same process go in instead, following the `simdSpeedup` check that
was already there:

- `dotProductWide` against `dotProductScalar` must exceed 1.3× (measures 5.8–6.3×). That kernel is
  what the convolver, the oversampler and the recurrent matrix-vector products all reduce to, so it
  is the single measurement covering the most optimised code in the engine.
- `direct_convolver_4096` against `buffered_partitioned_4096` must exceed 2× (measures 4.2×). This
  guards the asymptotic advantage rather than a time, so it survives being run on slower hardware.

Both are held well below what they measure: the thing worth catching is a ratio *collapsing*, which
means something structural has been undone, not a machine having a busy afternoon.

---

## Workstream Q — Traditional amplifier hot path

This workstream is ~90% of the traditional engine's cost. Q1 and Q2 together are the largest single
win available in the codebase.

### Q1. `DirectConvolver` is a branchy, scalar, double-precision reverse walk

**What is wrong.** `engine/dsp/src/Convolution.cpp:88`:

```cpp
double output {};
auto position = writePosition;
for (std::size_t tap = 0; tap < activeLength; ++tap)
{
    output += channelImpulse[tap] * channelHistory[position];
    position = position == 0 ? maximumLength - 1 : position - 1;
}
```

Three compounding defects:

1. `double` accumulation over `float` data — halves lane width for no audible benefit in a
   384-to-4096-tap FIR.
2. The history is walked **backwards**, so the hardware prefetcher works against it.
3. A data-dependent branch on every tap for the ring wraparound.

The combination is unvectorizable. The compiler emits one scalar multiply-add per tap with a
dependent branch, and this is the single most expensive loop in the plug-in.

**Design.** Doubled linear history buffer:

- Allocate `history` at `2 * maximumLength` per channel. On each write, store the incoming sample at
  both `position` and `position + maximumLength`.
- Store the impulse **time-reversed** at load time in `loadImpulse`.
- The convolution then reads a contiguous, forward-running span of `activeLength` floats starting at
  `position + maximumLength - activeLength + 1`, with no wraparound test and no branch.
- Accumulate in `float` with **four independent accumulators** to hide multiply-add latency, then
  reduce. Hand-written SSE2 (`_mm_loadu_ps` / `_mm_add_ps` / `_mm_mul_ps`) in the style already
  present in `engine/dsp/src/Simd.cpp`, with a scalar tail.

Memory cost is one extra `maximumLength * channels` float buffer per convolver — at 4096 taps and
2 channels that is 32 KB per convolver, negligible.

Note that `PartitionedConvolver` already exists and is asymptotically better for long impulses, but
`engine/dsp/src/Convolution.cpp:161` refuses any block where `samples != blockSize`, which makes it
unusable with variable-block-size hosts. That trap is why the cabinet uses the direct path. Fixing it
properly needs an input-buffering wrapper that adds latency, so it is deferred to Q5 as a separate
long-IR path rather than blocking this item.

**Expected.** 6–10× on this loop. Given it is ~46% of the traditional engine, that alone is a
~40% cut to the whole engine.

**Files.** `engine/dsp/src/Convolution.cpp`, `engine/dsp/include/nts/dsp/Convolution.h`.

**Tests.** `Tests/DspTests.cpp` convolution fixtures must produce **bit-identical** output — the only
numerical change is `double` to `float` accumulation, so if the fixtures carry an exact hash it will
need a one-time tolerance widening, and that widening must be justified against a reference
`double` implementation kept in the test rather than silently rebaselined.

**Done when.** Benchmarks show the improvement at 384, 1024 and 4096 taps, and `nts_dsp_tests` is
green.

### Q2. Both cabinet convolvers run even when one contributes nothing

**What is wrong.** `engine/amp/src/TraditionalAmp.cpp:753`:

```cpp
first.process(firstPointers.data(), count, processSamples);
second.process(secondPointers.data(), count, processSamples);
```

The two results are combined at `blend`. At `blend == 0` the B result is multiplied by zero; at
`blend == 1` the A result is. **Half of the most expensive operation in the plug-in is computed and
discarded at both extremes of a control**, and `blend == 0` — cabinet A only — is the default and
by far the most common setting.

**Design.** Skip the inactive convolver when blend sits at a rail. The correctness trap is history:
a convolver that has been skipped has a stale delay line, so re-engaging it would fade in a tail
built from silence. Handle it the way the codebase already handles this class of problem:

- Track a per-side `engaged` flag derived from `blend` with a small dead zone (say blend < 0.001 or
  > 0.999) **and** `!crossfadeActive`.
- On a rail-to-active transition, run the newly engaged side for one block with its output ramped in
  over the existing crossfade length, so its history refills before it is audible.
- Never skip while `CrossfadingDirectConvolver::crossfadeRemaining != 0` — a response swap is in
  flight and both instances are needed.

The same reasoning applies to `parameters.bypass`, which is already handled at
`engine/amp/src/TraditionalAmp.cpp:741`.

**Expected.** ~2× on the cabinet for the default preset. Multiplies with Q1.

**Files.** `engine/amp/src/TraditionalAmp.cpp`, `engine/amp/include/nts/amp/TraditionalAmp.h`.

**Tests.** New case in `Tests/AmpTests.cpp`: sweep blend from 0 to 1 and back across a block boundary
and assert no discontinuity above the existing click threshold; assert that output at blend 0 is
identical whether or not the skip is enabled.

**Done when.** The blend sweep is click-free and the benchmark shows the halving at blend 0.

### Q3. Half the saturator's `std::tanh` calls are loop-invariant

**What is wrong.** `engine/amp/src/TraditionalAmp.cpp:498`:

```cpp
selectedOversampler().process(channels, count, samples, [this](float input) noexcept
{
    const auto biased = input + config.bias;
    const auto polarity = biased >= 0.0f ? 1.0f + config.asymmetry : 1.0f - config.asymmetry;
    return std::tanh(biased * polarity) - std::tanh(config.bias * polarity);
});
```

`config.bias` and `config.asymmetry` are fixed for the whole block. The second term therefore takes
exactly **two** distinct values across the entire block — one per polarity branch — yet it is
recomputed for every oversampled sample. This lambda runs at 4× the sample rate, across four stages,
across two channels: roughly 32 wasted transcendental calls per output sample.

**Design, part one — free and exactly output-preserving.** Precompute both offsets in
`ResponsivePreampStage::setConfig` (`engine/amp/src/TraditionalAmp.cpp:437`):

```cpp
biasOffsetPositive = std::tanh(config.bias * (1.0f + config.asymmetry));
biasOffsetNegative = std::tanh(config.bias * (1.0f - config.asymmetry));
```

and select by the same polarity branch already present. This removes one of the two `tanh` calls with
**no change to the output at all**. Land this on its own.

**Design, part two — approximation, gated.** Replace the remaining `std::tanh` with a rational
approximation. A saturator needs roughly 1e-4 relative accuracy, not 1 ULP. The standard form

```
x * (27 + x*x) / (27 + 9*x*x)   clamped to +/-1 outside |x| > 3
```

is ~5 cycles against ~25 for `std::tanh`, with error below 1e-4 over the useful range. This
**changes the output** and therefore must sit behind the quality tier from V1 — `Studio` keeps
`std::tanh`, `Standard` and `Eco` use the approximation. Note the same substitution must be applied
to `biasOffsetPositive`/`biasOffsetNegative` when the approximation is active, or the DC offset
subtraction stops cancelling and the stage develops a small bias.

**Expected.** Part one: ~30% off the saturator, free. Part two: a further ~4×, tier-gated. Together
5–8× on what is ~29% of the engine.

**Files.** `engine/amp/src/TraditionalAmp.cpp`, `engine/amp/include/nts/amp/TraditionalAmp.h`,
`engine/dsp/include/nts/dsp/Nonlinear.h`.

**Tests.** Part one: amp fixtures **bit-identical**, no tolerance change permitted. Part two: a new
test asserting the approximation's maximum absolute error against `std::tanh` over [-8, 8], plus an
ABX-grade null test using the existing tooling in `docs/phase-03-abx-tools.md` to confirm the
difference sits below the listening threshold.

**Done when.** Part one is merged with unchanged hashes; part two is merged behind the tier flag with
its error bound asserted.

**Both parts done.** Part two landed with V1: `PreampStageConfig::approximateSaturation` selects
`dsp::fastTanh`, and the tier sets it for Eco and Standard. The DC-offset trap this entry warned
about is handled — `biasOffsetPositive` and `biasOffsetNegative` are derived through whichever tanh
the stage is going to use, because mixing the two would leave the subtraction failing to cancel and
the stage would develop a standing offset.

The branch is hoisted out of the sample loop into two whole loop bodies rather than tested per
sample: this runs at up to eight times the sample rate for every stage, and a per-sample test on a
value fixed for the block would have eaten the saving before it arrived.

### Q4. `Biquad::process` reloads its state from memory every sample

**What is wrong.** `engine/dsp/src/Filters.cpp:139` loops sample-outer, channel-inner:

```cpp
for (std::size_t sample = 0; sample < samples; ++sample)
{
    advanceCoefficients();
    for (std::size_t channel = 0; channel < count; ++channel)
    {
        if (channels[channel] == nullptr) continue;
        const auto input = static_cast<double>(channels[channel][sample]);
        const auto output = current.b0 * input + z1[channel];
        z1[channel] = current.b1 * input - current.a1 * output + z2[channel];
        z2[channel] = current.b2 * input - current.a2 * output;
        ...
```

`z1[channel]` and `z2[channel]` are array accesses in the innermost loop, so the state never lives in
a register. `advanceCoefficients()` is a call plus a branch per sample. The null check runs per
sample per channel. State is `double` for no benefit at these Q values.

`OnePoleFilter::process` immediately above it at `engine/dsp/src/Filters.cpp:95` already does this
correctly — channel-outer, state hoisted into a local. The biquad simply does not follow suit.

There are roughly 25 biquad instances in the traditional chain: 5 in `PreEq`, 3 in `ToneStack`, 3 per
preamp stage across 4 stages, plus the power amp and cabinet filters.

**Design.** Restructure to match `OnePoleFilter`:

- Channel outer, sample inner. Hoist `z1`/`z2` into locals, write back once at the end.
- Hoist the null check out of the loop.
- Split into two paths: a **fast path** when `remaining == 0` (no coefficient interpolation in
  flight) that hoists the five coefficients into locals, and a **slow path** that keeps the current
  per-sample `advanceCoefficients()`. The fast path is the overwhelmingly common case.
- Change state and arithmetic from `double` to `float`. Verify against the existing filter fixtures;
  if any shelf at very low frequency shows coefficient-quantization trouble, keep `double` for the
  state and `float` for the data path.

**Expected.** 2–3× across all biquad instances, ~11% of the engine.

**Measured — the prediction was wrong.** Implemented as described (fast/slow split, state hoisted,
`double` kept so the result stays bit-identical). In isolation `biquad` measured **0.63 µs against a
0.66 µs baseline: no change.** The reason is that a biquad is *latency-bound on its own feedback
path*, not throughput-bound — each output feeds the next sample's state, so a serial dependency of
four to six cycles sets the floor whatever the operands are. Register allocation cannot beat that,
and at ~8 cycles per sample the original was already close to it. Hoisting `double` to `float` would
not change this either; the dependency chain is the same length.

The restructure was kept regardless, because it is what makes T3 worth anything: the settled path
it adds is the path T3's dirty check finally allows to run. Before T3, `remaining` was reset to 64
on every block and the fast path was dead code. The two only pay off together, and the end-to-end
numbers above are where that shows.

**Files.** `engine/dsp/src/Filters.cpp`, `engine/dsp/include/nts/dsp/Filters.h`.

**Tests.** `Tests/DspTests.cpp` filter fixtures and the frequency-response assertions must hold. The
`double`-to-`float` change is the only numerical difference; if it moves a hash, prove the new value
against a `double` reference in the test before rebaselining.

**Done when.** Fixtures green and the benchmark shows the improvement.

### Q5. Long impulse responses need the partitioned path

**What is wrong.** Q1 makes direct convolution roughly 8× faster, which brings a 4096-tap IR from
~1.2 Gcycles/s to ~150 Mcycles/s — acceptable, but still the largest single cost. Direct convolution
is O(N) per sample; partitioned FFT convolution is O(log N). At 4096 taps the crossover strongly
favours the FFT.

`PartitionedConvolver` is written, tested, and unusable: `engine/dsp/src/Convolution.cpp:161`
refuses any block where `samples != blockSize`, and the comment above it explains why — a short block
would desynchronize the spectrum history permanently.

**Design.** An input-buffering wrapper that decouples host block size from partition size:

- Fixed internal partition size (256 samples is a reasonable default).
- Accumulate host input into a partition-sized staging buffer; run the FFT convolution when full;
  drain output through a matching buffer.
- This adds exactly one partition of latency, which **must** be added to `latencySamples()` and
  therefore flows into the dry-path delay at `PluginProcessorAudio.cpp:352` and the reported host
  latency. That is the cost, and it is why this is a separate path rather than a replacement.
- Select automatically: direct convolution below a tap threshold (measure it in P1 — likely around
  512–1024 taps), partitioned above it. The user-visible consequence is that loading a long IR
  increases reported latency, which should be surfaced in the cabinet page.

Two additional inefficiencies to fix while in this code:

- `engine/dsp/src/Convolution.cpp:176` multiplies **all** `fftSize` bins. The input is real, so bins
  above `fftSize/2` are conjugate-symmetric and redundant — iterate to `fftSize/2 + 1` and
  reconstruct. Free 2×.
- The transform is a full complex FFT of real data. A real-input FFT of half the size is another 2×.
  Check whether `nts::dsp::Fft` already exposes one before writing it.

**Expected.** ~4× over the Q1-improved direct path at 4096 taps, at the cost of 256 samples of
latency.

**Files.** `engine/dsp/src/Convolution.cpp`, `engine/dsp/include/nts/dsp/Convolution.h`,
`engine/amp/src/TraditionalAmp.cpp`, `Source/ui/CabinetPage.cpp`.

**Tests.** Drive the wrapper with a pathological block-size sequence (63, 1, 512, 127, …) and assert
the output matches the direct convolver's, offset by exactly the reported latency. This is the test
that the current fixed-block trap makes impossible, and it is the point of the item.

**Done when.** The variable-block test passes and reported latency is correct in a host.

**Component done and measured; not wired into the cabinet.** Two of the three pieces landed:

- **The conjugate-symmetry fix**, which needed no wrapper and carries no latency. Both operands are
  transforms of real signals, so their product is conjugate-symmetric and only the lower half plus
  DC and Nyquist need computing; the upper half is reflected. `partitioned_convolver` went from
  12.61 µs to 8.65 µs, **1.46×**, and `CrossfadingConvolver` inherits it.
- **`BufferedCrossfadingConvolver`**, the input-buffering wrapper. Tested exactly as this entry
  specifies — driven with 63, 1, 512, 127, 3, 200, 33 and required to match direct convolution
  offset by its reported latency, plus an assertion that the leading latency is silence rather than
  stale data.

What it is worth, stereo at 4096 taps and a 128-sample block: **61.29 µs direct against 14.48 µs
buffered, 4.2×**. That is measured against the *already optimised* direct convolver, so against the
original implementation it is closer to thirty times.

**Wired in, using a cheaper arrangement than any of the three first considered.** The constraint is
real: the cabinet blends two responses, so both sides must share a latency or blending them
comb-filters instead of mixing, and the choice therefore belongs to the *section* rather than a slot.

What made the original three options expensive was assuming the section must keep both
implementations live, or must ask its caller to reload on a switch. It does neither.
`CabinetSection` keeps a **copy of each loaded response** — at most 64 KB for two stereo responses at
the 4096-tap maximum — which lets it reload both sides into the other pair itself when the threshold
is crossed. The FFT path's spectra are allocated on **first use**, on whichever thread is loading, so
a user who never loads a long impulse never carries them; the audio thread only reads a flag
published with release ordering once that allocation is complete.

Threshold 1024 taps, partition 128 samples. Below it, direct convolution at roughly four
microseconds a block and no latency; above it, the partitioned path and 128 samples the host
compensates for. The latency flows through `CabinetSection::latencySamples` into
`AmpVoice::latencySamples`, which is why the amplifier benchmark's latency column reads 160 at
4096 taps: 128 for the cabinet and 32 for the oversampling.

End to end at 4096 taps and 4x oversampling: **138.6 µs before the switch, 95.8 µs after** — and
748.6 µs before any of this work began.

The test drives the transition **in both directions**, because the failure that would matter is a
one-way trip: a user who tries a long impulse and thinks better of it has to get their latency back.

---

## Workstream R — Neural runtime

### R1. WaveNet weights are stored in a layout that cannot vectorize

**What is wrong.** `engine/ml-runtime/src/PackedWaveNetModel.cpp:166`:

```cpp
const float* const kernelRow = kernel + row * channels * layer.kernelSize;
for (std::size_t column2 = 0; column2 < channels; ++column2)
    sum += kernelRow[column2 * layer.kernelSize] * source[column2];
```

The weight access has stride `kernelSize`. At `kernelSize >= 16` that is one useful float per cache
line: the loop is memory-bound on a working set that would otherwise fit comfortably in L1. It cannot
vectorize, because consecutive iterations touch non-adjacent memory.

The on-disk layout is `[row][column][tap]`. The access pattern wants `[tap][row][column]`.

**Design.** Repack at load time, in `PackedWaveNetModel::load`, after validation and before
`primeFromSilence()`. Build a second `std::vector<float>` in tap-major order and process from that;
keep the original only if something else reads it. **No change to the packed file format** — this is
purely an in-memory transformation, so existing artifacts and their SHA validation are untouched.

With contiguous access the inner loop becomes a straight dot product that auto-vectorizes, and can be
hand-written in SSE2 alongside Q1's kernel.

**Also in this loop:** `engine/ml-runtime/src/PackedWaveNetModel.cpp:163` computes
`(layer.position + columns - lag % columns) % columns` — two integer divisions per tap. `lag` is
loop-invariant per tap and `columns` is fixed per layer, so the modulo can be strength-reduced to a
compare-and-subtract, or eliminated entirely by the same doubled-buffer trick as Q1.

**Expected.** 3–5× on the WaveNet trunk.

**Measured — 1.4× at eight channels, and it exposed a threshold the plan did not anticipate.**
The transpose landed as designed and the packed test vectors pass unchanged. But routing the inner
loop through the out-of-line `dsp::dotProduct` made the **three-channel** model 26% *slower* than
the strided scalar loop it replaced: a three-element dot product cannot amortise a cross-library
call plus a four-accumulator horizontal reduction, and the vector main loop never executes at all.

The fix is a length threshold in `Simd.h` — under eight elements the dot product is an inlined
scalar loop, at or above it the vector kernel. Eight was measured, not guessed: a first attempt at
sixteen gave back the entire eight-channel win. With the threshold, three channels returns to
parity and eight channels keeps 1.4×.

The lesson generalises to the rest of workstream R and to U1: **the layout fix and the vector
kernel are separate wins**, and only the second one has a size below which it stops being a win.
`wavenet3` is the "lite tier" the benchmark holds to a budget precisely because it is what a weak
machine runs, so a regression there would have hit exactly the users this plan is for.

**Files.** `engine/ml-runtime/src/PackedWaveNetModel.cpp`,
`engine/ml-runtime/include/nts/ml/PackedWaveNetModel.h`.

**Tests.** `Tests/MlRuntimeTests.cpp` test-vector validation must pass unchanged — the packed
artifacts carry expected outputs and a tolerance, and this item must not move them.

**Done when.** Test vectors pass and `nts_ml_runtime_benchmarks` shows the improvement.

### R2. The LSTM spends most of its time in `std::exp`

**What is wrong.** `engine/ml-runtime/src/PackedTanhModel.cpp:190`:

```cpp
const auto i = sigmoid(gateValues[row]), f = sigmoid(gateValues[h + row]);
const auto g = std::tanh(gateValues[2 * h + row]), o = sigmoid(gateValues[3 * h + row]);
nextCell[row] = f * cell[row] + i * g; nextState[row] = o * std::tanh(nextCell[row]);
```

Three sigmoids and two `tanh` per hidden unit per sample. `sigmoid` at
`engine/ml-runtime/src/PackedTanhModel.cpp:26` calls `std::exp`. At h = 64 that is 320 transcendental
calls per sample — roughly 15 M calls per second at 48 kHz, which on its own can saturate a low-end
core.

**Design.** Vectorized polynomial approximations for both, applied across the hidden vector rather
than per element. Because both gates are bounded and the LSTM's own recurrence is contractive, the
accuracy requirement is far lower than it looks — but this **changes the output**, so:

- Gate it behind the same quality tier as Q3 part two.
- Assert a maximum absolute error bound in tests.
- Re-run the existing C++/Python parity check (`nts_ml_torch_parity`) against the **exact** path, not
  the approximate one, so parity remains a statement about the reference implementation.

**Expected.** 3–5× on the LSTM, which is the dominant neural architecture.

**Measured — 1.11× on the LSTM, and the estimate was wrong for an instructive reason.** The
approximations landed as `dsp::fastTanh` and `dsp::fastSigmoid`, the latter derived from the former
through the identity `sigmoid(x) = (1 + tanh(x/2)) / 2` so there is one approximation and one error
bound. A test asserts that bound at **under 1e-4** against `std::tanh` and the exact logistic over
±12, and a second test drives 4096 samples through both paths and asserts the difference does not
accumulate through the recurrence.

The LSTM went from 153.1 µs to 137.9 µs. The three-to-five-times figure assumed `std::exp` and
`std::tanh` cost around twenty-five cycles each; on this toolchain they are far cheaper than that,
so the activations are roughly a fifth of the LSTM's cost rather than most of it, and making them
four times faster saves about a ninth of the total. The matrix products R3 already vectorised are
what remain.

**On the `tanhRnn` architecture it is a wash or slightly negative** — 33.4 µs exact against 35.7 µs
approximate — because that architecture evaluates a single tanh per hidden unit, so the per-sample
branch on the flag costs about what the approximation saves. This measurement is the reason the
benchmark now carries an `lstm64` fixture at all: `recurrentFixture` builds a v1 architecture-1
model, so every neural benchmark before this one was timing the architecture with the *fewest*
activations, and none exercised the conditioned LSTM that Phase 5 actually recommends.

**Consequence for V1:** the tier must not enable approximate activations globally. It is worth
taking for LSTM and GRU captures and worth declining for `tanhRnn`. Left **opt-in and off by
default**, reachable through `NeuralModel::setApproximateActivations`; no shipping path takes it
yet, and nothing is validated through it — the packed test vectors and the Python parity suites all
run the exact path.

**Files.** `engine/ml-runtime/src/PackedTanhModel.cpp`, `engine/dsp/include/nts/dsp/Nonlinear.h`.

**Tests.** Error-bound assertion; parity test unchanged on the exact path; an audible-difference
check on a captured model.

### R3. The recurrent matvec is unvectorized for LSTM and GRU

**What is wrong.** `engine/ml-runtime/src/PackedTanhModel.cpp:185`:

```cpp
for (std::size_t column = 0; column < h; ++column)
    sum += recurrentWeight[row * h + column] * state[column];
```

Contiguous, and therefore correct in layout — but scalar, with a single accumulator, so it is limited
by multiply-add latency rather than throughput. The `tanhRnn` branch directly above at
`engine/ml-runtime/src/PackedTanhModel.cpp:163` is **already manually four-way unrolled with four
lane accumulators**; `lstm` and `gru` were not given the same treatment. That looks like an
oversight rather than a decision.

**Design.** Extract the four-accumulator dot product into a shared helper in `nts::dsp`, use it from
all three branches, and give it an SSE2 implementation. The GRU branch at
`engine/ml-runtime/src/PackedTanhModel.cpp:208` reads three separate weight rows against the same
`state` vector in one loop — that is good for `state` locality and should be kept, just widened.

**Expected.** 2–3×, and unlike R2 it is exactly output-preserving up to floating-point
reassociation.

**Files.** `engine/ml-runtime/src/PackedTanhModel.cpp`, `engine/dsp/include/nts/dsp/Simd.h`,
`engine/dsp/src/Simd.cpp`.

**Tests.** Test vectors within existing tolerance. Reassociation moves the last bits, so an exact
hash here will need a justified tolerance rather than a rebaseline.

### R4. Stereo runs two model instances on identical input

**What is wrong.** `engine/ml-runtime/src/NeuralAmpProcessor.cpp:135` holds `models[channel]` and
runs inference per channel. A guitar DI is mono. When a host duplicates it to stereo — the common
case — the plug-in performs **two full inferences to produce two bit-identical results**.

**Design.** Detect duplicate input channels per block and run inference once:

- Compare the channel buffers with a cheap early-exit scan. For a genuinely duplicated mono source
  the comparison succeeds on the whole block; for real stereo it exits within a few samples, so the
  check costs approximately nothing in either case.
- On a match, run `models[0]` and copy its output to the other channel.
- **Do not** swap models mid-stream: `models[1]` keeps running only when needed, and when the input
  transitions from duplicated to genuinely stereo its state is stale. Handle it the same way as Q2 —
  keep `models[1]` fed with the (identical) input even when its output is discarded, so its state
  stays warm, and skip only the *output* computation. If that proves to cost as much as it saves,
  fall back to a short crossfade on re-engagement.

**Expected.** ~2× on the neural engine for mono sources, which is most of them.

**Measured — exactly 2× on the inference, 4.1× on the benchmark.** Duplicated-stereo
`recurrent64` went from 133.9 µs to 32.3 µs, which is R3's 2.2× and this item's 2× compounding.
Implemented with the state-staleness problem handled by resetting the idle instance and fading its
output in over 512 samples on the return to genuine stereo, rather than the "keep it warm" approach
this entry sketched — keeping it warm would have meant running the inference, which is the entire
cost being saved. A test asserts the collapsed output is bit-equal to a genuine mono instance.

**Files.** `engine/ml-runtime/src/NeuralAmpProcessor.cpp`.

**Tests.** Assert identical output for duplicated-mono input with and without the optimization;
assert a click-free transition when input becomes genuinely stereo mid-stream.

---

## Workstream S — Shared DSP primitives

### S1. Integer modulo on the per-sample path

**What is wrong.** Non-power-of-two integer division appears in at least six per-sample sites:

| Location | Cost |
|---|---|
| `engine/dsp/src/Oversampling.cpp:103` | 1 per input sample |
| `engine/dsp/src/Oversampling.cpp:131` | 1 per **oversampled** sample |
| `engine/dsp/src/Convolution.cpp:108` | 1 per sample |
| `engine/dsp/src/Effects.cpp:78` | 1 per sample |
| `engine/dsp/src/Effects.cpp:79` | 1 per sample |
| `engine/dsp/src/Effects.cpp:85` | 1 per sample |

Integer division is 20–40 cycles on the older CPUs this plan targets, and it is not pipelined.

**Design.** Two options per site, whichever fits:

- Round the buffer length up to a power of two and mask. Costs memory, saves the division entirely.
  Right choice for the delay line, where `capacity` is already derived from a maximum time and has
  slack.
- Conditional subtract: `if (++position >= size) position = 0;`. Free, and correct wherever the
  increment is exactly one — which is every site above except the fractional delay reads at
  `engine/dsp/src/Effects.cpp:78`, where the subtrahend is bounded by `capacity` so a single
  conditional subtract still suffices.

**Files.** `engine/dsp/src/Oversampling.cpp`, `engine/dsp/src/Convolution.cpp`,
`engine/dsp/src/Effects.cpp`.

**Tests.** Existing fixtures; output must be bit-identical.

**Measured — smaller than expected in isolation.** All six sites converted. The oversamplers show
no isolated change, because the division was never the bottleneck there: the surrounding loop is
still the `double`-accumulating reverse ring walk that S2 exists to fix, and it dominates by enough
to hide the division entirely. The saving is real but it only becomes visible once S2 lands. The
delay line's three divisions per sample were the more worthwhile half of this item.

### S2. The oversampler repeats `DirectConvolver`'s mistakes

**What is wrong.** `engine/dsp/src/Oversampling.cpp:81` and `:108` use the same `double`
accumulation, backwards ring walk and per-tap branch as Q1, in a loop that runs at up to 8× the
sample rate.

**Design.** Apply the Q1 treatment — doubled linear buffer, forward contiguous access, `float`
accumulation, SSE2 kernel with multiple accumulators. The polyphase structure means the upsample
inner loop has a stride of `oversamplingFactor` over the coefficients; deinterleave the coefficients
into per-phase contiguous sub-filters at `designFilter()` time so each phase becomes a dense dot
product.

**Expected.** 4–6× on ~16% of the traditional engine.

**Measured — 1.6× at 4×, 2.1× at 8×.** Below the estimate, and the reason is structural: the
upsampler's per-phase dot products are only `antiAliasTapsPerPhase + 1` = nine elements long
whatever the oversampling factor, because deinterleaving divides the filter across the phases. Nine
elements sit just above the threshold R1 established, so they vectorise barely. The decimator's
single full-length dot product — 33 taps at 4×, 65 at 8× — is where the gain actually comes from,
which is why the higher factors improve more.

**Files.** `engine/dsp/src/Oversampling.cpp`, `engine/dsp/include/nts/dsp/Oversampling.h`.

**Tests.** The existing anti-alias and linear-phase assertions in `Tests/DspTests.cpp`, plus
`hasLinearPhaseCoefficients()` — deinterleaving must not disturb symmetry.

### S3. Every preamp stage allocates four oversamplers

**What is wrong.** `engine/amp/src/TraditionalAmp.cpp:425`:

```cpp
const std::array factors { dsp::OversamplingFactor::x1, dsp::OversamplingFactor::x2,
                           dsp::OversamplingFactor::x4, dsp::OversamplingFactor::x8 };
for (std::size_t index = 0; index < oversamplers.size(); ++index)
    oversamplers[index].prepare(spec, factors[index]);
```

All four are prepared, each with a `work` buffer of `maximumBlockSize * factor * channels`. That is
1+2+4+8 = **15× the block size per stage**, across 4 stages, across 2 voices. At a 2048-sample block
in stereo that is ~2 MB of scratch, most of which is never touched but all of which competes for L2.
`designFilter()` also runs four times per stage with `sin`/`cos` per tap.

The design intent is clear — switching oversampling factor must not allocate on the audio thread —
and that constraint has to be preserved.

**Design.** Keep four `Oversampler` instances (so switching stays allocation-free) but have them
**share one worst-case `work` buffer** sized for 8×, owned by the stage and passed in. Only one
oversampler is ever active, so there is no aliasing. This cuts scratch from 15× to 8× and, more
importantly, means the active factor's working set is the *same* memory each time rather than a
different buffer per factor.

Optionally: skip `prepare` on factors the current quality tier forbids (V1 caps `Eco` at 1×), and
prepare them lazily if the tier changes — tier changes happen on the message thread, so allocation
there is fine.

**Files.** `engine/amp/src/TraditionalAmp.cpp`, `engine/amp/include/nts/amp/TraditionalAmp.h`,
`engine/dsp/include/nts/dsp/Oversampling.h`.

**Tests.** Heap-allocation detector must show zero allocations across an oversampling factor change
during processing — that is the constraint this item must not break.

**Done.** `Oversampler::prepare` takes an optional `sharedWork` buffer, and each preamp stage now
owns one sized for 8x and hands it to all four of its instances. Scratch per stage drops from
fifteen times the block size to eight. Sharing is safe precisely because `selectedOversampler`
returns exactly one instance, so two can never be mid-`process` at once — that constraint is stated
on the parameter, because it is the thing a future caller could get wrong.

The allocation-free requirement is unaffected: the four instances still exist, so changing factor
under audio still allocates nothing. At the default 2048-sample stereo configuration this saves
roughly 900 KB across four stages and two voices, which is memory rather than CPU — no benchmark
moves, and none was expected to.

---

## Workstream T — Always-on per-block cost

Individually small, collectively a measurable fraction of the block, and all of it is work whose
result is often discarded.

### T1. `measureBuffer` runs twice per block through the slow accessor

**What is wrong.** `Source/PluginProcessorInternal.h:206`, called at
`Source/PluginProcessorAudio.cpp:153` and `:420`. It uses `buffer.getSample(channel, sample)` inside
the inner loop — a call through JUCE's channel-pointer indirection per sample — and computes
zero-crossing rate and inter-channel correlation whether or not the assistant page is open.

**Design.** Hoist `getReadPointer` outside the loops. Gate the whole call on an atomic
`assistantActive` flag set by the editor when the assistant page is visible; when inactive, skip
entirely and push nothing to `assistantSummaryQueue`.

**Files.** `Source/PluginProcessorInternal.h`, `Source/PluginProcessorAudio.cpp`,
`Source/ui/ToneAssistantPage.cpp`.

**Done, but gated on editor existence rather than page visibility.** The read pointers are
hoisted, and the whole measurement is skipped when no editor is open.

Page visibility, which this entry proposed, would have been wrong. The editor's timer drains the
assistant and tuner queues *whether or not their page is showing*, deliberately, so a reading is
fresh the moment the page is opened — there is a comment in `timerCallback` saying so. Gating the
producer on visibility would have broken that. Editor existence cannot: with no editor there is no
page to open, and nothing drains the queues anyway, so every push was already being dropped.

The saving applies whenever the plug-in window is closed, which is most of a mixing session and
exactly the case a weak machine cares about. A test renders the same signal with the editor
notionally open and closed and requires the audio to be **bit-identical** — the gate is only
legitimate if it is genuinely display-only.

### T2. Delay and reverb process at zero mix

**What is wrong.** `Source/PluginProcessorAudio.cpp:327` and `:329` run unconditionally. The reverb
is four combs and two all-passes per channel; at `mix == 0` the result is added at zero weight.

**Design.** Skip when mix is zero **and** the tail has decayed. Naively skipping cuts a fading tail,
so track a silence counter: when mix reaches zero, keep processing until the internal state falls
below a threshold for `tailSamples()`, then stop. Resume immediately when mix becomes non-zero. Note
that `getTailLengthSeconds()` at `Source/PluginProcessorAudio.cpp:437` deliberately reports across
all engines and must keep doing so — this item changes when processing runs, not what is reported.

**Files.** `Source/PluginProcessorAudio.cpp`, `engine/dsp/src/Effects.cpp`.

**Tests.** Assert a reverb tail is not truncated when mix is swept to zero mid-decay.

**Done, and it turned up a latent bug.** `Reverb::process` *already* returned early at zero mix —
but it left its comb and all-pass lines frozen, so a send brought back up replayed whatever they
were holding, which for a long reverb is seconds of unrelated audio. `Delay` had no skip at all.

Both now share the engagement pattern from Q2: hysteresis around the mix control, and a clear on
the way back in so the tail builds from silence. The "decay then stop" design this entry sketched
cannot work — the lines are continuously driven by the input, so they never decay while the effect
is running, and the state is only stale in the sense that it is *old*, not quiet. Two tests drive
each effect hard while parked and assert that bringing it back up on silence produces nothing.

`Delay::setParameters` also recomputed a `std::exp` for its damping coefficient on every block,
because the plug-in refreshes effect parameters unconditionally; it is now cached against the
cutoff it was derived from.

### T3. Parameter structs are rebuilt every block

**What is wrong.** `currentAmpParameters()` (`Source/PluginProcessorAudio.cpp:523`) constructs the
entire `AmpParameters` struct every block, including `dbToLinear` calls and a copy of a factory
preset. `currentPedalParameters` is called for all slots every block at
`Source/PluginProcessorAudio.cpp:264`, and `refreshEffectParameters()` at `:326` recomputes delay and
reverb coefficients — including a `std::exp` in `Delay::setParameters` — every block regardless of
whether anything moved.

**Design.** Keep a cached struct and a dirty flag. `parameterOf` reads atomics; hash or compare the
raw parameter values once per block (cheap — 52 atomic loads and a comparison) and rebuild only on
change. The downstream `setParameters` calls already no-op on unchanged values in some cases
(`PedalSlot::retarget` at `engine/pedals/src/PedalBoard.cpp:60` does exactly this) — extend that
pattern upward rather than inventing a new one.

**Files.** `Source/PluginProcessorAudio.cpp`, `Source/PluginProcessorInternal.h`.

**Done for the amplifier; the cascade was deeper than this entry assumed.** `AmpParameters` and its
nested structs gained defaulted `operator==`, and `TraditionalAmpProcessor::setParameters` now
returns early when nothing has moved, guarded by a `parametersDirty` flag for the paths that
reconfigure a voice without going through it (preset load, immediate application, and the end of a
mode transition, which leaves the voice that stopped sounding holding stale values).

What this entry missed is that the cost was never mainly the struct rebuild. Reconfiguring called
`BiquadCoefficients::make` around fifty times per block across the two voices — each a `sin`, a
`cos` and a `pow` — *and*, because every `setCoefficients` call restarts a 64-sample interpolation,
it pinned every filter in the amplifier to its per-sample interpolating path. Q4's settled path
could never execute. That coupling is why Q4 measured nothing on its own and why the pair is worth
roughly a quarter of the 384-tap end-to-end improvement.

**Still outstanding under this item:** the pedal-slot and effect parameter rebuilds in
`Source/PluginProcessorAudio.cpp:264` and `:326`, which have the same shape and have not been
touched.

### T4. Tuner decimation runs when the tuner is closed

**What is wrong.** `Source/PluginProcessorAudio.cpp:179` loops every sample, every channel, through
`inputSnapshot.getSample()`, accumulating into the tuner queue, whether or not the tuner page exists.

**Design.** Gate on an atomic set by the editor, the same mechanism as T1. Hoist the read pointers
while there.

**Files.** `Source/PluginProcessorAudio.cpp`, `Source/ui/TunerPage.cpp`.

**Done**, on the same editor-existence gate as T1, with the read pointers hoisted and the two
per-sample divisions turned into precomputed reciprocals.

This is the one change in the plan so far that **altered externally visible behaviour**, and the
existing tests caught it: eight assertions in `nts_wrapper_tests` drive `processBlock` and then
tick `updateTuner()` without ever constructing an editor. They now declare the shell open, which
is what they were already simulating by ticking the timer at all.

---

## Workstream U — Build configuration and dispatch

### U1. No architecture flags are set; runtime dispatch is the right answer

**What is wrong.** `CMakeLists.txt` sets no `/arch:` or `/fp:` flags. MSVC on x64 defaults to SSE2
with `/fp:precise`, so none of the DSP loops get FMA or 8-wide operations. `engine/dsp/src/Simd.cpp`
contains a single `multiplyGain` helper that the real DSP never calls — the scaffolding exists but is
unused.

Setting `/arch:AVX2` globally would give roughly 2× on the vectorizable loops and **hard-crash with
an illegal instruction on any pre-2013 CPU**, which is precisely the hardware this plan targets. It
is the wrong answer here.

**Design.** Runtime dispatch:

- Baseline binary stays SSE2 — everything must work with no dispatch at all.
- Put the genuine kernels in dedicated translation units: convolution dot product (Q1), polyphase
  filter (S2), recurrent matvec (R3), WaveNet trunk (R1).
- Compile a second copy of those units with `/arch:AVX2` via CMake `set_source_files_properties`.
- Detect once at startup with `__cpuid` / `__cpuidex` (leaf 7 for AVX2, and check `XGETBV` for OS
  support — the common bug is testing the CPU bit without testing whether the OS saves YMM state).
- Select through function pointers initialized once, never per block.

Also evaluate `/fp:fast` **scoped to the DSP and ML libraries only** — never to
`engine/state` or the serialization code, where reproducibility matters. Its main benefit here is
permitting reassociation in reduction loops, which is exactly what these kernels are.

**Files.** `CMakeLists.txt`, `engine/dsp/src/Simd.cpp`, `engine/dsp/include/nts/dsp/Simd.h`, plus new
`*_avx2.cpp` units.

**Tests.** A test that forces the SSE2 path and the AVX2 path and asserts their outputs agree within
tolerance. CI must exercise both — if the runner supports AVX2, the SSE2 path still needs explicit
coverage via a forced-dispatch override.

**Done when.** Both paths produce matching output, and the binary starts on a CPU without AVX2
(verify under an emulator or by masking the feature bit).

**Done, and the measurement objection turned out to be solvable.** `SimdAvx2.cpp` holds the wide
kernel and nothing else -- one function, no statics, no initialisers -- compiled with `/arch:AVX2`
through `set_source_files_properties` while the rest of the engine stays at SSE2. Dispatch resolves
once before `main` through a function pointer.

The detection asks all three questions this entry called for. CPUID leaf 1 for AVX and OSXSAVE,
**XGETBV for whether the OS actually preserves YMM state across a context switch**, and only then
leaf 7 for AVX2. The middle one is the one that is usually missed and the only one that catches a
capable CPU under an operating system that cannot use it -- such an OS corrupts the upper halves
silently rather than faulting.

**Measured at 1.56× on the kernel, consistently.** The concern recorded earlier -- that this machine
could not resolve the benefit against a ±10% noise floor -- was wrong, and the fix was a technique
already in use elsewhere in this document: force each path in turn and time them back to back **in
one process**, so the run-to-run spread cancels out of the ratio exactly as it does for the other
gates. Scalar against the dispatched kernel is now 9.8×, up from 6.3× when SSE2 was the ceiling.

End to end the effect is small and inside the noise -- the amplifier moved from 95.8 µs to 93.9 µs
at 4096 taps, the recurrent model from 32.0 µs to 27.3 µs. That is the expected shape rather than a
disappointment: after Q1 through S3 the dot product is no longer most of a block, so making it half
again faster cannot move the total much. The win is real, bounded, and matters most on the machines
with the least headroom.

**The test is the part worth keeping.** `setSimdPath` forces either kernel, and the suite drives
both across every length from zero to forty plus a long case, requiring them to agree with each
other and with a scalar reference. Without it the SSE2 path -- the one that has to keep working on
the hardware this plan exists to serve -- would never execute again on any developer machine new
enough to take the AVX2 one. Agreement is to 1e-4 rather than bit-exact, because the AVX2 kernel
fuses its multiply and add and so carries *more* precision than the SSE2 one, not less.

### U2. Confirm LTO and check the whole-program picture

**What is wrong.** `juce_recommended_lto_flags` is applied to the plug-in and standalone targets
(`CMakeLists.txt:332`, `:415`) but **not** to the engine libraries at `CMakeLists.txt:197`. Cross-TU
inlining of small DSP helpers — `suppressDenormal`, `dbToLinear`, `clamp01` — depends on it.

**Design.** Verify whether the engine libraries are static and therefore already covered by the
consumer's LTO. If not, enable it there. Measure; LTO occasionally hurts, and this is a
one-benchmark-run question.

**Files.** `CMakeLists.txt`.

**Tried, measured, and rejected.** Enabling `juce_recommended_lto_flags` on the engine libraries
built and passed all 23 suites, but the benchmark result was inside run-to-run noise: across
rebuilds one amplifier configuration measured 9% faster and another 7% slower. The effect, if any,
is smaller than this machine's between-run variance, and the flag propagates PUBLIC to all
twenty-three test executables, so it costs build time everywhere.

Reverted, with the finding recorded in `CMakeLists.txt` next to the loop so the next person does
not repeat the experiment blind. Two things a future attempt needs: the
`if(NOT TUBEFORGE_ENABLE_SANITIZERS)` guard, because MSVC's ASan cannot be combined with `/GL` and
the sanitizer build depends on these libraries not carrying whole-program flags; and a quieter
machine.

The specific cost that motivated this item — a real call to `dsp::dotProduct` for operands too
short to amortise it — was solved directly by the length threshold in R1 instead.

---

## Workstream V — Quality tiers

Micro-optimization alone will not make a 4096-tap dual-IR stereo chain at 8× oversampling run on a
dual-core 2.0 GHz machine. It needs the option to do less work.

### V1. An explicit performance tier

**Design.** A user-facing three-position control, persisted in project state and settings:

| | Eco | Standard | Studio |
|---|---|---|---|
| Oversampling cap | 1× | 2× (auto) | up to 8× |
| Cabinet IR length | truncated to 256 taps | 1024 | full 4096 |
| Dual-IR blend | A only, blend control disabled | both | both |
| Saturator | polynomial (Q3 part two) | polynomial | `std::tanh` |
| LSTM activations | approximate (R2) | approximate | exact |
| Neural stereo | always mono-collapse | collapse when detected (R4) | collapse when detected |
| Assistant metering | off | on | on |

Truncating a cabinet IR to 256 taps is close to inaudible on a guitar cabinet — the tail past a few
milliseconds is mostly room, and the plug-in is not a reverb — and it is a **16× cut in the most
expensive operation in the plug-in**. That single row is worth more than most of workstreams Q and S
combined on the worst-case configuration.

The truncation must use a short fade-out over the last ~32 taps rather than a hard cut, or the
discontinuity produces audible high-frequency splatter.

**Files.** `Source/PluginProcessor.h`, `Source/PluginProcessorAudio.cpp`,
`engine/state/src/ProjectState.cpp`, `engine/state/src/SettingsStore.cpp`,
`engine/amp/src/TraditionalAmp.cpp`, `Source/ui/` (a control, plus disabled states).

**Tests.** State round-trip for the new field including v0/v1 migration; assert each tier's caps are
actually enforced; assert a tier change mid-playback is click-free.

**Done when.** All three tiers are selectable, persisted, and their CPU cost is recorded in the P1
benchmark matrix.

**Done apart from the UI control.** A `performanceTier` choice parameter, appended last so every
existing index stays valid for saved projects and host automation, defaulting to Standard. The
limits live in one `constexpr` table on the processor (`limitsFor`), and six places read it: the
oversampling ceiling and the single-cabinet rule in `currentAmpParameters`, impulse truncation in
`applyCabinetIr`, forced mono collapse and approximate activations on the neural processor, and the
assistant metering gate in the callback. Persistence comes free with the parameter; no state
migration was needed.

Measured with `nts_plugin_benchmarks`, same request at each tier — 8x oversampling, both cabinets,
effects engaged, editor open — as a percentage of the callback budget for one instance:

| Block | Studio | Standard | Eco | Studio → Eco |
|---|---|---|---|---|
| 64 | 5.87% | 3.03% | 2.05% | **2.9×** |
| 128 | 5.35% | 2.90% | 2.16% | **2.5×** |
| 256 | 5.79% | 3.06% | 2.02% | **2.9×** |
| 512 | 5.79% | 2.99% | 1.95% | **3.0×** |

Consistent across every block size, which puts it comfortably clear of the ±10% noise floor P1
established — unlike T1, T2 and T4.

**The headline "up to 16×" is not what this measures**, and the difference is worth stating: that
figure came from truncating a 4096-tap user impulse to 256, and the benchmark runs the built-in
384-tap responses because it loads no user IR. The 2.5–3.0× above is what the oversampling cap, the
single-cabinet rule and the approximations deliver on their own. The truncation lever is
implemented and covered by the tier test, but its benefit is unmeasured.

**Three deviations from this entry's design:**

- **The tier caps the oversampling factor rather than the control.** Clamping the parameter would
  have rewritten the user's choice into their saved project; capping the derived factor means
  raising the tier restores what they asked for. A test asserts the control survives a round trip
  through Eco.
- **Approximate activations are not applied globally.** R2 measured them slower than exact on
  `tanhRnn`, so `PackedTanhModel::approximatesActivations` declines them for that architecture
  regardless of what the tier asks. The tier expresses intent; the model decides whether the intent
  is worth honouring.
- **Impulse truncation fades over the last 32 taps** rather than cutting, as this entry required —
  a step in an impulse is broadband splatter.

**UI done as well.** A tier selector sits in the shell rail beside the engine selector, and the
per-tier control overrides are pushed down to the pages through a new
`ModulePage::setPerformanceLimits` hook rather than the shell reaching into them: the oversampling
menu greys the entries the tier would override and captions itself "Oversampling (capped)", and the
cabinet blend control is disabled with its caption saying why.

Pushed rather than pulled, and limits rather than values: a page has no business polling a rig-wide
setting, and the tier caps what the engine *derives* while never rewriting what is stored — so a
control is greyed, never moved, and raising the tier brings it back exactly where the user left it.

The test builds the editor at each tier. That is the check that matters for the selector, because a
`ComboBoxAttachment` against a parameter id that does not exist asserts rather than failing quietly.

### V2. Auto-select the tier from measured load

**Design.** `DiagnosticsCollector` already tracks callback time, deadline, CPU load and dropouts. Use
them: if CPU load exceeds a threshold for a sustained window, or dropouts occur, step the tier down
and inform the user rather than glitching. Never step **up** automatically — a user who chose Studio
did so deliberately, and silently changing their tone is worse than a dropout.

The hysteresis pattern is already in the codebase: `automaticOversamplingFactor` at
`Source/PluginProcessorAudio.cpp:495` uses a margin to stop a control resting on a boundary from
oscillating. Reuse that shape.

**Files.** `Source/PluginProcessorAudio.cpp`, `engine/diagnostics/src/DiagnosticsCollector.cpp`,
`Source/PluginEditor.cpp`.

**Done.** The audio thread samples `diagnostics.snapshot()` every 32 blocks — it reads only atomics,
so it is safe there, but it is not free enough to run every block and the condition it looks for is
a sustained one anyway. A dropout counts immediately, because a dropout is not a warning about the
failure, it *is* the failure; sustained load above 75% has to persist for eight checks, because one
busy callback happens for reasons that have nothing to do with this plug-in.

The request crosses to the message thread through an atomic and `triggerAsyncUpdate`, which is the
route the MIDI program change already takes and which works whether or not an editor exists — the
case that matters most, since a machine in trouble is most likely one running with its window shut.
The reduction is logged at warning severity so there is a record of the plug-in having changed the
user's setting.

**One step at a time, downward only, and the user's choice wins.** The handler re-reads the current
tier rather than trusting the request, so a control moved between the audio thread asking and the
message thread acting is not overwritten. Recovery is deliberately never automatic: silently
restoring a tone when a transient load passes is worse than the dropout it avoided.

The test pins the **no-op** case — four hundred blocks at Studio on a machine that is coping, and
the tier must still be Studio. A load-watching heuristic's failure mode is firing when it should
not, and that is the half that can be tested without manufacturing an overload.

---

## Recommended sequence

Ordered by value per unit of risk, not by workstream.

| # | Item | Effort | Expected | Output-preserving | State |
|---|---|---|---|---|---|
| 1 | P1 baseline | S | — | — | ✅ done, amp + plug-in harnesses |
| 2 | Q3 part one — invariant `tanh` | XS | ~30% of saturator | ✅ exact | ✅ done |
| 3 | Q2 — blend-rail cabinet skip | S | ~2× cabinet, default preset | ✅ exact | ✅ done |
| 4 | Q1 — SIMD direct convolver | M | 6–10× cabinet | ✅ to float rounding | ✅ done, 6.7× |
| 5 | Q4 — biquad restructure | M | 2–3× all biquads | ✅ to float rounding | ✅ done, no isolated gain |
| 6 | S1 — modulo removal | S | small, broad | ✅ exact | ✅ done |
| 7 | T1–T4 — always-on cost | M | small, broad | ✅ exact | ✅ T1/T2/T4 done; T3 amp only |
| 8 | S2 — SIMD oversampler | M | 4–6× oversampling | ✅ to float rounding | ✅ done, 1.6–2.1× |
| 9 | R4 — neural mono collapse | S | ~2× neural, mono sources | ✅ exact | ✅ done, 2.0× |
| 10 | R1 — WaveNet repack | M | 3–5× WaveNet | ✅ exact | ✅ done, 1.4× at 8ch |
| 11 | R3 — recurrent matvec | S | 2–3× LSTM/GRU | ✅ to reassociation | ✅ done, 2.2× |
| 12 | V1 — quality tiers | L | up to 16× worst case | ❌ by design | ✅ done, 2.5–3.0×, with UI |
| 13 | Q3 part two, R2 — approximations | M | 4× saturator, 3–5× LSTM | ❌ gated by V1 | ✅ both done |
| 14 | U1 — runtime AVX2 dispatch | L | ~2× on kernels | ✅ to reassociation | ✅ done, 1.56× on the kernel |
| 15 | Q5 — partitioned long-IR path | L | 4× at 4096 taps | ❌ adds latency | ✅ done, 1.45× end to end |
| 16 | P2, U2, S3, V2 | M | hygiene | ✅ | ✅ S3, P2, V2 done; U2 rejected |

Items 2 through 11 are all output-preserving, so the existing regression fixtures and hashes are the
safety net for the entire first half of this plan. That is the reason for the ordering: the
approximations and the tier work, which genuinely change the sound, only begin once the free wins are
banked and measured.

A reasonable first milestone is items 1–5, which should cut the traditional engine by roughly 60%
without changing a single audible thing.
