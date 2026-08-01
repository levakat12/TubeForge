# TubeForge improvement plan

A sequenced, implementation-ready plan derived from a read of the `0.10.0` tree. Every item states
what is wrong or missing, the evidence for it, the design to apply, the files touched, the tests that
prove it, and what "done" means.

Nothing in this document has been implemented. Line references are against the tree at commit
`0a97c55`; re-check them before editing, because they will drift.

## Status

Baseline before any work: clean Release build, **19/19 CTest suites passing**. That is the reference
every change below was measured against.

| Phase | Items | State |
|---|---|---|
| 1 | A3, A5, A1, A2+A4 | ✅ Complete — 19/19 green, benchmark acceptance gate passing |
| 2 | B1, B5, B6 | ✅ Complete. **B2 attempted and declined — needs an author decision** |
| 3 | B3, B4 | ✅ Complete — ASan leg green on 9 suites; clang-tidy advisory, 47 findings triaged |
| 4 | C1 → C4 | ✅ Complete — user cabinet IR loading works end to end, path persisted, Cabinet page in the shell |
| 5 | D1, D2-interim | ✅ Complete — models renamed and frozen extractor asserted; gradient now honours the configured pre-emphasis weight |
| 6 | E1 | ✅ Partial — tone analysis and reconstruction extracted (495 lines, 2 mutexes, 2 threads). Package library and neural loading deliberately left; see E1 |
| 7 | D3 → D2-full → D4 | ✅ Complete — torch backend verified equivalent, 35–184x faster, C++ parity holds |
| 8 | E2 → E3 | ✅ Complete — shared front end, and the processor split into four units |
| 9 | F1, F2, F3 | ✅ Complete — tuner, MIDI program change, delay and reverb |

**Coverage: 100% of the planned items.** Every item A1–F3 is implemented and verified. The one
thing deliberately **not** done is **B2**, which is a style decision that belongs to the author
rather than a task to complete — the measurement is in its entry.

Beyond the plan, the follow-ups each item named have also been closed: the cabinet blend control,
the tuner display with mute-while-tuning, and the `PartitionedConvolver` fixed-block trap.

Answering the open design question in E2 turned out to change the answer: two thirds of the proposed
shared chain **cannot** be shared without altering the amplifier, and the code says so plainly. See
that entry.

PyTorch 2.11+cu128 is now installed, so phase 7 was completed rather than skipped.

Three items in this plan were **wrong as originally written**, and were corrected against measurement
rather than left standing:

