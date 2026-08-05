# Tone-life fix plan: dull song match, gate boom, dead pedals

An implementation-ready plan for the tone quality defects found by listening tests: song matches
that sound dull and lifeless, a loud "boom" on the first attack after a pause, a noise gate that
kills sustain, excessive manual correction after a match, topology dominating the shortlist, and
neural pedal captures that barely respond to Drive.

Every item states the symptom, the mechanism with evidence, the change, the risk, and what "done"
means. Line references are against the working tree at commit `f55407d`; re-check them before
editing.

None of these defects are neural-network problems. Seven of the eight are in the traditional amp
and the reconstruction search; one is in pedal wiring. See
[Out of scope](#out-of-scope-and-why) for the WaveNet-side proposals this plan deliberately drops.

## Status

| Track | Items | Effort | State |
|---|---|---|---|
| A — Stop the output stage fighting the tone | T1, T2, T3 | 0.5 d | 🟡 T1, T3 done; **T2 attempted and reverted** — see below |
| B — Un-bias the candidate search | T4, T5 | 0.5 d | ✅ Complete |
| C — Make dynamics steerable | T6, T7 | 1.5 d | ✅ Complete |
| D — Gate as an expander | T8 | 0.5 d | ✅ Complete |
| E — Neural pedal calibration | T9, T10 | 0.75 d | ✅ Complete |
| F — Layered and panned sources | T11a, T11b, T11c | 1.25 d | ✅ Complete |

**Coverage: 95% by effort** (4.75 of 5.0 days). Every item except T2.

T2 (0.25 d) is counted at zero: it was implemented three ways, none could be shown to fix anything,
and all were reverted. It is the only outstanding work and needs re-diagnosing rather than
re-implementing.

All 12 buildable suites pass: `nts_amp_tests`, `nts_dsp_tests`, `nts_pedal_tests`,
`nts_reconstruction_tests`, `nts_tone_analysis_tests`, `nts_assistant_tests`, `nts_ecosystem_tests`,
`nts_ml_runtime_tests`, `nts_unit_tests`, `nts_integration_tests`, `nts_device_tests`, `nts_ir_tests`.
`nts_wrapper_tests` and the plug-in itself cannot be linked here — `nts_circuit` does not compile at
HEAD, unrelated to this plan and tracked separately.

One planned test was not delivered: T4's unit test on `coarsePoint`. That function is in an anonymous
namespace in `SourceReconstruction.cpp` and is not reachable from `Tests/`. Asserting the property
through `reconstruct()` instead is fragile — the shortlist keeps 3 or 4 of a pool of 12 after
refinement re-ranks it, so the unbiased grid point is not guaranteed to survive into the observable
output even when the grid is correct. Exposing `coarsePoint` through a detail header would make the
exact test possible and is the recommended follow-up.

**T1 is a hard dependency of Track C.** Candidates are currently rendered with the loudness matcher
active and then scored on crest factor, so the dynamics terms partly measure the matcher. Adding a
dynamics axis before T1 lands would train the search to match the matcher.

Suggested first session: **T1, T2, T4** — three small changes that address the loudest symptoms and
do not touch each other.

## Symptom-to-cause map

| Symptom (from listening tests) | Item | Mechanism |
|---|---|---|
| First attack after a pause is a loud boom, then normalises | T2 | Loudness matcher winds up to its +12 dB clamp during gated decay |
| "Doesn't know how to adjust gain"; raising drive does nothing | T1, T3 | Loudness matcher normalises output RMS back to DI RMS, and is not exposed in the UI |
| Character is right but the match is dull | T4, T5 | Coarse grid renders 10 of 12 candidates darker than measured, never brighter; the brightness correction that would offset it is dead code |
| Dynamics feel lacking / lifeless | T6, T7 | Transients are measured and scored at 6%, but no candidate parameter is driven by any dynamics measurement |
| Gate kills the tone; only noise-floor removal wanted | T8 | `rangeDb` defaults to −80 dB (full mute), and the gate runs pre-amp for two of three engines |
| Too much manual correction after a match | T4, T5, T7 | Three `DiAdaptation` fields are computed and never applied; `pickEmphasisDb` is never fitted |
| Topology dominates the shortlist | T5 | Topology is the fastest search digit, frozen during refinement, and forced into the shortlist by the diversity filter |
| NAM pedals silent at Drive 5, weak at 10 | T9, T10 | Pedal neural slots never get input compensation; drive range is −12…+12 dB so Drive 5 is unity |

---

## Track A — Stop the output stage fighting the tone

### T1 — Render reconstruction candidates with the loudness matcher off

**Symptom.** Matched rigs sound dull and dynamically flat; the dynamics and transient similarity
terms do not discriminate.

**Mechanism.** `AmpParameters::loudnessMatch` defaults to `true`
([TraditionalAmp.h:198](../engine/amp/include/nts/amp/TraditionalAmp.h#L198)) and
`makeOriginalPreset` never touches it, so every rendered candidate runs the matcher. The matcher is
a slow compressor: `matchGain` chases `sqrt(inputEnergy / outputEnergy)` with a coefficient of
`0.0002` (≈104 ms at 48 kHz) at
[TraditionalAmp.cpp:1039-1046](../engine/amp/src/TraditionalAmp.cpp#L1039). It flattens crest factor
before the tone analyser sees the render, and `ToneSimilarity::compare` then scores that render on
`crestFactorDb`, `loudnessRangeDb`, `compressionAmount` and `attackMilliseconds`.

This is the same class of bug as the note already in the tree at
[SourceReconstruction.cpp:357](../engine/reconstruction/src/SourceReconstruction.cpp#L357) — *"candidates
were rendered at 1x, so the tone analyser measured the aliasing of the render rather than the
amplifier"* — one axis over.

**Change.** In `setCandidateParameters`, next to the existing `oversamplingFactor = 4` line and its
comment, set `p.loudnessMatch = false`. Add a comment recording why, in the same voice as the
aliasing note.

**Risk.** Candidate renders get louder and more dynamic, which shifts every similarity score. The
existing shortlist snapshots in the reconstruction tests will move. That is the point, but it means
T1 invalidates any stored expected scores — update them in the same commit.

**Done when.** A high-gain reference renders candidates whose measured `crestFactorDb` tracks the
preset's drive rather than sitting near-constant across the pool.

### T2 — Freeze `matchGain` while the gate is not open — ❌ ATTEMPTED, REVERTED

**The diagnosis below is not confirmed, and both fixes built from it failed.** Recorded in full
because the measurements are the useful part; do not re-attempt this item from the theory as
written.

What was tried, in order:

1. **Freeze `matchGain` while `gate.state()` is not `open`/`holding`.** Made the measured defect
   *worse*: the burst-to-burst step went from 1.4 dB to 3.5 dB. The energy estimators are 42 ms
   one-poles and kept integrating through the pause, so resuming only the gain restarted tracking
   with both averages holding gated values — the spike moved to the attack instead of ending.
2. **Freeze the energy estimators along with the gain.** No improvement: 3.9 dB.
3. **Hold on silence instead of drifting to unity** — replacing the
   `matchGain += 0.0002 * (1.0 - matchGain)` else-branch with a hold, keyed on output energy rather
   than on the gate. Analytically the strongest candidate, and it left the gated case *bit-identical*
   at 1.382 dB, because with a −80 dB gate depth and a quiet-but-nonzero gap the output energy never
   falls under the credibility floor, so the branch never runs.

What the instrumentation did establish, and what contradicts the plan:

- On `Original Tight Guitar` the matcher settles at its **lower** clamp, not its upper one: measured
  `inputEnergy = 1.6e-3` against `outputEnergy = 1.6e-1`, so the ratio wants −20 dB and is clamped
  to −12 dB. The `sqrt(in/out)` term pinning to `4.0` — the entire basis of the "+12 dB boom"
  story — does not happen on this preset.
- The gate never reported `closed` in the traced runs, only `opening` and `open`, which is why any
  gate-state-keyed fix was inert.
- A real level step across a pause **does** exist and is attributable to the matcher: 1.4 dB gated
  and 4.2 dB ungated between the end of one burst and the start of the next, against under 1 dB with
  `loudnessMatch = false`. So the user's symptom is real; the mechanism is not the one below.
- The most likely remaining explanation is benign rather than a bug: an RMS-ratio matcher across a
  nonlinear device legitimately converges to a different ratio at low level than at high level, and
  carries that into the next note.

**Recommended re-scope.** Do not theorise further from synthetic bursts. Instrument `matchGain` over
time in the actual plug-in, on the material where the boom was heard, and find the configuration
where it moves upward before designing a fix. T3 (exposing the control) is worth doing regardless and
gives the user an immediate escape.

The original diagnosis follows, retained for reference only.

---

**Symptom.** The first note after any pause is up to +12 dB too loud, then settles over ~100 ms.
Only happens with the gate enabled, which is why it reads as a gate bug.

**Mechanism.** The dry reference is captured **before** the gate (deliberately — see the comment at
[PluginProcessorAudio.cpp:283](../Source/PluginProcessorAudio.cpp#L283)) while `outputEnergy` is
measured **after** it ([TraditionalAmp.cpp:1035-1046](../engine/amp/src/TraditionalAmp.cpp#L1035)).
When a note decays below threshold the gate closes over the release time (80 ms default) while
`inputEnergy` still holds the DI level. The ratio explodes, `target` pins to its `4.0` clamp
(+12 dB), and `matchGain` climbs toward it. The next pick arrives into that accumulated makeup.

The `outputEnergy > 1.0e-9` guard (≈ −90 dBFS RMS) sits far below the gate's closed floor
(`rangeDb` −80 dB), so the else-branch that would pull `matchGain` back to unity rarely fires.

The comment at `PluginProcessorAudio.cpp:283` shows this hazard was already understood and solved
for the neural and circuit engines by hoisting the gate out of the amp. The traditional engine still
has the gate inside the loop at
[TraditionalAmp.cpp:993](../engine/amp/src/TraditionalAmp.cpp#L993), so it still happens there.

**Change.** Gate the matcher on the gate's own state. `AmpVoice` already owns the `NoiseGate`, and
`NoiseGate::state()` is public:

- when `parameters.gateEnabled && gate.state() != GateState::open`, skip both the `target`
  computation and the `matchGain` update, holding `matchGain` at its current value;
- raise the `outputEnergy` guard from `1.0e-9` to roughly `1.0e-6` (≈ −60 dBFS RMS) so the
  unity-restoring branch engages at realistic noise floors.

Holding rather than resetting is deliberate: resetting to 1.0 would produce a level jump in the
other direction on every gate close.

**Alternative considered.** Measuring `inputEnergy` post-gate instead. That is arguably more
correct, but it changes what the matcher means for the ungated case too, and the comment at
`PluginProcessorAudio.cpp:283` documents a deliberate reason for the current tap. Freezing is the
smaller, more reversible change; revisit the tap if T2 proves insufficient.

**Risk.** Low. Confined to the branch that already exists.

**Done when.** A test that plays a burst, silence past the gate close, then a second burst, shows
the second burst's peak within 1 dB of the first. This is a regression test worth keeping.

### T3 — Expose `loudnessMatch` as a plug-in parameter — ✅ DONE

Landed at all six sites: id in `PluginProcessorInternal.h` plus an **appended** entry in the ordered
id list, the `TUBEFORGE_RUNTIME_PARAMETERS` macro, a `Bool` in the layout defaulting to `true`, the
read in `currentAmpParameters`, both preset-apply paths, and a toggle on `ToneShapingPage` beside the
noise-gate one.

**Compile-verified, not link-verified.** Every shell translation unit compiles, including all four
this item changed, but `nts_wrapper_tests` cannot link: `nts_circuit` does not build at HEAD in this
environment (`Components.cpp:12` declares a `constexpr std::array` of a type holding `std::string`
members, which MSVC rejects with C2131). That break is unrelated to this plan and is tracked
separately. The plug-in has therefore not been run, so the control's *placement* on the page is
unverified by eye.

### T3 — original entry

**Symptom.** Turning up drive or master does not get louder, and there is no way to stop it.

**Mechanism.** `loudnessMatch` exists only in preset JSON
([TraditionalAmp.cpp:298](../engine/amp/src/TraditionalAmp.cpp#L298),
[:387](../engine/amp/src/TraditionalAmp.cpp#L387)). Grep finds no parameter id, no UI control, and no
`setParameterValue` call. A user cannot reach it.

**Change.** Add a `loudnessMatch` boolean parameter following the pattern of `gateEnabled`:

- id in [PluginProcessorInternal.h](../Source/PluginProcessorInternal.h) alongside `oversampling`;
- entry in the `X(...)` parameter macro list in
  [PluginProcessor.h:50-54](../Source/PluginProcessor.h#L50);
- read it in `currentAmpParameters` next to `parameters.gateEnabled` at
  [PluginProcessorAudio.cpp:619](../Source/PluginProcessorAudio.cpp#L619);
- write it in both preset-application paths, `PluginProcessorAssets.cpp:546` and
  `PluginProcessorHost.cpp:157`, which already handle `gateEnabled` identically;
- surface it on `ToneShapingPage` next to the oversampling selector.

**Default.** Keep `true` for hand-built presets — it is useful for auditioning at matched loudness.
Reconstruction presets set it `false` via T1, so a loaded song match arrives with it off.

**Risk.** Parameter-list changes affect saved state layout. Append rather than insert, and confirm
an existing session still loads.

**Done when.** The control appears, toggles audibly, and survives a save/reload cycle.

---

## Track B — Un-bias the candidate search

### T4 — Fix the systematic dark bias in the coarse grid

**Symptom.** "Character of the sound is good but recreation lacks the life inside of it." Matches
are consistently darker than the reference.

**Mechanism.** `coarsePoint`
([SourceReconstruction.cpp:322-340](../engine/reconstruction/src/SourceReconstruction.cpp#L322))
uses mixed-radix indexing:

```
topologyStep   = variant % 2
gainStep       = (variant / 2)  % 5 - 2
brightnessStep = (variant / 10) % 3 - 1
tightnessStep  = (variant / 30) % 3 - 1
```

The caller only ever asks for `poolSize = max(candidateCount * 3, 12)` = **12** consecutive variants
([line 786](../engine/reconstruction/src/SourceReconstruction.cpp#L786),
[loop at line 868](../engine/reconstruction/src/SourceReconstruction.cpp#L868)). For variants 0–11:

- `tightnessStep` is **always −1** — the axis is never explored, and every candidate is one step
  below measured;
- `brightnessStep` is **−1 for variants 0–9** and 0 for 10–11 — **never +1**.

The docstring's claim of "2 × 5 × 3 × 3 = 90 unique points" is true of the function and false of the
caller: the two slowest digits are effectively constant.

Brightness then drives four things at once in `setCandidateParameters`
([lines 362-380](../engine/reconstruction/src/SourceReconstruction.cpp#L362)):
`preEq.highCutHz = 6500 + brightness * 9500`, `toneStack.treble = 0.22 + brightness * 0.65`,
`powerAmp.presence = brightness`, `postHighDb = (brightness - 0.5) * 5`. So 10 of 12 coarse
candidates are rendered darker than measured in four places simultaneously, and none brighter.
Refinement is coordinate descent from that winner, so it starts from a dark point chosen against a
dark field.

**Change.** Order each axis's steps so index 0 is the unbiased centre. Replace the arithmetic
`digit - 1` with a lookup:

```cpp
// Steps ordered centre-first so a pool smaller than the full grid is unbiased. The previous
// `digit - 1` form put -1 at index 0, so a 12-variant pool rendered every candidate below the
// measured tightness and 10 of 12 below the measured brightness -- always darker, never brighter.
constexpr std::array<int, 3> centredSteps { 0, -1, 1 };
constexpr std::array<int, 5> centredGainSteps { 0, -1, 1, -2, 2 };
```

Also move `brightness` and `tightness` onto the faster digits and `topology` onto a slower one — see
T5, which shares this edit.

**Risk.** Changes which rigs the shortlist contains for every reference. Any stored expected
candidate names or scores in the reconstruction tests will move.

**Done when.** For a pool of 12, the rendered set contains at least one candidate at or above the
measured brightness and at or above the measured tightness. Assert this directly on `coarsePoint`
output rather than on audio — it is a cheap, exact unit test.

### T5 — Stop topology dominating the shortlist

**Symptom.** "I didn't like that topologies influence song match a lot."

**Mechanism.** Three separate places push topology forward:

1. `topologyStep = variant % 2` — the **fastest** digit, so half a 12-point pool is spent on
   topology rather than tone.
2. `makeOriginalPreset(point.topology, instrument)`
   ([line 807](../engine/reconstruction/src/SourceReconstruction.cpp#L807)) builds the whole preset
   from topology; `setCandidateParameters` only overrides part of it, so topology moves everything
   else too.
3. `refinementNeighbours` deliberately never perturbs topology
   ([line 390](../engine/reconstruction/src/SourceReconstruction.cpp#L390)), so the coarse winner's
   topology is frozen — decided on a sparse, dark-biased sample.
4. The shortlist filter `first.topology != second.topology || ...`
   ([line 916](../engine/reconstruction/src/SourceReconstruction.cpp#L916)) **forces** topology
   variety into what the user sees.

**Change.** Three edits:

- move `topology` to the slowest digit in `coarsePoint`, so a small pool spends its budget on gain
  and brightness;
- drop `first.topology != second.topology` from `audiblyDifferent`, leaving diversity judged on
  drive, treble, mid, high-cut and stage count — parameters a listener can actually hear;
- after refinement converges, re-evaluate the winning point under the **other** topology and keep
  whichever scores higher. This replaces "topology is frozen" with one extra render, and preserves
  the stated reason for not perturbing it during descent (it restarts the search rather than
  refining it).

**Risk.** One extra candidate render per reconstruction. Negligible against a pool of 12 plus
refinement.

**Done when.** Two references that differ only in brightness produce shortlists that differ in
brightness-driven parameters rather than in topology.

---

## Track C — Make dynamics steerable

Depends on **T1**. Without it the dynamics terms partly measure the loudness matcher.

### T6 — Apply or delete the dead `DiAdaptation` fields — ✅ DONE, with one correction

**The claim below that `highShelfDb` is discarded is wrong.** The pre-EQ has no high shelf to put it
in, but `setCandidateParameters` already applies the *same* decision as
`postHighDb = (brightness - 0.5) * 5.0` — the identical formula. So the high-shelf choice was always
reaching the render; the adaptation field is a *report* of it, and the only defect was that it was
computed from the unperturbed `report.brightness.value` while the preset carried the candidate's
`point.brightness`, so it described a shelf up to half a grid step away from the real one. Fixed by
reporting from `point.brightness`. Nothing was recovered on the dullness axis, so this does **not**
compound T4 as the table below suggests.

`dynamicRangeScale` was genuinely unconsumed, and there is no input compressor or expander in
`AmpParameters` to apply it to. Resolved by folding it into `powerAmp.sag` — the amplifier's own
dynamic-range mechanism, where a compressed reference wants more sag and an open one less. Blended
half-and-half with the existing tightness-derived value rather than substituted, because
`refinementNeighbours` steps tightness and would otherwise lose all influence over sag. This also
makes `report.compression` actuate something for the first time, which is a down-payment on T7.

The original text follows.

---

**Symptom.** Excessive manual correction needed after a match; matches dull.

**Mechanism.** `DiAdaptation` has five fields
([SourceReconstruction.h:126-134](../engine/reconstruction/include/nts/reconstruction/SourceReconstruction.h#L126)).
All five are computed at
[SourceReconstruction.cpp:809-813](../engine/reconstruction/src/SourceReconstruction.cpp#L809), but
only three are copied onto the preset at
[lines 814-818](../engine/reconstruction/src/SourceReconstruction.cpp#L814):

| Field | Computed | Applied |
|---|---|---|
| `inputTrimDb` | ✅ | ✅ `manualInputTrimDb` |
| `lowShelfDb` | ✅ | ✅ `preEq.lowShelfDb` |
| `midEqDb` | ✅ | ✅ `preEq.midEmphasisDb` |
| `highShelfDb` | ✅ | ❌ **dead** |
| `dynamicRangeScale` | ✅ | ❌ **dead** |

`highShelfDb = (brightness - 0.5) * 5.0` is up to +2.5 dB of top end for a bright reference,
computed and discarded — it would have partly offset the T4 bias. `dynamicRangeScale =
clamp(1.35 - compression.value, 0.45, 1.35)` is the **only** consumer of the measured `compression`
descriptor, so that measurement currently influences nothing in the render. Grep confirms one
declaration and one assignment for `dynamicRangeScale`, zero reads.

**Change.**

- Apply `highShelfDb` to `preEq.highShelfDb` (enable the shelf as the low-shelf path already does).
- For `dynamicRangeScale`, decide rather than defer: either map it onto `powerAmp.sag` — a
  compressed reference wants less supply sag, an open one more — or delete the field. Mapping is
  preferred and folds into T7. Do not leave it written-and-unread.

**Risk.** Both changes brighten and enliven the output, so they compound T1 and T4. Land them in
separate commits so a listening test can attribute the change.

**Done when.** No field of `DiAdaptation` is written without being read. A grep-based check is
enough.

### T7 — Add a dynamics axis to the candidate search — ✅ DONE, with two deviations

**Deviation 1: `attackReduction`, not `stages[].bias`.** The plan named `bias` as one of the three
parameters to drive. That was the wrong target — `bias` is a duty-cycle offset feeding the even-harmonic
content, and it is already correctly tied to tightness. The parameter that actually controls how much
the amplifier rounds off a pick is `PreampStageConfig::attackReduction`:
`transientGain = 1 / (1 + 22 * attackReduction * attack)` in the preamp loop. Like `pickEmphasisDb` it
was **never fitted** by `setCandidateParameters`, so every candidate ever produced carried whatever
`makeOriginalPreset` left. Driving it is the single largest part of this item.

**Deviation 2: dynamics is a refinement axis, not a coarse one.** The plan put it on a fast coarse
digit. That does not fit in the pool: with five axes and twelve consecutive variants, adding a
three-step digit to the front starves whichever axis it displaces, and raising `poolSize` to cover
5 x 3 x 3 = 45 points would roughly triple reconstruction time. Dynamics is also the one axis seeded
from a *direct measurement* rather than from a descriptor whose mapping onto controls is a guess, so
its centre is already the best coarse answer. It is therefore seeded at the measured value, left
unperturbed by the grid, and stepped by `refinementNeighbours` — which is coordinate descent from a
measurement, better conditioned than sampling a guess. `refinementAxes` is now 4, so a refinement
round costs 8 renders instead of 6.

**A regression in T4, found while reordering the radix and fixed here.** T4 moved gain to
`(variant / 9) % 5`, which at the default pool of twelve sampled only **two** of gain's five steps —
down from all five before T4. Fixing the dullness bias had quietly cost the gain axis most of its
coverage, on the very complaint ("lacking gain") the plan opens with. The digits are now ordered by
what a small pool gains from sampling them: gain fastest (`variant % 5`, all five steps at pool
twelve), then brightness (all three), then tightness and topology at their centres. Unbiased where
unexplored, and both axes covered.

Landed: `CandidatePoint::dynamics`, a `dynamicsSeed` helper normalised against the same 18 dB crest
and 40 ms attack references `ToneSimilarity::compare` uses, the three parameter mappings
(`attackReduction`, `pickEmphasisDb`, and half of `powerAmp.sag` with T6's compression blend on top),
and the refinement axis.

**`transientWeight` raised from 0.06 to 0.12** (bass 0.07 to 0.13), taken out of `nonlinearWeight`,
which still leads at 0.36. Flagged in the code as a judgement rather than a measurement: unlike the
rest of this work it has no failing case pinning the right value, so it is a modest step and wants an
A/B against a fixed reference clip before it moves again.

**Test.** `ReconstructionTests` now asserts the mapping actuates: two references identical except for
`crestFactorDb` and `attackMilliseconds` must produce rigs ordered correctly in all three driven
parameters. Verified to fail on all three assertions when `dynamicsSeed` is pinned to a constant.

The original entry follows.

---

**Symptom.** "Dynamics also feel lacking."

**Mechanism.** Transients *are* measured and *are* scored, but nothing actuates them.

Measured — `DynamicFeatures`
([ToneAnalysis.h:34](../engine/tone-analysis/include/nts/tone/ToneAnalysis.h#L34)) carries
`crestFactorDb`, `transientPreservation`, `attackMilliseconds`, `releaseMilliseconds`, `sustain`,
`compressionAmount`, `loudnessRangeDb`, `gainReductionEstimateDb`, plus
`SpectralFeatures::upperMidAttack` and the `ToneReport::compression` descriptor. Nothing is missing
at the measurement layer.

Scored — `ToneSimilarity::compare`
([ToneProfileDatabase.cpp:155-184](../engine/tone-analysis/src/ToneProfileDatabase.cpp#L155)):

| Term | Weight (guitar) |
|---|---|
| nonlinear | **0.42** |
| embedding | **0.30** |
| dynamics | 0.10 |
| spectral | 0.07 |
| **transient** | **0.06** |
| lowFrequency | 0.05 |

Attack behaviour is 6% of the decision; all dynamics together are 16%, against 72% for harmonic
content and the opaque embedding. A candidate can win on harmonics while being dynamically wrong.

Not actuated — `CandidatePoint` is `{topology, gain, brightness, tightness, driveOffset}`
([line 291](../engine/reconstruction/src/SourceReconstruction.cpp#L291)). **No axis is derived from
a dynamics measurement**, and `setCandidateParameters` sets no parameter from one:

- `report.compression` reaches only the dead `dynamicRangeScale` (T6);
- `preEq.pickEmphasisDb` — the amp's actual pick-attack control, a 2800 Hz peak at
  [TraditionalAmp.cpp:406](../engine/amp/src/TraditionalAmp.cpp#L406) — is **never set** by
  `setCandidateParameters`, and `makeOriginalPreset` does not touch it either, so it sits at 0 for
  every candidate;
- the three parameters that do affect feel — `powerAmp.sag`, `powerAmp.feedback`, `stages[].bias` —
  are all driven by `report.tightness`, a spectral descriptor, **not** by crest factor or attack
  time. And per T4, tightness is pinned one step below measured.

The optimiser can only match dynamics by accident, through whatever correlation tightness happens to
have with sag. There is no gradient from "the reference has a 14 dB crest factor and a 4 ms attack"
to any knob.

**Change.**

1. Add `float dynamics {}` to `CandidatePoint`, seeded from the measured features — a normalised
   combination of `crestFactorDb` (18 dB reference, matching the scorer's normalisation) and
   `attackMilliseconds` (40 ms reference).
2. Give it a digit in `coarsePoint`, on a fast digit alongside brightness.
3. Drive three parameters from it in `setCandidateParameters`, replacing the tightness-derived
   values where they conflict:
   - `powerAmp.sag` — the supply-compression model, the dominant feel parameter;
   - `stages[].bias` — currently `(0.5 - report.tightness.value) * 0.45 + offset`;
   - `preEq.pickEmphasisDb` — currently unset; map a high measured `transientPreservation` to a
     positive value.
4. Add it to `refinementNeighbours` so coordinate descent can walk it.
5. Raise `transientWeight` from 0.06. A tuning call, but 0.06 against 0.42 cannot discriminate feel.
   Change it alone, in its own commit, and A/B it — this weight is the one item here whose right
   value is a judgement rather than a fact.

**Risk.** The largest change in this plan. It widens the search space, so the pool of 12 covers
proportionally less of it — consider raising `poolSize` for the dynamics axis specifically, and
measure the added reconstruction time before committing to it.

**Done when.** Two references with the same spectrum and materially different crest factors produce
candidates whose `powerAmp.sag` and `pickEmphasisDb` differ in the expected direction.

---

## Track D — Gate as an expander

### T8 — Default to noise-floor removal, not muting — ✅ DONE

All three parts landed. `rangeDb`, `gateDepthDb` and the `gateDepth` parameter default now sit at
−15 dB with the full −90 dB range still selectable. `holdRemaining` decrements once per sample
whenever the gate is not being held open, so hold is a duration rather than a
below-threshold-only countdown. The shared gate moved behind the neural and circuit engines via a
`runSharedGate` lambda, so its threshold refers to the signal a player hears rather than to the clean
DI; the traditional engine returns early and keeps gating inside `AmpVoice`, where its dry reference
requires it.

The existing depth-continuum test in `AmpTests` passes unchanged because it sets depth explicitly
rather than relying on the default.

### T8 — original entry

**Symptom.** "Noise gate killing the tone all the time; I only need noise floor removal."

**Mechanism.** Three separate issues:

1. `NoiseGateParameters::rangeDb` defaults to **−80 dB**
   ([Dynamics.h:17](../engine/dsp/include/nts/dsp/Dynamics.h#L17)) — a full mute. Noise-floor removal
   wants a shallow downward expansion.
2. For the neural and circuit engines the shared gate runs **before** the amp
   ([PluginProcessorAudio.cpp:289](../Source/PluginProcessorAudio.cpp#L289)), so it gates a clean DI
   ahead of high gain — chopping sustain — and its threshold must sit above the DI's noise floor
   rather than the amp's output floor, which are tens of dB apart.
3. `holdRemaining` only decrements inside the `envelope < closeThreshold` branch
   ([Dynamics.cpp:83](../engine/dsp/src/Dynamics.cpp#L83)), so while the envelope sits in the
   hysteresis band the state machine freezes and hold never expires.

**Change.**

- Default `gateDepth` to about **−15 dB** in the parameter layout, and default
  `NoiseGateParameters::rangeDb` to match. This turns the default behaviour into an expander without
  new DSP; users who want a hard gate still have the full −120 dB range.
- Decrement `holdRemaining` once per sample regardless of which branch runs, so hold is a duration
  rather than a below-threshold-only countdown.
- For the neural and circuit engines, move the shared gate to **after** the engine, so its threshold
  refers to the same signal a user hears. Keep the traditional engine's internal gate where it is —
  T2 makes it safe.

**Risk.** Changing a default alters existing sessions' behaviour only if they never stored the
parameter. Confirm that saved state round-trips the old value.

**Done when.** With defaults, a decaying note's tail is attenuated rather than cut, and a
noise-floor-only signal drops by the range amount.

---

## Track E — Neural pedal calibration

### T9 — Wire input compensation and controls into pedal neural slots — ✅ DONE

`PedalSlot::configureNeural()` added and called from `setParameters`, so every neural slot opts into
input compensation and receives the slot's Drive and Tone as the first two entries of the model's
control vector in the same −1..1 convention the amplifier uses. A plain `.nam` capture declares no
conditioning and ignores the vector, so it costs nothing there.

The amp's `.nam` staging path now reads `inputRmsDb` from `normalization.json` when the package ships
one, falling back to −21 dB — which is what `stageArtifactDirectory` already did, so this path was the
inconsistent one rather than both being wrong.

### T10 — Re-centre the neural pedal drive range — ✅ DONE, and it exposed a real bug

Range widened from −12…+12 dB to −6…+24 dB.

**The widening broke the empty-slot pass-through, and that was a pre-existing defect rather than a
consequence of the change.** `renderNeural` applied the drive gain as a pre-gain whether or not a
model was staged, so a slot the user had loaded nothing into was still amplifying. It looked correct
only because Drive 5 mapped to exactly 0 dB under the old range; widening it turned the same code into
a 9 dB boost from an empty slot, and `nts_pedal_tests` caught it immediately.

Fixed by returning early from `renderNeural` when `hasActiveModel()` is false, still advancing the
drive smoother so a ramp in flight retires. The existing test was strengthened to sweep drive across
its whole travel instead of sampling the one coordinate where the bug was invisible; verified to fail
at all five settings against the pre-fix code.

### T9 / T10 — original entries

**Symptom.** "NAM pedals don't sound at all, at least when Drive is at 5."

**Mechanism.** `setInputCompensationEnabled` and `setControls` are called **only** on the amp's
`neuralAmp` ([PluginProcessorAudio.cpp:315-321](../Source/PluginProcessorAudio.cpp#L315)). Grep finds
no equivalent for pedal slots, even though each neural slot owns its own `NeuralAmpProcessor`
([PedalBoard.h:149](../engine/pedals/include/nts/pedals/PedalBoard.h#L149)). So:

- the slot's `InputCalibrationMonitor` exists but its compensation is never enabled — the model
  receives whatever level the chain hands it, with no alignment to the capture's declared
  `expectedInputRmsDb`;
- a distilled conditioned model in a pedal slot runs with all five controls pinned at 0, so its own
  knobs do nothing.

Separately, [PluginProcessorAssets.cpp:403](../Source/PluginProcessorAssets.cpp#L403) hardcodes
`-21.0f` when staging a `.nam` import into the amp, ignoring the capture's declared calibration. The
pedal helper at [line 70](../Source/PluginProcessorAssets.cpp#L70) *does* read `inputRmsDb` from the
manifest, so the amp path is the inconsistent one.

**Change.**

- Add a `PedalSlot` method that forwards to `neural.setInputCompensationEnabled(...)` and
  `neural.setControls(...)`, and call it from `PedalBoard::setParameters` so a neural slot is
  calibrated on every parameter refresh, exactly as the amp is.
- Map the slot's own Tone and Level controls onto the model's control vector for conditioned
  models, so a distilled pedal capture responds to its knobs.
- Read the declared calibration in the amp's `.nam` staging path instead of hardcoding `-21.0f`.

**Risk.** Enabling compensation changes the level into every existing neural pedal preset. This is a
correctness fix, but it is audible; note it in the release notes.

**Done when.** A neural pedal capture staged with a declared `inputRmsDb` of −21 produces the same
output level whether the incoming DI sits at −21 or −15 dBFS RMS.

### T10 — Re-centre the neural pedal drive range

**Symptom.** Nothing at Drive 5, "some character" at Drive 10.

**Mechanism.** The `neuralCapture` voicing is `minDriveDb -12, maxDriveDb +12`
([PedalBoard.cpp:49](../engine/pedals/src/PedalBoard.cpp#L49)), and `driveTarget()` maps the 0–10
control linearly across it. Drive 5 is therefore **exactly 0 dB** — unity, uncompensated. Drive 10 is
+12 dB, which is why character appears only at the top of the knob. Against the other kinds:

| Kind | min → max drive |
|---|---|
| boost | 0 → 24 dB |
| overdrive | 6 → 34 dB |
| distortion | 10 → 42 dB |
| fuzz | 20 → 52 dB |
| **neuralCapture** | **−12 → +12 dB** |

The neural slot is the only kind whose drive control cannot actually drive anything.

**Change.** After T9 makes unity mean "the level the capture expects", widen the range so the top of
travel pushes meaningfully past it — roughly `-6 → +24 dB`, with Drive 5 landing at the calibrated
point. Verify against the corpus rather than picking the numbers blind: render a sweep of input
levels through several `.nam` pedal captures and find where each stops gaining harmonics.

**Honest limit, to state in the UI or docs.** A `.nam` pedal capture is a snapshot at one knob
position. If it was captured at low drive, no amount of input gain reproduces high-drive tone. T9
and T10 will fix "sounds like nothing"; they will not turn a low-drive capture into a high-drive
pedal. That is a format limit, not a defect.

**Done when.** A `.nam` pedal capture made at a high drive setting is audibly saturating at Drive 5
and clearly more so at Drive 10.

---

---

## Track F — Layered and panned sources

Commercial rhythm guitars are usually two takes, hard-panned. That breaks the match in a way none of
Tracks A–E addressed, and it raises a separate question about playback.

### T11a — Correct the dynamics seed for double-tracking — ✅ DONE

**A bug in T7, found by asking what happens when the reference is two guitars.** A summed pair
corrupts both features `dynamicsSeed` reads, in the same direction:

- two takes sum with their peaks landing at different moments, so the **crest factor of the sum is
  lower** than either take's;
- two pick attacks tens of milliseconds apart read as **one attack lasting the gap between them**.

Neither is a property of the amplifier. Uncorrected, a hard-panned rhythm part fits a rig with more
attack softening, less pick emphasis and more supply sag than the amp in the recording had — the
analyser measuring the arrangement, which is the same failure mode as the 1x-aliasing note and the
loudness-matcher note before it.

Meanwhile `doubleTrackingLikelihood` was **already computed**
([ToneAnalysis.cpp:318](../engine/tone-analysis/src/ToneAnalysis.cpp#L318)) and used only as a
*penalty* — `productionPenalty += doubleTrackingLikelihood * 0.12`
([SourceReconstruction.cpp:941](../engine/reconstruction/src/SourceReconstruction.cpp#L941)), which
scores every candidate down without changing the fit. The measurement that should have been
correcting the fit was only punishing it.

`dynamicsSeed` now takes the likelihood and adds back about 3 dB of crest factor and removes about
12 ms of apparent attack at full likelihood. **Those constants are the size of the effect in typical
double-tracked material, not derived quantities** — flagged as such in the code. Tested through the
public API and verified to fail both assertions when the correction is disabled.

### T11b — Analyse one side when the part is double-tracked — ✅ DONE

`recommendStereoMode(audio, doubleTrackingLikelihood)` added to the reconstruction API. Returns
`fullStereo` below a likelihood of 0.45, and otherwise the side with more **transient** peak energy —
first-difference energy, not total energy, because the axis this protects is the transient one and a
merely louder channel is not the one picked harder. `mid` is never recommended: summing two
hard-panned takes with micro-delays between them comb-filters, which is the failure it would be
chosen to avoid. Mono and empty input return `fullStereo` so callers can use it unconditionally.

Wired into `StudioServices::reconstructSongFile` as a **second analysis pass**: when the user left the
control at Full stereo — an explicit Left/Right/Mid/Side is a decision and is not second-guessed —
and the first analysis reports double-tracking, the region is re-extracted and re-analysed through the
recommended side. A second pass rather than a cheaper up-front estimate specifically to avoid a second
definition of the likelihood that could drift from the analyser's. Costs one analysis of a
few-second region against a separation pass orders of magnitude longer, and falls back silently to the
full-stereo analysis if the retry fails.

**Compile-verified, not link-verified**, for the same reason as T3: `nts_circuit` does not build at
HEAD, so the plug-in cannot be linked or run here. The decision function itself is covered by unit
tests in `ReconstructionTests`.

### T11c — Dual-rig stereo playback — ✅ DONE, with one deviation from the spec below

**Deviation: a separate `width` control, not a reinterpretation of `blend`.** The spec below proposed
making `blend` do double duty, "collapsing to today's summed behaviour at centre". That is wrong:
`blend` is not at centre in most stored presets, so redefining it would change the sound of every
preset ever saved. `CabinetParameters::width` is a new field defaulting to 0, where the stage is
bit-identical to what it did before — asserted by a test, not assumed.

Landed:

- **Routing.** Channel 0 takes slot A, channel 1 takes slot B, interpolated away from the existing
  sum by `width`. A mono configuration stays on the sum, which is the only thing it can be. No level
  compensation: at the default blend of 0.5 with two similar responses the sum already approximates
  either one, so the crossfade is close to level-matched, and with two deliberately different
  responses any correction would be guessing which one the user considers the reference.
- **Engagement.** `width` counts as weight on *both* sides in `updateEngagement` and `reset`.
  Without that, blend 0 skips and zeroes the second convolver, and asking for a stereo split would
  have produced silence on the right rather than cabinet B.
- **`monoCompatibility()`** — the reason this was worth specifying before writing. Measures the
  energy ratio between the actual mono sum and the mean channel energy, one-pole averaged over about
  a second, published as dB of level lost when folded. Measured rather than predicted, so it reports
  what the two loaded responses actually do to each other. Held rather than decayed when the signal
  is too quiet to measure — the same reasoning that should have applied to the loudness matcher.
- **Eco tier.** `width` is forced to 0 alongside `blend`, and the control is disabled with the
  caption saying so. It had to be: width keeps both convolvers engaged whatever the blend says, so
  leaving it up would reinstate the second convolution the tier exists to avoid.
- **Preset round-trip** as `cabinetWidth`, absent-defaults-to-0 so old presets read back unchanged.
- **Plug-in parameter and UI**: appended to the ordered id list, a slider on the cabinet page, and a
  mono-fold readout beneath it that turns amber past −6 dB with "check alignment".

Tests in `AmpTests::testCabinetStereoWidth`: inert at default (bit-identical channels, 0 dB fold),
channels genuinely differ at width 1 with two different responses while staying better than −3 dB
mono, and an inter-channel delay reports a materially worse fold than the undelayed split — which is
the hazard the meter exists for.

**Compile-verified, not link-verified or heard**, same as T3 and T11b: `nts_circuit` does not build
at HEAD. The engine-side behaviour is covered by tests; the slider's placement and the readout's
wording have not been seen on screen.

Two of the spec's open questions are now answered: width lives on the cabinet page, and Eco disables
it. The third — whether both sides should be independently matchable from a song, since the reference
genuinely contains two takes — is untouched and would be a new item.

### T11c — original specification

**What is not worth building, and why.** A true double-track cannot be synthesised from one
performance. Its width comes from decorrelation that is musically coherent: independent pick timing,
independent vibrato, independent string noise. A delayed or detuned copy of the *same* take is
correlated with itself, so summing it gives comb filtering or chorus, not a second player. Any
pitch-shift "doubler" is therefore the most new code — the DSP engine has a `PitchDetector` and no
shifter — for the least authenticity. Skip it.

**What is worth building.** Render one DI through two *different* amp and cabinet configurations and
pan them left and right. This is a real production technique — re-amping one take through two rigs —
and it decorrelates **spectrally** rather than temporally, so nothing combs.

Most of the machinery exists. `CabinetSection` already holds two independent impulse slots with
`blend`, `phaseInvertB` and `delaySamplesB`
([TraditionalAmp.h:144](../engine/amp/include/nts/amp/TraditionalAmp.h#L144)). Today `blend` **sums**
them into one signal. The work:

1. Route slot A to left and slot B to right, with `blend` becoming a width control that collapses to
   today's summed behaviour at centre — so existing presets are unchanged at their stored value.
2. Allow small per-side offsets on drive and the tone stack, which is what makes it read as two rigs
   rather than one rig through two cabinets.
3. **A mono-compatibility meter, which is the reason this is specified rather than written.**
   `delaySamplesB` between left and right is exactly the inter-channel micro-delay that Phase 1 of
   the original proposal identified as destructive under mono summing. Separate channels: wide.
   Summed to mono: comb-filtered. The feature needs to show the user what their mix bus will do to it,
   not just offer a knob.
4. Decide the interaction with `NeuralAmpProcessor::setForceMonoCollapse` and the mono-collapse
   detection, which currently assumes a duplicated mono DI and would defeat a dual-rig image.

Open questions for whoever picks this up: whether width belongs on the amp page or the cabinet page;
whether the two sides should be independently *matchable* from a song (the reference has two takes, so
in principle both could be fitted); and whether the Eco performance tier — which already forces a
single convolution — simply disables it.

## Verification

Per-item criteria are above. Across the plan:

- **Regression test for T2** — burst, silence past the gate close, second burst; assert the second
  burst's peak is within 1 dB of the first. This defect is subtle and will recur without a test.
- **Unit test for T4** — assert on `coarsePoint` output directly, no audio needed: for a pool of 12,
  the rendered set must contain a candidate at or above measured brightness and at or above measured
  tightness.
- **Grep check for T6** — no `DiAdaptation` field written without a reader.
- **Listening A/B** — Track A, Track B and Track C change the sound of every match. Land each track
  in its own commit against a fixed reference clip so the effect of each is attributable. `docs/`
  already documents a listening protocol in
  [phase-03-listening-protocol.md](phase-03-listening-protocol.md); reuse it.
- **Reconstruction timing** — T5 adds one render, T7 may need a larger pool. Measure against the
  existing baseline before and after.

## Out of scope, and why

An earlier proposal targeted the WaveNet side of the pipeline. Most of it does not apply to this
tree, and none of it addresses the defects above.

| Proposal | Why it is dropped |
|---|---|
| Snake / parametric activations, FiLM conditioning, receptive-field resizing | Nothing here trains a WaveNet. Trainable architectures are enumerated at [config.py:71](../ml/nts_ml/training/config.py#L71) and the torch backend trains LSTM only. WaveNet exists as a read-only NAM v0.7 reader used as a distillation teacher, plus `PackedWaveNetModel` for playback. Changing activations would fork away from NAM compatibility for no gain — imported captures have fixed weights. |
| Transient gain-up of separated target stems (+2–4 dB on attacks) | Gaining up a stem's attacks does not restore what masking removed; it teaches a model a transient response the source never had. The lever already exists and is nearly off: `transient_weighted = 0.05` in every config under [ml/configs](../ml/configs). |
| Stem sanitation for neural training | The primary config is `nam-distill-lstm.toml`; student targets are rendered by the teacher ([distill.py:23](../ml/nts_ml/nam/distill.py#L23)), not taken from separated stems. Stems feed the reconstruction path, which fits analog presets — the subject of this plan. |
| Curriculum learning with early layers frozen | [config.py:76](../ml/nts_ml/training/config.py#L76) enforces `layers == 1` for everything except TCN. There are no early layers to freeze. |
| Decoupling amp from cabinet IR | Impossible for NAM imports — the mic'd cabinet is part of the capture, as `measure_latency`'s comment in [distill.py](../ml/nts_ml/nam/distill.py) states. Structurally, `CabinetSection` lives inside `TraditionalAmp`, so the neural engine has no cabinet stage to decouple. Hoisting it is a real refactor touching latency reporting and preset schema; defensible later, not a fix for these symptoms. |
| Post-convolver harmonic exciter | Would sit in a part of the chain with **no oversampling**, so it would alias. Also duplicates two existing controls aimed at the same band: the presence high shelf at 2600 Hz ([TraditionalAmp.cpp:668](../engine/amp/src/TraditionalAmp.cpp#L668)) and `pickEmphasisDb` at 2800 Hz — the latter of which T7 puts to use. |
| Pre-inference gain trim | Already exists. `Param::input` is hoisted so all three engines share one trim ([PluginProcessorAudio.cpp:250](../Source/PluginProcessorAudio.cpp#L250)). |

Two items from that proposal survive independently of this plan and are worth their own tickets:

- **A-weighted ESR loss.** No A-weighting exists anywhere in the repo. If added, it must go into
  **both** [losses/audio.py](../ml/nts_ml/losses/audio.py) and `torch_backend.combined_loss` — they
  are mirrored term-for-term so a checkpoint is selected by the value that produced it.
- **Minimum-phase cabinet IR conversion.** No `minphase`/cepstral code exists. Cheap, offline,
  testable in `CabinetIrLoader`. Make it a load-time option rather than a silent transform, and note
  that converting both slots removes much of the reason `cabinetAlignment` exists.

A third correction worth recording: the premise that 8x oversampling rules out aliasing does not
hold here. Oversampling is wired only into the traditional amp's stages
([PluginProcessorAudio.cpp:615](../Source/PluginProcessorAudio.cpp#L615)); the neural engine runs at
host rate with no oversampler. Auto resolves to 4x, not 8x, and the anti-alias filter's own comment
records ~−30 dB of fold-back rejection at 4x
([Oversampling.h:16-26](../engine/dsp/include/nts/dsp/Oversampling.h#L16)). An aliasing probe on the
neural path is a separate, unstarted piece of work.