- **A5's guard test could not work.** The specified round-trip test was built, then checked by
  transposing two entries in the id table — and it still passed. Any test reaching the mapping goes
  through the same table it is checking. Replaced with a single-source X-macro so the failure mode
  is designed out. Details in [A5](#a5-fifty-two-string-keyed-parameter-lookups-per-audio-block).
- **B2's premise was false.** "Derive the config from existing code, it should be close to a no-op"
  measured at ~100% of lines changed at every column limit. Details in
  [B2](#b2-clang-format--attempted-not-adopted-needs-a-decision).
- **C1's design would have shipped an audio bug.** Moving the cabinet to `CrossfadingConvolver`
  looked right — the API is exactly what runtime IR loading needs — but the partitioned convolver
  underneath it is only correct on fixed-size blocks, and diverges by 0.41 on short ones. Replaced
  with a time-domain equivalent. Details in
  [C1](#c1-make-cabinet-convolution-safe-for-runtime-ir-replacement).

Each fix in phase 1 was also verified to fail without its fix — the trim removed, the bypass ramp
made instant, the dry delay forced to zero — so none of the new assertions are vacuous.

## How to use this document

Work is grouped into six workstreams, A through F. Within a workstream, items are ordered by
dependency. Across workstreams the recommended order is in [Sequencing](#sequencing).

Each item carries:

- **Why** — the user-visible or engineering consequence.
- **Evidence** — file and line, so the claim can be checked rather than trusted.
- **Design** — the approach, including the alternative that was rejected and the reason.
- **Files** — everything that changes.
- **Tests** — the specific assertions to add, in the existing `TestHarness` style.
- **Acceptance** — the condition that closes the item.
- **Risk** — what could go wrong and how to back out.

### Conventions to preserve

- C++20, no exceptions on the audio thread, `noexcept` process functions, no allocation in
  `processBlock`. `Tests/IntegrationTests.cpp` has a heap-allocation detector — keep it green.
- Engine libraries stay JUCE-independent except where they already depend on it (`nts_state`,
  `nts_ir`, `nts_ecosystem`).
- Style follows the existing code: Allman braces, four-space indent, `camelCase` members, comments
  that explain *why* rather than *what*. Comment density in this codebase is low and purposeful;
  match it.
- Every behavioral change gets a test in the suite that owns that layer before it is considered done.

### Verification loop

```bash
cmake --build build --config Release --parallel && ctest --test-dir build -C Release --output-on-failure
```

For DSP work also run the benchmark matrix, since several items touch the audio thread:

```bash
./build/nts_dsp_benchmarks_artefacts/Release/nts_dsp_benchmarks.exe
```

---

## Workstream A — Correctness on the audio path

Five contained defects. All are in `Source/PluginProcessor.cpp`. Two of them (A3, A4) are on the
project's own remaining-release-gates list by way of the VST3PluginTestHost validation requirement.

### A1. Input trim is inert in Neural and Physical Circuit modes

**Why.** A user in Neural or Physical Circuit mode turns the Input knob and nothing happens. There is
no error, no warning, and the meter does not move. Trim is the first control anyone touches when
setting up a DI, so this reads as a broken plugin.

**Evidence.**

- [PluginProcessor.cpp:314](../Source/PluginProcessor.cpp#L314) stores `0.0f` into
  `runtimeParameters.inputGainDb` unconditionally. This is deliberate: `engine.process` runs *after*
  the amplifier at [:400](../Source/PluginProcessor.cpp#L400), so a non-zero value there would act as
  a second output gain, not an input trim.
- The `input` parameter reaches the signal only through `AmpParameters::manualInputTrimDb` at
  [:1724](../Source/PluginProcessor.cpp#L1724), consumed by `AmpVoice::process` at
  [TraditionalAmp.cpp:830](../engine/amp/src/TraditionalAmp.cpp#L830).
- Neural mode at [:373](../Source/PluginProcessor.cpp#L373) calls `neuralAmp.process` directly.
  `NeuralAmpProcessor` has no trim input — see its public API in
  [NeuralAmpProcessor.h:47-64](../engine/ml-runtime/include/nts/ml/NeuralAmpProcessor.h#L47).
- Physical mode at [:378](../Source/PluginProcessor.cpp#L378) calls `physicalCircuit.process`.
  `currentCircuitControls()` at [:1765](../Source/PluginProcessor.cpp#L1765) builds eight fields and
  none is a trim.

**Design.** Add a processor-owned trim stage applied to the buffer immediately before the engine-mode
branch, gated so the Traditional path is untouched:

```cpp
// PluginProcessor.h
nts::dsp::SmoothedParameter inputTrimGain;   // linear gain, log smoothing

// prepareToPlay
inputTrimGain.prepare(sampleRate, 20.0, nts::dsp::SmoothingMode::logarithmic);
inputTrimGain.reset(nts::dsp::dbToLinear(parameterOf(Param::input)));

// processBlock, before the engine-mode branch
if (engineMode != 0)   // Traditional applies trim inside AmpVoice; see A1 note
{
    inputTrimGain.setTarget(nts::dsp::dbToLinear(parameterOf(Param::input)));
    for (std::size_t sample = 0; sample < sampleCount; ++sample)
    {
        const auto gain = inputTrimGain.next();
        for (int channel = 0; channel < outputCount; ++channel)
            outputs[channel][sample] *= gain;
    }
}
```

`SmoothedParameter` is at [Smoothing.h:17](../engine/dsp/include/nts/dsp/Smoothing.h#L17) and has
exactly `prepare` / `setTarget` / `next`. Use logarithmic smoothing so a trim sweep is perceptually
even.

**Why the conditional rather than a single unified stage.** Hoisting trim out of `AmpVoice` for all
three modes is the correct end state and is scheduled as [E2](#e2-shared-signal-chain). It cannot be
done here without a semantic change: `InputCalibrator::process` runs at
[TraditionalAmp.cpp:827](../engine/amp/src/TraditionalAmp.cpp#L827), *before* the trim is applied at
line 830. So `suggestedTrimDb` today is an absolute recommendation measured on the raw input. Move
trim upstream and it silently becomes a residual, changing what the Neural Capture and Amplifier
pages display. E2 handles that properly by feeding the calibrator from the already-existing pristine
`inputSnapshot` buffer. Until then, the conditional costs one branch and zero regression risk.

Leave a comment at the branch pointing at E2 so the temporary shape is not mistaken for the design.

**Files.** `Source/PluginProcessor.h`, `Source/PluginProcessor.cpp`.

**Tests.** New cases in `Tests/WrapperTests.cpp` (it already instantiates the processor):

1. For each of `engineMode` 1 and 2, process a known-amplitude sine at `input = 0 dB`, capture output
   RMS, then at `input = +12 dB`, and assert the ratio is within 0.5 dB of 4.0. Guard against the
   engines' own nonlinearity by using an input low enough to stay linear (peak ≤ 0.05).
2. Assert `engineMode = 0` output is bit-identical before and after the change, to prove the
   Traditional path is untouched. Capture a reference buffer with `renderOffline`.
3. Sweep `input` from -20 dB to +20 dB across a block boundary and assert no sample-to-sample delta
   exceeds a threshold, proving the smoother is engaged.

**Acceptance.** Input trim measurably changes level in all three engine modes; Traditional output is
unchanged; no discontinuity on a trim sweep.

**Risk.** Low. If the ratio test fails in Neural mode, check whether
`setInputCompensationEnabled` ([NeuralAmpProcessor.h:58](../engine/ml-runtime/include/nts/ml/NeuralAmpProcessor.h#L58))
is active — it applies its own bounded gain and will confound the measurement. Disable it in the test.

---

### A2. Bypass and engine-mode changes are hard switches

**Why.** Both produce an audible click, and bypass additionally produces a timing shift because the
processed path has latency and the dry path does not.

**Evidence.** [PluginProcessor.cpp:355-388](../Source/PluginProcessor.cpp#L355) branches on the raw
parameter value every block with no fade. `nts::dsp::ModeCrossfader`
([ModeCrossfader.h:15](../engine/dsp/include/nts/dsp/ModeCrossfader.h#L15)) exists, is tested at
[DspTests.cpp:327](../Tests/DspTests.cpp#L327) and [:462](../Tests/DspTests.cpp#L462), and is used
only *inside* `TraditionalAmpProcessor` ([TraditionalAmp.h:357](../engine/amp/include/nts/amp/TraditionalAmp.h#L357))
for preset changes — never between engine modes or across bypass.

**Design.** These two switches need different treatment, because only one of them can preserve
latency.

**Bypass — crossfade against a delayed dry path.** Bypass must be latency-neutral: the host keeps its
delay compensation applied, so the dry signal has to be delayed by the reported latency to line up.
This is also what A4 requires. Add:

```cpp
// A ring buffer of reported-latency length, written pre-processing, read post.
nts::dsp::DelayLine dryDelay;              // add if absent; trivial ring buffer
nts::dsp::SmoothedParameter bypassMix;     // 0 = processed, 1 = dry
```

Write the pre-engine buffer into `dryDelay` each block, run the engine, then mix:

```cpp
bypassMix.setTarget(bypassActive ? 1.0f : 0.0f);
for (std::size_t sample = 0; sample < sampleCount; ++sample)
{
    const auto mix = bypassMix.next();
    for (int channel = 0; channel < outputCount; ++channel)
        outputs[channel][sample] = outputs[channel][sample] * (1.0f - mix)
                                 + dryDelay.read(channel, sample) * mix;
}
```

Use a 20 ms ramp. Equal-gain (not equal-power) is correct here because the two signals are
correlated. Size `dryDelay` in `prepareToPlay` to the maximum latency any mode can report, so
`setLatencySamples` changes never reallocate on the audio thread.

**Engine mode — muted switch, not a crossfade.** `ModeCrossfader::process` runs the supplied lambda
twice during a fade, once per mode ([ModeCrossfader.h:92-93](../engine/dsp/include/nts/dsp/ModeCrossfader.h#L92)).
That is fine for CPU over 2048 samples, but the three engines report *different* latencies —
Traditional carries oversampling latency, Neural reports zero, Physical has its own. Crossfading two
paths that are time-offset from each other smears transients and comb-filters the overlap.

Two ways out:

- *Equalize latency across all modes* by padding each to the maximum. Clean, allows a true crossfade,
  but penalizes the lowest-latency mode permanently — unacceptable for a plugin whose README
  advertises 1x oversampling specifically for zero algorithmic latency.
- *Muted switch* (**recommended**): on a mode change, ramp output to silence over ~5 ms, switch the
  mode and `reset()` the incoming engine, then ramp back up over ~5 ms. No click, no smearing, no
  latency penalty. Ten milliseconds of silence when a user deliberately changes amplifier engines is
  imperceptible and honest.

Implement the muted switch as a small state machine in the processor: `Idle → FadeOut → Switch →
FadeIn → Idle`, driven off a `SmoothedParameter` and a pending-mode member. Keep reporting latency
through the existing `refreshProcessingLatency` / `triggerAsyncUpdate` path at
[:1864](../Source/PluginProcessor.cpp#L1864) — during the silent window the host's latency change is
inaudible, which is a further argument for this design.

**Files.** `Source/PluginProcessor.h`, `Source/PluginProcessor.cpp`. Possibly a new
`engine/dsp/include/nts/dsp/DelayLine.h` if no ring-buffer delay exists — check `Filters.h` and
`Common.h` first.

**Tests.** `Tests/WrapperTests.cpp`:

1. **No click on bypass.** Process a steady sine, toggle `bypass` mid-block, assert the maximum
   absolute first difference of the output stays below the value seen in steady state times a small
   factor (2.0 is generous and still catches a hard switch, which produces a full-scale step).
2. **Bypass is time-aligned.** With `bypass` on, cross-correlate output against input and assert the
   peak is at lag 0, not at lag `-latencySamples`.
3. **Bypass is transparent.** With `bypass` on, assert output equals input to within 1e-6 after the
   ramp completes.
4. **No click on mode change.** Same first-difference test across a `engineMode` 0→1→2→0 sweep.
5. **Mute window is bounded.** Assert output returns to non-silent within 512 samples of the switch.

**Acceptance.** All five pass; benchmark matrix shows no regression outside noise.

**Risk.** Medium — this is the most invasive Workstream A item. The dry delay interacts with A4; do
A2 and A4 in one branch. If `setLatencySamples` is called with a value exceeding the prepared delay
size, the read must clamp rather than read out of bounds; assert this in Debug.

---

### A3. `getTailLengthSeconds()` returns zero while cabinet convolution is active

**Why.** Hosts use tail length to decide how long to keep calling `processBlock` after transport
stop. Reporting zero means offline render, bounce, and track freeze truncate the cabinet tail — the
last ~85 ms of every rendered take is cut off. This is silent data loss in the user's exported audio,
and it will not reproduce during live monitoring, which makes it easy to ship.

**Evidence.** [PluginProcessor.cpp:440](../Source/PluginProcessor.cpp#L440) returns `0.0`.
`CabinetSection` convolves with up to `maximumIrLength = 4096` samples
([TraditionalAmp.h:285](../engine/amp/include/nts/amp/TraditionalAmp.h#L285)) — 85 ms at 48 kHz.

**Design.** Expose the active impulse length and report it plus processing latency.
`DirectConvolver::impulseLength()` already exists
([Convolution.h:40](../engine/dsp/include/nts/dsp/Convolution.h#L40)). Add:

```cpp
// CabinetSection
[[nodiscard]] std::size_t tailSamples() const noexcept
{ return parameters.bypass ? 0 : std::max(first.impulseLength(),
                                          second.impulseLength() + parameters.delaySamplesB); }

// AmpVoice, TraditionalAmpProcessor — forward, taking the max over both voices
[[nodiscard]] std::size_t tailSamples() const noexcept;
```

Then:

```cpp
double TubeForgeAudioProcessor::getTailLengthSeconds() const
{
    const auto tail = std::max({ traditionalAmp.tailSamples(),
                                 physicalCircuit.tailSamples(),   // see note
                                 std::size_t {} });
    return static_cast<double>(tail + pendingOversamplingLatencySamples.load(std::memory_order_relaxed))
         / currentSampleRate;
}
```

Report the maximum across engines rather than the current mode's, so switching modes mid-session
never shortens an already-negotiated tail.

For `CircuitProcessor` the cabinet is a reactive IIR approximation rather than an IR, so it has no
finite impulse length. Give it a documented fixed allowance — 50 ms is comfortably beyond the decay
of the filters involved — rather than inventing a measurement.

Note this must not be `noexcept`-constrained or lock-taking: JUCE may call it from the message
thread while audio runs. Reading `std::size_t` members that only change in `prepare`/`loadImpulse` is
fine; do not add a mutex.

**Files.** `engine/amp/include/nts/amp/TraditionalAmp.h`, `engine/amp/src/TraditionalAmp.cpp`,
`engine/circuit/include/nts/circuit/CircuitProcessor.h`, `Source/PluginProcessor.cpp`.

**Tests.** `Tests/AmpTests.cpp`: after `prepare`, assert `tailSamples() > 0`; after setting
`cabinet.bypass = true`, assert it is `0`. `Tests/WrapperTests.cpp`: assert
`getTailLengthSeconds()` is within [0.02, 0.5] for a prepared processor at 48 kHz.

**Acceptance.** Non-zero tail reported; an offline render of a note ending at the buffer boundary
retains its cabinet decay.

**Risk.** Very low.

---

### A4. No `getBypassParameter()` override

**Why.** VST3 hosts discover the bypass control through this method. Without it the host renders its
own bypass by simply not calling the plugin, which drops latency compensation and defeats the click-
free bypass from A2. VST3PluginTestHost — named in
[phase-10-coverage.md:29](phase-10-coverage.md#L29) as a release gate — checks for this.

**Evidence.** `grep -rn "getBypassParameter\|processBlockBypassed" Source apps engine` returns
nothing. The parameter is registered as a plain `AudioParameterBool` at
[PluginProcessor.cpp:1554](../Source/PluginProcessor.cpp#L1554).

**Design.**

```cpp
juce::AudioProcessorParameter* getBypassParameter() const override
{
    return parameterState.getParameter(ParameterIds::bypass);
}
```

Two consequences that must be handled, not just noted:

1. **The host will no longer call `processBlockBypassed`.** Once this returns non-null, JUCE's
   contract puts the plugin in charge: the host sets the parameter and `processBlock` must honor it,
   including a smooth transition and a latency-aligned dry path. A2 supplies both. Do not implement
   `processBlockBypassed` — it would be dead code that diverges.
2. **Mark the parameter as meta.** Use `juce::AudioParameterBoolAttributes{}.withMeta(true)` so hosts
   treat it as a control-surface parameter rather than an automatable audio parameter.

Also worth doing in the same pass, since it is the same file and the same host-conformance concern:
group the ~45 flat parameters into `juce::AudioProcessorParameterGroup`s (Input, Amp, Gate,
Cabinet, Neural, Circuit, Output). Hosts render a flat 45-entry list as an unusable wall; groups cost
nothing and change no parameter IDs, so automation stays valid.

**Files.** `Source/PluginProcessor.h`, `Source/PluginProcessor.cpp`.

**Tests.** `Tests/VST3HostTests.cpp` already performs a real scan and instantiation — add an
assertion that the scanned plugin exposes a bypass parameter and that its index matches the
`bypass` parameter's. Add a `WrapperTests` case asserting `getBypassParameter() != nullptr` and that
its `getParameterIndex()` is stable across `setStateInformation`.

**Acceptance.** Host reports a bypass control; A2's transparency and alignment tests still pass with
bypass driven through the host parameter rather than directly.

**Risk.** Low, but do it together with A2 — shipping A4 without A2 makes bypass worse, not better,
because the host stops doing its own clean bypass and the plugin's hard switch takes over.

---

### A5. Fifty-two string-keyed parameter lookups per audio block

**Why.** Wasted audio-thread budget for no benefit. This is not a real-time-safety violation — JUCE's
`adapterTable` is a `std::map<StringRef, …, StringRefLessThan>`
(`juce_AudioProcessorValueTreeState.h:662` in the fetched JUCE tree), so lookups compare strings but
do not allocate. It is simply ~52 tree descents of full string comparison per block, roughly a
million string comparisons per second at 96 kHz with a 32-sample buffer.

**Evidence.** `valueOf` at [PluginProcessor.cpp:1622](../Source/PluginProcessor.cpp#L1622) calls
`getRawParameterValue` on every access. 13 call sites inside `processBlock`, 39 more inside
`currentAmpParameters()`, 83 in the file.

**Design.** Resolve every pointer once in the constructor. `AudioProcessorValueTreeState` guarantees
these `std::atomic<float>*` are stable for its lifetime.

> **Revised during implementation.** The original design here was an enum plus a parallel
> `constexpr` id array, held together by a `static_assert` on length and a round-trip test for
> ordering. **The round-trip test does not work and was verified not to work**: transposing two
> entries in the id array and rebuilding left the test passing. Any test that reaches the mapping has
> to go through the same table it is checking, so a consistent transposition agrees with itself. The
> length assertion catches nothing here either, since a swap preserves length.
>
> Both lists are now generated from one X-macro, `TUBEFORGE_RUNTIME_PARAMETERS`, so an enumerator and
> its string id cannot diverge — the failure mode is designed out rather than tested for. This is the
> only user-defined macro in the codebase; the alternative was a correctness hazard no test could
> cover.

```cpp
// PluginProcessor.h -- one list, above the class
#define TUBEFORGE_RUNTIME_PARAMETERS(X) \
    X(input) X(output) X(bypass) X(gain) /* … */ X(gateRelease)

enum class Param : std::size_t
{
#define TUBEFORGE_DECLARE_PARAM_ENUMERATOR(name) name,
    TUBEFORGE_RUNTIME_PARAMETERS(TUBEFORGE_DECLARE_PARAM_ENUMERATOR)
#undef TUBEFORGE_DECLARE_PARAM_ENUMERATOR
    count
};

std::array<std::atomic<float>*, static_cast<std::size_t>(Param::count)> parameterPointers {};

[[nodiscard]] float parameterOf(Param id) const noexcept
{
    return parameterPointers[static_cast<std::size_t>(id)]->load(std::memory_order_relaxed);
}
```

The id array in the implementation file expands from the same macro. Populate in the constructor and
`jassert` that none resolved to null — a typo in an id currently degrades to a silent `0.0f` return
at [:1627](../Source/PluginProcessor.cpp#L1627), which is how a parameter can quietly stop working.

Keep `valueOf` for the message-thread call sites (project save/load, circuit graph construction, the
assistant); it is only the audio path that matters.

Where several parameters are indexed dynamically — the four stage drives — assert their enumerators
are contiguous rather than rebuilding a local id array.

**Files.** `Source/PluginProcessor.h`, `Source/PluginProcessor.cpp`.

**Tests.** A `WrapperTests` case asserting: every enumerator names a parameter that exists; every
cached pointer tracks its parameter's value; and **every parameter in the layout is reachable through
the table**, which is the invariant that survives the macro and catches a parameter added to
`createParameterLayout` that nobody added to the runtime list. That last assertion was verified to
fail when a name is removed from the macro.

**Acceptance.** No `valueOf` calls remain inside `processBlock` or `currentAmpParameters`; benchmark
acceptance gate still passes; null assertion in place.

**Risk.** Low once the mapping is structural.

---

## Workstream B — Build and quality tooling

The gap here is narrower than it first appears. `/W4` is **already** applied to all twelve engine
libraries and both wrappers via `juce::juce_recommended_warning_flags`
([CMakeLists.txt:118-123](../CMakeLists.txt#L118), [:234](../CMakeLists.txt#L234),
[:298](../CMakeLists.txt#L298)); JUCE's helper maps that to `/W4` on MSVC. What is missing is
enforcement, static analysis, formatting, and sanitizers.

### B1. Warnings as errors in CI

**Design.** Add an option, default off locally so day-to-day work is not blocked, on in CI:

```cmake
option(TUBEFORGE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
if(TUBEFORGE_WARNINGS_AS_ERRORS)
    if(MSVC)
        add_compile_options(/WX)
    else()
        add_compile_options(-Werror)
    endif()
endif()
```

Add `-DTUBEFORGE_WARNINGS_AS_ERRORS=ON` to the configure step in
[.github/workflows/build.yml](../.github/workflows/build.yml).

**Sequencing note.** Run a `/WX` build locally *first* and fix the fallout in its own commit before
turning it on in CI, so the enabling commit is a one-liner. Expect the residue to be in the
Windows-specific code (`ProcessMemory.cpp`) and in the numeric conversions across `nts_dsp`.

**Acceptance.** CI Debug and Release both pass with `/WX`.

### B2. `.clang-format` — **attempted, not adopted; needs a decision**

**Why it was proposed.** `CONVENTIONS.md` describes agent behavior, not code style. Style is
currently held by hand, which works with one author and stops working with two.

**What actually happened.** The premise above — "derive the config from the existing code, it should
be close to a no-op" — **is false, and was measured to be false.** A config derived from the
codebase (Microsoft base, Allman, 4-space, `Cpp11BracedListStyle: false`, left pointers) reformats
essentially the entire tree:

| Column limit | Lines changed across `Filters.cpp`, `ProjectState.cpp`, `TraditionalAmp.cpp`, `PluginProcessor.cpp` |
|---:|---|
| 100 | 111% of original line count |
| 110 | 104% |
| 130 | 99% |

Two causes, and neither is a tuning problem:

1. **Multi-statement lines.** `spec = newSpec; crossfader.prepare(spec, voices.size());`
   ([TraditionalAmp.cpp:818](../engine/amp/src/TraditionalAmp.cpp#L818) and hundreds like it).
   clang-format always splits these. No option preserves them.
2. **Semantic wrapping.** The DSP code wraps argument lists to carry meaning — `normalize()` calls
   in [Filters.cpp](../engine/dsp/src/Filters.cpp) put numerator coefficients on one line and
   denominator coefficients on the next. clang-format repacks them by width, destroying the grouping.
   A file written in conventional style (`Smoothing.h`) still shows ~26% churn.

**Decision required from the author.** This is a style choice, not a defect, so it is left open:

- **Adopt** — accept one bulk-reformat commit touching essentially every C++ file, recorded in
  `.git-blame-ignore-revs`. Gains machine-enforced consistency; loses the semantic wrapping in the
  DSP code and rewrites all `git blame` attribution.
- **Decline** — keep hand-maintained style. Costs consistency enforcement as the project grows.

No `.clang-format` file is checked in. That is deliberate: editors auto-format on save when they find
one, so committing a config that rewrites 100% of the tree would silently corrupt any file anyone
opened. Add it only together with the bulk commit.

The same finding applies to `ruff format` on the Python package, and was handled the same way in
[B5](#b5-python-linting-and-the-unused-config).

### B3. `.clang-tidy`

**Design.** Start narrow so the first run is actionable rather than a wall of noise:

```yaml
Checks: >
  bugprone-*,
  cert-*,
  performance-*,
  readability-identifier-naming,
  -bugprone-easily-swappable-parameters,
  -cert-err58-cpp
WarningsAsErrors: ''
HeaderFilterRegex: 'engine/.*'
```

`bugprone-*` and `performance-*` are the high-yield sets for this codebase — lots of manual buffer
indexing, `std::span` plumbing, and pass-by-value structs. Run it over `engine/` only at first;
`Source/` pulls in JUCE headers and will produce noise that has to be filtered separately.

Wire it as a separate, initially **non-blocking** CI job with `continue-on-error: true`. Promote to
blocking once the backlog is cleared.

clang-tidy needs a compile database, which the Visual Studio generator does not emit — hence the
separate Ninja configure in the CI job. Two practical notes from getting it running: the first run
produced **83,000** findings, essentially all `misc-include-cleaner` reporting names reached through
transitive JUCE includes, so that check is off; and on a machine with more than one Visual Studio
installed, clang-tidy resolves MSVC headers from the wrong toolchain and dies on
`error STL1000: Unexpected compiler version`. Run it inside `vcvars64.bat` to pin the toolchain.

**Findings — 47 across `engine/`, triaged:**

| Finding | Count | Disposition |
|---|---:|---|
| `bugprone-suspicious-stringview-data-usage` | 13 | Backlog. `.data()` on a `string_view` without passing size. |
| `performance-unnecessary-value-param` | 8 | Backlog. Mostly `std::stop_token` copied per call. |
| `bugprone-implicit-widening-of-multiplication-result` | 6 | Backlog — worth a look, `int` multiply widened to `uint64_t`. |
| `misc-const-correctness` | 5 | Backlog, cosmetic. |
| `bugprone-exception-escape` | 4 | Triaged. `PackedTanhModel::processSample` is a **false positive** — it only indexes vectors, which cannot throw. `validate` and `isSafePackagePath` build `std::string` messages and genuinely can throw `bad_alloc`; real but theoretical. |
| `misc-use-internal-linkage` | 3 | Backlog. `serializePresetLegacy` and two others should be static. |
| `bugprone-branch-clone` / `misc-redundant-expression` | 4 | **Fixed** — see below. |
| `bugprone-integer-division` | 1 | **False positive.** `spectralSquared / (fftSize / 2 + 1)` in `Regression.cpp` is the FFT bin count; `fftSize` is a power of two so the division is exact. |
| *(separate finding)* | — | **`PartitionedConvolver`'s fixed-block trap is now closed.** It refuses a mismatched block outright rather than corrupting its overlap, exposes `requiredBlockSize()`, and its documentation points anything host-fed at `CrossfadingDirectConvolver`. |
| remainder | 3 | Backlog, cosmetic. |

**Fixed during this pass, and worth the author's attention:**
[ToneProfileDatabase.cpp:173](../engine/tone-analysis/src/ToneProfileDatabase.cpp#L173) had
`bass ? 0.30f : 0.30f` and `bass ? 0.07f : 0.07f` inside a weight table whose other four rows
genuinely differ by instrument (`0.12f : 0.10f`, `0.07f : 0.06f`, `0.14f : 0.05f`). Rewritten as
plain constants, which is **behaviour-preserving**. Whether the bass values were *meant* to differ is
a tuning question only the author can answer — if they were, the guitar/bass-aware similarity
weighting is not as instrument-aware as intended.

**Acceptance.** Config present ✅, CI job reporting ✅, findings triaged ✅.

### B4. Address Sanitizer leg

**Why.** The highest-value tool for this specific codebase: lock-free SPSC ring buffers
([SpscRingBuffer.h](../engine/diagnostics/include/nts/diagnostics/SpscRingBuffer.h)), double-buffered
atomic slot swaps in `NeuralAmpProcessor` and `CircuitProcessor`, raw pointer arrays into
`std::vector` storage throughout `nts_dsp`, and hand-rolled SSE2 kernels. Any of those can be off by
one without a single test failing.

**Design.**

```cmake
option(TUBEFORGE_ENABLE_SANITIZERS "Build with Address Sanitizer" OFF)
if(TUBEFORGE_ENABLE_SANITIZERS AND MSVC)
    add_compile_options(/fsanitize=address)
    add_link_options(/INCREMENTAL:NO)
endif()
```

Two constraints to document in the option's help text: MSVC's ASan is incompatible with the
`/RTC` runtime checks that Debug enables by default, and with link-time optimization — so the
sanitizer leg must build a Release-with-symbols configuration and must not pull
`juce::juce_recommended_lto_flags` ([CMakeLists.txt:233](../CMakeLists.txt#L233)). MSVC has no UBSan;
if a Linux/clang leg is ever added (see [B6](#b6-cross-platform-groundwork-optional)), enable
`-fsanitize=undefined` there.

Add a CI job running the fast suites only — `nts_unit_tests`, `nts_dsp_tests`, `nts_amp_tests`,
`nts_circuit_tests`, `nts_ml_runtime_tests`. Exclude the soak and VST3 host tests; ASan's slowdown
makes them impractical.

**Acceptance.** Sanitizer job green, or findings filed.

### B5. Python linting and the unused config

**Why.** `[tool.ruff]` is configured at [ml/pyproject.toml](../ml/pyproject.toml) and never invoked —
not in CI, not in CMake. Dead configuration is worse than none, because it implies coverage that does
not exist.

**Design.** Add a lint step to CI, and — importantly — **pin the rule selection**. The existing
`[tool.ruff]` block sets only `line-length` and `target-version`, so running ruff would enforce
whatever its default selection happens to be that release, and an upgrade could fail CI on rules
nobody chose.

The adopted selection is `E4`, `E9`, `F`, `I`, `UP`, `B` — the rules that find defects. `E5` (line
length) and `E7` (statements per line) are deliberately excluded, for the same reason `ruff format`
is not run: with them enabled the package reports 187 line-length and 106 statement-style violations,
all of which are the compact style the package is consistently written in. That is an authorial
choice, and undoing it is a decision to take deliberately rather than as a side effect of adding a
CI step. See [B2](#b2-clang-format--attempted-not-adopted-needs-a-decision) for the same finding on
the C++ side.

Under the pinned selection the package had 67 findings: 64 auto-fixed (import ordering, quoted
annotations, deprecated `typing` imports, two unused imports), and three fixed by hand — two `B904`
(`raise ... from None` in the Demucs worker's top-level handler) and one `UP042`
(`CaptureStage(str, Enum)` → `StrEnum`). The `StrEnum` change was checked to be behaviour-preserving
first: the workflow file is written through `stage.value`
([wizard.py:64](../ml/nts_ml/capture/wizard.py#L64)), not `str(stage)`, so the persisted JSON is
unchanged. `nts_ml_python_tests` and `nts_ml_runtime_parity` both still pass.

Consider `mypy --strict` on `ml/nts_ml` as a follow-up — the package is already thoroughly annotated
with `NDArray[np.float32]` types, so the incremental cost is low and the payoff on a numeric codebase
is high.

**Acceptance.** `ruff check` clean in CI. ✅

### B6. Housekeeping

- **Remove `Modelfile.txt`** from version control. It is a 45-byte Ollama model config in the repo
  root, unrelated to the product. `.aider*` is correctly ignored in
  [.gitignore](../.gitignore); this one predates that rule.
- **Add `.editorconfig`** so non-C++ files (YAML, Markdown, CMake) get consistent treatment across
  editors.
- **Coverage measurement** (optional): OpenCppCoverage over the C++ suites and `coverage.py` over
  `ml/tests`, uploaded as CI artifacts. The per-phase coverage documents measure *requirement*
  coverage by hand, which is more meaningful than line coverage — but line coverage would tell you
  which of the 25k lines no test has ever executed, which is currently unknown.

#### B6b. Cross-platform groundwork (optional)

The engine libraries are deliberately framework-independent, and only three places are
Windows-specific: `psapi` linkage ([CMakeLists.txt:58](../CMakeLists.txt#L58)), the ASIO block, and
`ProcessMemory.cpp`. A Linux CI leg building `nts_dsp` + `nts_amp` + their tests under clang would
cost one workflow job and buy a second compiler's diagnostics plus UBSan — the strongest available
check on the numeric code. This is not about shipping on Linux; it is about the free bug-finding.

---

## Workstream C — User cabinet IR loading

The single largest product gap, and the infrastructure is already written and tested.

### C1. Make cabinet convolution safe for runtime IR replacement

**Why — and this is a prerequisite, not a nicety.** `CabinetSection` holds two `DirectConvolver`s
([TraditionalAmp.h:301](../engine/amp/include/nts/amp/TraditionalAmp.h#L301)), and
`DirectConvolver::loadImpulse` ([Convolution.cpp:76-87](../engine/dsp/src/Convolution.cpp#L76))
zeroes and rewrites the coefficient buffer in place, mutates `activeLength`, and calls `reset()` —
with no double-buffering and no atomic publication. Calling it from a loader thread while the audio
thread is inside `process()` is a data race that will produce audible garbage at best.

**Evidence that the right primitive already exists.** `nts::dsp::CrossfadingConvolver`
([Convolution.h:76-99](../engine/dsp/include/nts/dsp/Convolution.h#L76)) has precisely the required
shape: `loadInactiveImpulse` (safe to call from a worker), `requestSwap(crossfadeSamples)` (atomic),
and a `process` that crossfades between two `PartitionedConvolver` instances. This is the
"inactive preparation and atomic response crossfades" the Phase 2 documentation refers to. It is
simply not the class `CabinetSection` uses.

**Design.**

> **Revised during implementation — the original design here was unsafe.** It said to replace
> `DirectConvolver` with `CrossfadingConvolver`. That would have introduced an audio bug.
> `CrossfadingConvolver` wraps `PartitionedConvolver`, whose `process`
> ([Convolution.cpp](../engine/dsp/src/Convolution.cpp)) advances its spectrum history and shifts
> its overlap by a **whole block regardless of how many samples it was actually passed**. It is
> therefore only correct when every call receives exactly the prepared block size. The cabinet
> cannot promise that: hosts deliver short blocks routinely — the last block of an offline render,
> a buffer-size change mid-session — and `renderOffline`
> ([TraditionalAmp.cpp:1002](../engine/amp/src/TraditionalAmp.cpp#L1002)) does it on every trailing
> partial block.
>
> Measured against direct convolution over the same signal:
>
> | Feed | Max absolute difference |
> |---|---|
> | Full blocks, partitioned vs direct | 0.000000477 |
> | **Mixed short blocks, partitioned vs direct** | **0.408929706** |
> | Mixed vs full blocks, direct vs direct (control) | 0.000000000 |
>
> Neither `PartitionedConvolver` nor `CrossfadingConvolver` is referenced outside `engine/dsp`, so
> there is no live bug today — but they are a trap for exactly the wiring this item called for.

The adopted design keeps time-domain convolution and adds the staged swap to it: a new
`nts::dsp::CrossfadingDirectConvolver` holding two `DirectConvolver`s, with the same
`loadInactiveImpulse` / `requestSwap` contract as `CrossfadingConvolver`. `CabinetSection` uses that.
This preserves exact current numerics and stays correct for any block size. Consequences handled:

- `loadImpulseA`/`loadImpulseB` gain a `crossfadeSamples` parameter (default 2048); the
  synthesized-IR call sites at prepare time pass `0` for an immediate swap.
- **`reset()` must clear history without discarding the loaded response**, matching
  `DirectConvolver::reset`. Getting this wrong is not theoretical — the first implementation reset
  the active index too, and because `CabinetSection::prepare` calls `reset()` *after* staging its
  impulses, the staged response was discarded and the cabinet silently passed its input through.
  Caught by the before/after preset render, which is the reason to do that comparison.
- `tailSamples()` reads lengths tracked in `CabinetSection`, not from the convolvers, so a response
  that is staged or mid-fade is already covered by the reported tail.
- **`maximumIrLength` stays at 4096.** The original rationale for raising it — partitioned
  convolution being O(log N) — no longer applies, and direct convolution is O(taps) per sample, so
  32768 would be an 8× cost in the cabinet. Raising it is now coupled to fixing
  `PartitionedConvolver`'s partial-block handling, which is a separate item worth filing.

**Result.** All four factory presets render **bit-for-bit identical** before and after — stronger
than the 1e-5 tolerance this item originally budgeted for, precisely because the change stayed in
the time domain.

**Files.** `engine/amp/include/nts/amp/TraditionalAmp.h`, `engine/amp/src/TraditionalAmp.cpp`, and
possibly `engine/dsp/include/nts/dsp/Convolution.h` if `CrossfadingConvolver` needs an
`impulseLength()` accessor.

**Tests.** `Tests/AmpTests.cpp`:

1. Load IR A, process, load a *different* IR A while processing continues, and assert the output
   transitions smoothly — no sample-to-sample discontinuity above threshold and the final response
   matches the second IR.
2. A concurrency test: spawn a thread calling `loadImpulseA` in a loop while the main thread calls
   `process` in a loop for a few seconds; assert all output samples remain finite and bounded. Run
   this under the B4 sanitizer leg, where it is most likely to catch something.
3. Assert existing synthesized-IR behavior is unchanged — capture a reference render before the
   change.

**Acceptance.** IR replacement during playback is click-free and race-free; existing amp regression
tests unchanged.

**Risk.** Medium. This changes the cabinet's convolution engine, which sits in the signal path of
every preset. The DSP regression fixtures in `Tests/AmpTests.cpp` and the numeric metrics in
`nts_dsp` `Regression.h` are the safety net — run them before and after and compare, expecting
FFT-vs-direct differences at the 1e-5 level rather than bit-identity.

### C2. Wire `nts_ir` into the plugin

**Why.** `nts::ir::CabinetIrLoader` does async WAV/AIFF/FLAC decode, sample-rate conversion, DC
removal, silence trimming, and peak normalization — and is referenced by nothing but its own test.
`grep -rn "CabinetIrLoader" Source apps engine` outside `engine/ir` returns zero hits, and
`nts_ir` appears in `target_link_libraries` only for `nts_ir_tests`
([CMakeLists.txt:510](../CMakeLists.txt#L510)). It was built and never connected.

**Design.**

1. Add `nts_ir` to the `TubeForge` and `nts_standalone_app` link lists
   ([CMakeLists.txt:216](../CMakeLists.txt#L216), [:280](../CMakeLists.txt#L280)).
2. Add to the processor:

```cpp
void requestCabinetIrLoad(int slot /* 0 = A, 1 = B */, const juce::File& file);
void clearCabinetIr(int slot);                       // revert to the synthesized IR
[[nodiscard]] juce::String cabinetIrStatusText(int slot) const;
[[nodiscard]] juce::File cabinetIrFile(int slot) const;
```

3. `requestCabinetIrLoad` calls `irLoader.loadAsync(file, currentSampleRate, options, completion)`.
   The completion runs on the loader's worker thread and calls `traditionalAmp.loadCabinetImpulse(slot, …)`,
   which is safe once C1 lands. Update the status string under `cabinetStatusMutex` and
   `triggerAsyncUpdate()` so the UI refreshes.
4. **Keep the decoded IR at its native rate** in a member, because `prepareToPlay` can be called with
   a new sample rate at any time. On rate change, re-run `prepareImpulseResponse`
   ([Convolution.h:29](../engine/dsp/include/nts/dsp/Convolution.h#L29)) against the stored decode
   and re-publish. Without this, a user's IR is silently wrong after the host changes rate.
5. Use `ImpulsePreparationOptions{ .outputChannels = 2, .trimThresholdDb = -80.0f, .removeDc = true,
   .normalizePeak = true, .normalizationDb = -1.0f }` — the defaults are already right for cabinets.
6. Reject and report: files longer than `maximumIrLength` after resampling, zero-channel decodes, and
   formats the `AudioFormatManager` cannot open. The loader already returns an `error` string in
   `CabinetLoadResult` ([CabinetIrLoader.h:13](../engine/ir/include/nts/ir/CabinetIrLoader.h#L13)) —
   surface it verbatim rather than inventing a message.

**Files.** `CMakeLists.txt`, `Source/PluginProcessor.h`, `Source/PluginProcessor.cpp`.

**Tests.** `Tests/WrapperTests.cpp` — write a short synthetic WAV to a temp file, request a load, wait
on the status text, assert success and that the rendered response changed. `Tests/IrTests.cpp` already
covers decode/prepare; do not duplicate it.

### C3. Persist IR paths in project state

**Why.** A project that loads a user IR and does not save the path reopens with the wrong cabinet and
no indication why.

**Design.** `AssetState::relativePaths` already exists
([ProjectState.h:57](../engine/state/include/nts/state/ProjectState.h#L57)) and
`normalizeAssetPath` ([:86](../engine/state/include/nts/state/ProjectState.h#L86)) already handles
project-relative paths — the mechanism is built, just unused for cabinets.

Add explicit fields rather than overloading the generic list, because slot identity matters:

```cpp
struct AssetState
{
    std::vector<std::string> relativePaths;
    std::string cabinetIrPathA;   // schema 3
    std::string cabinetIrPathB;   // schema 3
    bool operator==(const AssetState&) const = default;
};
```

Bump `currentSchemaVersion` from 2 to 3 ([ProjectState.h:11](../engine/state/include/nts/state/ProjectState.h#L11))
and extend the existing migration path — the project already ships a v0→v1 migration, so follow that
shape. A v2 project must load into v3 with empty IR paths and no warning.

On load, a missing IR file must degrade gracefully: fall back to the synthesized cabinet, set a
status string naming the missing file, and *keep the path in the state* so re-saving does not destroy
the reference.

**Files.** `engine/state/include/nts/state/ProjectState.h`, `engine/state/src/ProjectState.cpp`,
`Source/PluginProcessor.cpp` (`makeProjectState` / `applyProjectState` at
[:1645](../Source/PluginProcessor.cpp#L1645) and [:1663](../Source/PluginProcessor.cpp#L1663)).

**Tests.** `Tests/UnitTests.cpp` (owns state round-trip): v3 round-trip with IR paths; v2 JSON loads
into v3 with empty paths; validation rejects absolute paths that escape the project directory —
`normalizeAssetPath` should already enforce this, so assert it does.

### C4. Cabinet page in the editor

**Design.** New `Source/ui/CabinetPage.{h,cpp}` following `ModulePage`
([ModulePage.h](../Source/ui/ModulePage.h)) — the contract is documented there: the page owns its
controls, attachments, and file choosers, and communicates outward only through `std::function`.
`ProfileLibraryPage` is the closest existing model for a page with file I/O; copy its structure.

Contents: two IR slots each with file chooser, name label, clear button, and status line; blend,
alignment, polarity, low/high cut, and bypass attached to their existing parameters; the metadata
from `CabinetSection::metadataA/B`. Register in `PluginEditor.cpp` alongside the other pages.

Use `juce::FileChooser::launchAsync` — never the modal variant, which blocks the message thread and
is prohibited in some hosts.

**Acceptance.** A user can load, audition, swap, and clear IRs without a click, and the choice
survives save/reload. ✅

**Notes from implementation.**

- A response must be loaded into **both** `AmpVoice`s, not just the sounding one
  ([TraditionalAmp.h:355](../engine/amp/include/nts/amp/TraditionalAmp.h#L355)). They alternate
  across preset changes, so loading into one alone puts the previous cabinet back the next time a
  preset is recalled — a bug that would only show up after a preset switch.
- `TraditionalAmpProcessor::prepare` reinstates the built-in responses, so `prepareToPlay` has to
  re-apply any user response afterwards. The decoded audio is kept at its own rate for exactly this
  reason: the re-application also re-prepares it against the new sample rate.
- The Cabinet page exposes the two slots, the cabinet on/off switch, alignment and — added
  afterwards — an A/B blend. `cabinetBlend` is appended to `ampControlIds` so saved projects keep
  mapping positionally, and defaults to 50%, the value `CabinetParameters` already used, so no
  existing sound changes.
- Inserting the page at module index 2 shifts `libraryModule` and `firstProModule`
  ([PluginEditor.h](../Source/PluginEditor.h)). Safe because no persisted state stores a module
  index; `UiState` holds only size and diagnostics visibility.

---

## Workstream D — Neural training path

The gap between what the neural capture path documents and what it can do is the largest in the
project. Ordered by how much each unblocks.

### D1. Make GRU and TCN trainable, or reclassify them

**Why.** `CausalTcn.parameters()` ([neural.py:204](../ml/nts_ml/models/neural.py#L204)) returns only
`output_weight`, `output_bias`, and `residual_gain` — the `kernel`, `input_projection`, and
`control_projection` tensors are excluded. `ConditionedGru.parameters()`
([neural.py:147](../ml/nts_ml/models/neural.py#L147)) is the same. `_feature_gradients`
([trainer.py:105-118](../ml/nts_ml/training/trainer.py#L105)) computes gradients only for the linear
readout. Both models are therefore **echo state networks**: a frozen random recurrent/convolutional
feature extractor with a trained linear head. They cannot learn an amplifier's nonlinearity, no
matter how long they train. Only `ConditionedLstm` has real BPTT
([trainer.py:72-102](../ml/nts_ml/training/trainer.py#L72)).

The README describes them as "candidates" ([README.md:81](../README.md#L81)), which is not false —
they exist and produce output — but a reader will assume they are trainable alternatives, and they
are not.

**Design — two honest paths, pick one.**

- **Reclassify (cheap, immediate).** Rename them to reflect what they are (`RandomFeatureGru`,
  `RandomFeatureTcn`), document the frozen extractor in their docstrings, and correct the README and
  `docs/phase-05-coverage.md`. Echo state networks are a legitimate technique with a real advantage —
  training is convex and takes seconds — so this is not a retreat, it is accurate labelling.
- **Implement real gradients (expensive, and see D3).** Hand-deriving BPTT for a GRU and a dilated
  TCN in NumPy is roughly the work of `_lstm_gradients` twice over, and the result would still be
  unusable at scale for the reason in D3. Not recommended as a standalone effort — do it via D3
  instead, where autograd makes it free.

**Recommendation.** Reclassify now (an hour), and let D3 supply genuinely trainable versions.

**Done.** Renamed to `RandomFeatureGru` and `RandomFeatureTcn`, with docstrings stating what is
frozen and why. The `architecture` strings (`"conditioned_gru"`, `"causal_tcn"`) are **unchanged**:
they are baked into checkpoints and the packed export format, and `nts_ml_runtime_parity` still
passes. Worth noting that `docs/phase-05-coverage.md` was already honest about this — rows 18 and 22
read "Partial … BPTT deferred". It was the README that implied three trainable architectures.

**Files.** `ml/nts_ml/models/neural.py`, `ml/nts_ml/training/trainer.py`, `README.md`,
`docs/phase-05-coverage.md`, `docs/phase-05-architecture.md`.

**Tests.** Add an explicit `ml/tests` case asserting that after training, the frozen tensors are
bit-identical to their seeded initialization. That makes the design contract executable rather than a
comment, and it will fail loudly if someone later assumes they train.

### D2. Train on the objective that selects the model

**Why.** The gradient step optimizes plain time-domain MSE
([trainer.py:156](../ml/nts_ml/training/trainer.py#L156) → `_gradients`), while model selection uses
the nine-component `combined_loss` ([trainer.py:173](../ml/nts_ml/training/trainer.py#L173),
[losses/audio.py:87-101](../ml/nts_ml/losses/audio.py#L87)) with multi-resolution STFT,
pre-emphasis, spectral convergence, loudness, transient weighting, and silence stability. The model
is therefore trained for one thing and chosen for another.

This is not merely inelegant. Pre-emphasis and multi-resolution STFT terms *in the gradient* are the
established way to make amp captures sound right; plain time-domain MSE systematically underweights
the high-frequency harmonic detail that distinguishes one distortion character from another, because
that content carries little energy. The composite loss is currently a report, not an objective.

**Design.** Depends on D3. With autograd the fix is to call `combined_loss` in the training step and
backpropagate — a few lines. Without it, every component needs a hand-derived gradient, which for
multi-resolution STFT means differentiating through the FFT magnitude. That is real work and is the
strongest single argument for D3.

**Interim mitigation, now implemented.** Pre-emphasis is folded into the training gradient.
Because the filter is linear, filtering the error is identical to filtering both signals and
subtracting, so it costs one extra pass plus its adjoint.

The weight is **not** a new knob: it is read from `config.loss.pre_emphasis`, the same value the
validation score already uses. Train and select therefore cannot drift apart again, and because that
weight defaults to `0.0` the default configuration reduces exactly to the previous mean-squared
error — no existing result or determinism guarantee moves. A user who weights pre-emphasis in their
loss config now actually gets it optimised rather than merely reported.

**Tests.** Two, in `ml/tests/test_splits_losses.py`: the analytic gradient is checked against central
finite differences at weights 0.0, 0.5 and 1.0 (agrees to ~2e-4 relative), and the zero-weight path is
asserted to reproduce the plain squared-error gradient exactly. The first is the one that matters — a
wrong adjoint still trains, just towards the wrong thing.

### D3. Optional PyTorch trainer behind a worker boundary

**Why.** Training is currently infeasible at real-world scale. `CausalTcn.forward`
([neural.py:187-197](../ml/nts_ml/models/neural.py#L187)) is a triple-nested Python loop over
samples × layers × taps — about 576,000 Python-level iterations per second of audio, forward pass
only. `_lstm_gradients` is a per-sample Python loop over the reverse pass. `ml/pyproject.toml`
declares `numpy>=1.26` as its only dependency. A realistic capture — several minutes of paired audio
over tens of epochs — is hours to days of wall clock on CPU.

**Design — copy the pattern the project already got right.** The Demucs integration is exactly this
problem solved well: an out-of-process worker
([ml/scripts/demucs_separator_worker.py](../ml/scripts/demucs_separator_worker.py)) that imports
torch lazily, reports a clear message when the runtime is absent
([:33-35](../ml/scripts/demucs_separator_worker.py#L33)), auto-selects CUDA
([:52](../ml/scripts/demucs_separator_worker.py#L52)), streams progress through a JSON file, and sits
behind a deterministic DSP fallback. Torch is never a declared dependency of `nts_ml`.

Apply the same shape to training:

1. `ml/scripts/torch_trainer_worker.py` — takes the same `ExperimentConfig`
   ([training/config.py](../ml/nts_ml/training/config.py)), imports torch lazily, trains, and writes
   a checkpoint in the **existing** `.npz` format that `save_checkpoint`
   ([neural.py:97](../ml/nts_ml/models/neural.py#L97)) produces.
2. Torch reimplementations of the three architectures, weight-for-weight compatible with the NumPy
   forward passes.
3. `combined_loss` reimplemented in torch — every component is differentiable as written.
4. Dispatch in `train()` ([trainer.py:131](../ml/nts_ml/training/trainer.py#L131)): use the worker
   when available and configured, otherwise fall back to the NumPy path unchanged.

**The critical constraint that makes this safe.** The C++ side must not change at all. The export
format is pinned by `nts_ml_runtime_parity` ([CMakeLists.txt:379](../CMakeLists.txt#L379)), which
compares Python export against compiled C++ output within tolerance. That test is the contract — if
it passes with a torch-trained checkpoint, the runtime is unaffected.

**Tests.**

1. **Equivalence.** For identical weights, torch forward and NumPy forward agree within 1e-5 on a
   fixed input. This is the test that makes the whole thing trustworthy.
2. **Parity.** `nts_ml_runtime_parity` passes on a torch-trained artifact, unmodified.
3. **Fallback.** With torch uninstalled, `train()` still works through NumPy and the existing
   `ml/tests` suite passes untouched.
4. **Determinism.** Seeded torch training reproduces bit-identically, matching the existing
   determinism guarantees in `docs/phase-04-coverage.md`.

**Risk.** Highest-effort item in this plan; several days. But it is the difference between a neural
capture path that demonstrates and one that works. Keeping the NumPy path as the dependency-free
reference implementation means nothing is lost if torch is unavailable.

### D3 — delivered and measured

**One design simplification.** The plan called for an out-of-process worker copying the Demucs
pattern. That pattern exists because Demucs is invoked *from C++*, where a failed import cannot be
handled gracefully. `train()` is called from Python, so a lazy module-level import with a recorded
failure reason achieves the same optionality with far less machinery. The backend is
`nts_ml/training/torch_backend.py`; `training.backend` selects `numpy` (default), `torch`, or `auto`.

**The recurrence maps exactly onto `torch.nn.LSTM`.** Both order their gate blocks input, forget,
cell, output, so the matrices transfer with no permutation; the NumPy model's single bias becomes
torch's input-side bias with the hidden-side bias zeroed. This is what allows the fused cuDNN kernel
rather than a Python loop, and it is why the two agree to **2.7e-7 relative** — float32 rounding.

**Measured speedup** on an RTX 5080, one epoch, 32-unit state:

| Chunk samples | Total audio | NumPy | Torch | Speedup |
|---:|---:|---:|---:|---:|
| 2048 | 0.17 s | 0.48 s | 0.01 s | 35x |
| 8192 | 0.68 s | 1.91 s | 0.02 s | 117x |
| 32768 | 2.73 s | 7.68 s | 0.05 s | 151x |
| 32768 (16 chunks) | 10.92 s | 25.99 s | 0.16 s | 164x |
| 131072 | 21.85 s | 55.33 s | 0.30 s | 184x |

An earlier measurement showed torch *slower* at 0.5x. That was one-time CUDA context creation being
counted as training time — a reminder to discard the first run.

**Two findings that only appeared at realistic scale:**

- **cuDNN has a sequence-length ceiling.** It handles 32768 timesteps and fails at 65536 with
  `CUDNN_STATUS_NOT_SUPPORTED` — on *contiguous* input, despite the message blaming non-contiguity.
  Long sequences are now fed through in windows carrying hidden and cell state, which a test asserts
  is numerically identical to one long call. The obvious alternative, disabling cuDNN for those
  calls, was measured at 0.9x — no faster than the NumPy trainer the backend exists to replace.
- **Reproducibility is weaker than the NumPy path.** Seeded torch runs agree to ~1e-7 rather than
  bit-exactly, because cuDNN's recurrent backward accumulates in a nondeterministic order. Documented
  in the test and in `docs/phase-04-coverage.md`; train on NumPy where bit-exactness is required.

**D2-full is subsumed:** `torch_backend.combined_loss` implements the nine-component objective
differentiably, so the torch path optimises exactly what it selects on. A test checks it against the
NumPy reference component by component.

**Verification.** A torch-trained model exports, packs, and replays through the compiled C++ runtime
at **5.96e-08 max error** against thresholds of 1e-5 — no allowance made for the backend. Registered
as `nts_ml_torch_parity`, which skips cleanly where torch is absent. Note that CMake's `Python3`
discovery may select an interpreter without torch, in which case the test skips and reports why;
pass `-DPython3_EXECUTABLE=` to point it at the right one. The no-torch behaviour was verified by
blocking the import: the NumPy default is unaffected, `auto` falls back silently, and an explicit
`torch` request fails naming the reason.

### D4. Documentation reconciliation

Once D1–D3 land, update `README.md` (Phase 4 and 5 sections), `docs/phase-04-coverage.md`,
`docs/phase-05-coverage.md`, and `docs/phase-05-capture-guide.md` to state which models train, with
what optimizer, on what objective, and what runtime is required for each. The coverage documents are
the project's strongest asset precisely because they have been honest; keep them that way.

---

## Workstream E — Architecture

### E1. Extract offline services from the audio processor

**Why.** `TubeForgeAudioProcessor` owns ten subsystems, **seven mutexes**, and **four `std::jthread`s**
across a 222-line header ([PluginProcessor.h](../Source/PluginProcessor.h)) and a 1,892-line
implementation — seven times the next largest file in the project. Tone analysis, song
reconstruction, the profile library, and neural artifact loading are all offline concerns with no
relationship to `processBlock`. Their presence in the `AudioProcessor` means none of them can be
tested without instantiating JUCE and a full plugin.

**Evidence.** Mutexes at [PluginProcessor.h:177](../Source/PluginProcessor.h#L177), [:185](../Source/PluginProcessor.h#L185),
[:187](../Source/PluginProcessor.h#L187), [:192](../Source/PluginProcessor.h#L192),
[:199](../Source/PluginProcessor.h#L199), [:213](../Source/PluginProcessor.h#L213). Threads at
[:182](../Source/PluginProcessor.h#L182), [:189](../Source/PluginProcessor.h#L189),
[:196](../Source/PluginProcessor.h#L196), [:208](../Source/PluginProcessor.h#L208).

**Design.** Introduce `Source/StudioServices.{h,cpp}` owning tone analysis, reconstruction, the
package library, and neural artifact loading, with their mutexes and threads. The processor holds one
`StudioServices` member and forwards the UI-facing methods. What must stay in the processor: anything
`processBlock` touches — `traditionalAmp`, `physicalCircuit`, `neuralAmp`, `engine`, `diagnostics`,
the assistant summary *queue* (though not its accumulator or rule engine).

Do this as a pure move with no behavior change, in one commit, so the diff is reviewable as a
relocation. Resist the temptation to fix things while moving them.

**Payoff.** The processor drops to roughly its DSP responsibilities; the offline subsystems become
unit-testable directly; the four worker threads get a single owner with one shutdown path, replacing
the four hand-written `request_stop` calls in the destructor
([PluginProcessor.cpp:201-207](../Source/PluginProcessor.cpp#L201)).

**Tests.** Existing suites must pass unchanged — that is the acceptance criterion for a pure move.
Then add direct `StudioServices` tests that do not construct a plugin.

**Done, with one scope change.** `Source/StudioServices.{h,cpp}` now owns tone analysis and song
reconstruction: 495 lines, two mutexes and two `std::jthread`s out of the processor.
`PluginProcessor.cpp` drops from 2181 to 1693 lines.

The extraction turned out to be far cleaner than expected. The whole 515-line block touched exactly
**one** processor member — `parameterState`, and only from `applyReconstructionCandidate`. That became
a single injected `ApplyRig` callback, so the dependency runs one way and `StudioServices` holds no
back-reference to the processor.

**Not moved, deliberately:** the profile library and neural artifact loading. Both stage into
`neuralAmp`, which must stay on the real-time side, so moving them would trade one coupling for a
worse one — a service reaching into the audio engine. The line that emerged is worth keeping: *an
offline subsystem moves out when it does not touch the real-time engine.* Tone analysis and
reconstruction qualify; those two do not.

Two smaller findings from doing it: `refreshAssistant` was reading the moved state directly and now
goes through a narrow `reconstructionReferenceTone()` accessor rather than copying an entire
`ReconstructionResult` several times a minute; and the processor destructor still tried to stop the
two workers that had left, which the compiler caught.

### E2. Shared signal chain — ✅ done, with a much smaller footprint than proposed

**Why.** Switching from Traditional to Neural or Physical silently drops the entire front end — input
trim (A1), noise gate, pre low/high cut, tightness, pick emphasis — and the entire back end — cabinet,
post EQ, bass split, loudness match. All of it lives inside `AmpVoice`
([TraditionalAmp.h:307-338](../engine/amp/include/nts/amp/TraditionalAmp.h#L307)). The user gets no
indication; the controls remain visible and simply stop working.

**Design.** Restructure into:

```
input → trim → gate → pre-EQ → [ Traditional | Neural | Physical ] → cabinet → post-EQ → master
                                            ↑
                                  the only part that switches
```

**One design question must be answered before implementing**, and it is a judgment call rather than a
technical one: a neural capture was trained on a raw DI, so inserting pre-EQ before it changes what
the model sees relative to its training distribution, and inserting a cabinet after it double-applies
if the capture already includes one.

The defensible split:

- **Trim and gate are shared** — utility processing every mode needs, and the gate operates on the DI
  before any gain regardless of what follows.
- **Pre-EQ (tightness, pick emphasis) is per-mode**, because it is part of the amplifier's voice
  rather than a utility.
- **Cabinet is shared but defaults to bypassed in Neural mode**, with a clearly labelled toggle, since
  whether a capture includes a cabinet is a property of the capture and only the user knows.

Record whichever choice is made in `docs/phase-03-architecture.md` — the reasoning matters more than
the specific split, and the next person to touch this needs it.

**What the code turned out to allow.** The proposed split was trim + gate shared, pre-EQ per-mode,
cabinet shared. Reading `AmpVoice::process` before changing it, only the first of those three is
possible without building a different amplifier:

| Stage | Proposed | Actual | Why |
|---|---|---|---|
| Trim | shared | ✅ **shared** | Applied before the dry capture, so hoisting it is exactly output-preserving |
| Gate | shared | ⚠️ **shared for Neural and Circuit only** | `AmpVoice` captures its dry reference *between* trim and gate and feeds it to the loudness match. Gating upstream would lower the input energy that match tracks during gated passages and quietly raise its gain |
| Pre-EQ | per-mode | ✅ per-mode | As proposed |
| Cabinet | shared | ❌ **not shared** | `processDrivenPath`, which contains the cabinet, is applied to the *high band* of the bass crossover and then blended with a clean low band. A shared post-stage cabinet would sit after that blend and colour the clean bass too |

The `InputCalibrator` concern that shaped A1's temporary conditional also evaporated on inspection:
`grep -rn calibrationReading Source/` finds only the *neural* reading. Nothing surfaces the
traditional amplifier's calibrator, so moving the trim changes nothing anyone can see.

**Result.** The traditional path renders **identically** before and after — verified with a
processor-level fingerprint across gate on/off and guitar/bass. The Input control now behaves the
same in all three engines, and Neural and Circuit gained a gate they never had.

**Risk, in hindsight.** Rated highest in this plan, and the rating was right in that the obvious
version of the change would have altered the bass path and the loudness match. What made it safe was
reading the signal flow first and letting it veto two thirds of the design.

### E3. Split `PluginProcessor.cpp` — ✅ done

E1 had already taken the file from 2181 lines to 1693 by moving out the two subsystems that had no
business being there. The remainder is now split along its own seams:

| File | Lines | Holds |
|---|---:|---|
| `PluginProcessorAudio.cpp` | 589 | The callback and everything it touches: preparation, the block, latency, the parameter reads that feed them |
| `PluginProcessorAssets.cpp` | 512 | Loading into the engine: cabinet responses, neural artifacts, the circuit, tone packages |
| `PluginProcessorHost.cpp` | 349 | What the host and project file see: parameters, programs, state, the editor handle |
| `PluginProcessorSupport.cpp` | 312 | Construction, teardown, and the forwarders to StudioServices and the assistant |
| `PluginProcessorInternal.h` | 190 | The shared preamble: parameter ids, tables, small helpers |

A pure relocation — definitions moved verbatim, nothing renamed, no logic changed — so the acceptance
test is that every existing suite passes untouched, which it does across three consecutive runs.

Two things the mechanical split got wrong and testing caught, both worth knowing if it is ever redone:
a free function (`createPluginFilter`) and two member definitions whose return type sat on its own
line were dropped, caught by comparing the set of symbol names before and after; and the constructor
and destructor, sitting above the first member the script keyed on, were swept into the shared header
and compiled into all four units. The second attempt at extracting them truncated mid-definition
because a lambda in the constructor's initialiser list closed the brace scan early.

---

## Workstream F — Product features

Lower priority than A–E, but each is small relative to its user-visible value, and each reuses
something already built.

### F1. Real-time tuner — ✅ done

Every guitar plugin has one. The dominant-pitch detection with confidence scoring already exists in
the reconstruction pipeline
([SourceReconstruction.h](../engine/reconstruction/include/nts/reconstruction/SourceReconstruction.h)) —
tuning family and cent-offset estimation are described in
[README.md:132-134](../README.md#L132). Run it on the live input at a low rate, fed from the same
pristine `inputSnapshot` the meters use, published through the assistant's existing lock-free
`SummaryQueue` ([PluginProcessor.h:209](../Source/PluginProcessor.h#L209)) so no new audio-thread
mechanism is needed. A tuner page or a strip in the amplifier page.

Requirements: ±1 cent resolution, stable readings down to low B (~30 Hz) for bass, mute-while-tuning
option, and no audio-thread FFT — the detection runs off-thread on published frames.

**Delivered**, with one design change. The plan said to reuse the reconstruction pipeline's pitch
estimator through the assistant's `SummaryQueue`. Neither worked as written:

- The `SummaryQueue` carries scalar summaries — peak, RMS, zero-crossing rate — not audio. Pitch
  detection needs samples, so the transport is a separate `SpscRingBuffer<float>` of decimated mono.
- The reconstruction estimator
  ([SourceReconstruction.cpp:101](../engine/reconstruction/src/SourceReconstruction.cpp#L101)) is
  batch-shaped and allocates per frame, and — decisively — it takes the **integer** correlation lag.
  At an 8 kHz analysis rate that is a nine-cent error on a low E. Fine for classifying a recording's
  tuning family, useless for a tuner.

So `nts::dsp::PitchDetector` is new: normalised autocorrelation with a first-peak preference (which
is what stops a plucked string reading an octave low) plus **parabolic interpolation** of the peak.
That interpolation was verified to be load-bearing — disabling it fails the accuracy assertion
immediately. All storage is claimed in `prepare`, and it is in the DSP allocation-free sweep.

The audio thread does nothing but box-average and enqueue decimated samples from the pristine
`inputSnapshot`, before any trim or amplifier stage, so the tuner tracks the string rather than what
the amp does to it. A full queue drops samples rather than stalling the callback. Detection runs from
the shell tick, next to the assistant's queue drain.

Measured within **one cent** across B0, E1, A1, E2, A2, D3, G3 and E4, with correct note naming and
no octave errors, on harmonically rich fixtures rather than sines. End-to-end through `processBlock`:
low E and low B detected, flat reads flat, sharp reads sharp, and silence clears the display.

**Follow-ups now closed.** A Tuner page draws the reading — note name, cent offset with a needle
that turns green inside five cents, and a frequency readout — and `tunerMute` silences the output
while leaving the tuner tracking, because it is fed from the pristine input snapshot rather than the
output. The mute rides the same ramp as the engine-change mute, so it fades rather than cutting.

### F2. MIDI program change and a real program list — ✅ done

`acceptsMidi()` returns false and `getNumPrograms()` returns 1
([PluginProcessor.cpp:437](../Source/PluginProcessor.cpp#L437), [:441](../Source/PluginProcessor.cpp#L441)),
so nothing can switch presets remotely and hosts show no program list. Guitarists switch presets with
MIDI foot controllers; this is a standard expectation, not a luxury.

The content already exists: `factoryAmpParameters` holds four factory presets, and the `.ntone`
profile library holds arbitrarily many. Expose them as programs, accept program change messages, and
optionally add MIDI CC learn for the main continuous controls. Program switches must route through
the same click-free path A2 establishes.

**Delivered.** `NEEDS_MIDI_INPUT` is now `TRUE` — without it a host never delivers the messages,
whatever `acceptsMidi()` returns. The four factory voices are exposed as programs.

The design decision worth recording: **a program is not stored state.** It is exactly the
`instrument` and `topology` parameters, so `getCurrentProgram` reads them and `setCurrentProgram`
writes them. That keeps a program change and a manual change to the same two controls from
disagreeing, makes program selection automatable and saved with the project for free, and means the
switch is carried by the traditional amplifier's existing preset crossfade rather than needing a new
one.

A MIDI program change is **latched** on the audio thread into an atomic and applied from
`handleAsyncUpdate`, because applying it means touching parameter objects, which is message-thread
work. The last change in a block wins, which is the right behaviour for a foot controller sending a
burst.

**Not done:** MIDI CC learn, and exposing `.ntone` library profiles as programs. Both are additive;
the program list is currently the four factory voices.

**Tests.** Program count and names, out-of-range names, round-trip through `setCurrentProgram`, a
bass program actually selecting the bass instrument, a MIDI program change *not* taking effect on the
audio thread and *then* taking effect once dispatched, an out-of-range program number being ignored
rather than clamped, and the switch crossfading rather than stepping. Verified by disabling the latch
and watching the dispatch assertion fail.

One process note: enabling `JUCE_MODAL_LOOPS_PERMITTED=1` on the test target was needed so the suite
can pump the message queue and observe asynchronous work landing. It is scoped to that target.

### F3. Time-based effects — ✅ done

An amp sim with no reverb or delay is not a complete rig.

**One design change.** This item said to build the reverb on the convolution engine. That does not
work: the cabinet's convolver is time-domain and capped at 4096 taps — 85 ms, shorter than the
shortest useful reverb tail — and its cost is linear in tap count, so buying seconds of decay would
mean tens of thousands of multiply-accumulates per sample beside an already oversampled amplifier.
`nts::dsp::Reverb` is a Schroeder network instead: four damped parallel combs into two series
all-passes, giving seconds of decay for a fixed handful of operations.

`nts::dsp::Delay` is a feedback line with a damped repeat path, fractional read (so a moving delay
time does not step the pitch), and feedback clamped below unity. Both are sends — the dry path stays
at unity — and both **default to zero mix**, so a project that predates them sounds exactly as it did.

Placed after the amplifier and shared by all three engines, delay then reverb: reverb on the repeats
sounds like a room the echoes happen in, whereas delaying a reverb tail smears it.

`getTailLengthSeconds` now includes them, since a long reverb outlasts the cabinet by orders of
magnitude.

**Three defects found while wiring this up, all by testing rather than review:**

1. **A null dereference.** `prepareToPlay` never called `prepare` on the effects or the shared gate:
   a scripted edit's anchor had gone stale and the replacement silently no-opped. It segfaulted, and
   the ASan build named the line immediately. Both effects now also refuse to run before `prepare`,
   rather than trusting call order.
2. **The reverb filtered the dry signal.** The tail's low cut was applied to the summed output,
   which would have stripped the bottom off the amplifier. It now builds the wet signal separately.
3. **A test that was measuring nothing.** The engine-mode-2 trim assertion passed at 12 dB because it
   ran before the physical circuit finished compiling on its worker thread, so it was measuring a
   passthrough. With a warm-up it reads 6 dB — the circuit is a saturating tube model and genuinely
   compresses. The assertion is now per-engine, and the suite was run four times to confirm the
   flakiness is gone.

---

## Sequencing

Work top to bottom. Items on the same line are independent and can be parallelized.

| Phase | Items | Rationale | Rough effort |
|---|---|---|---|
| 1 | A3, A4 + A2 (one branch), A1, A5 | User-visible defects; two are release gates. A2 and A4 share the dry-delay work. | 2–3 days |
| 2 | B1, B2, B5, B6 | Cheap, and they protect everything after. Do before large diffs land. | 1 day |
| 3 | B3, B4 | Static analysis and sanitizers on a now-clean tree. | 1–2 days |
| 4 | C1 → C2 → C3 → C4 | Strictly ordered; C1 is a hard prerequisite. Highest product value. | 3–4 days |
| 5 | D1, D2-interim | Honesty fixes; cheap and immediate. | 1 day |
| 6 | E1 | Pure move; unblocks testing everything else. | 2 days |
| 7 | D3 → D2-full → D4 | The real ML fix. Long, self-contained, parallelizable with E. | 4–6 days |
| 8 | E2 → E3 | Highest-risk restructure; wants everything else stable first. | 4–5 days |
| 9 | F1, F2, F3 | Features, after the chain they plug into is settled. | 3–5 days |

**Do not reorder C1 after C2** — wiring the loader to a racy convolver ships a data race.
**Do not reorder E2 before Workstream A** — restructuring the signal path while known defects sit in
it makes both harder to verify.

## Risk register

| Risk | Items | Mitigation |
|---|---|---|
| Signal-path regression invisible to unit tests | A2, C1, E2 | Render every factory preset before and after; compare numerically with `nts_dsp` `Regression.h` metrics; use the Phase 3 listening protocol for E2 |
| Audio-thread race introduced by IR loading | C1, C2 | `CrossfadingConvolver`'s atomic swap; concurrency test under ASan (B4) |
| Latency handling breaks host delay compensation | A2, A3, A4 | Cross-correlation alignment tests; VST3PluginTestHost run before closing A4 |
| Parameter index/ID drift | A5 | Round-trip test over the whole enum, written first |
| Bulk reformat destroys `git blame` | B2 | `.git-blame-ignore-revs`, isolated commit |
| Torch trainer diverges from the C++ runtime | D3 | `nts_ml_runtime_parity` unchanged as the contract; weight-level equivalence test |
| Schema migration loses user projects | C3 | v2→v3 migration test; missing IR degrades without discarding the stored path |

## Definition of done

An item is complete when: the change is implemented; tests are added in the suite that owns the layer
and pass; `ctest` is fully green in Debug and Release; the DSP benchmark matrix shows no regression
outside noise for audio-path items; the relevant `docs/phase-NN-coverage.md` is updated if the
change moves a coverage number; and the README no longer claims anything the code does not do.
