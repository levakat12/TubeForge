# Cabinet plan: the cabinet becomes a stage, and the match fits it

An implementation-ready plan for two connected pieces of work:

1. **Make the cabinet a first-class part of the rig** — its own signal-chain stage rather than a
   sub-object of the traditional amplifier, with a control surface that covers what a cabinet
   actually is (speaker, microphone, position, two mics blended) instead of two file buttons.
2. **Route it into Song Match** — a cabinet is a *linear filter*, which is the one part of a guitar
   rig that can be fitted exactly rather than searched for. Today the reconstruction fits one
   cabinet field, from a descriptor that is literally `1 - brightness`, and then drops it on apply.

Line references are against the working tree at commit `6a4c276`; re-check them before editing.

## Status

| Track | Items | Effort | State |
|---|---|---|---|
| A — The cabinet becomes a stage | A1—A6 | 2.5 d | ✅ Complete |
| B — Parameters, project state, and the 64-slot ceiling | B1—B5 | 1.5 d | ✅ Complete |
| C — A built-in cabinet worth using | C1—C5 | 3.0 d | ✅ Complete |
| D — The page | D1—D7 | 2.5 d | ✅ Complete |
| E — IR handling, completed | E1—E6 | 2.0 d | ✅ Complete |
| F — Cabinet Match | F1—F7 | 3.0 d | ✅ Complete |
| G — Cost, tests, documentation | G1—G5 | 1.5 d | ✅ Complete |

**Coverage: 100% by effort** (16 of 16 days). Every planned item is built.

`ctest` is green: **21 of 21** suites, including the real VST3 scan/instantiate/process/editor run.
The two benchmark-acceptance tests are excluded from that count and pass in Release — they are
timing-budget tests that no Debug build meets, and they were failing that way before this work.

### What this does not claim

**Nobody has listened to any of it.** That is the honest headline and it has not changed since the
first day of this work. Everything that can be tested is: the null behaviour of every new control,
that the model's rendered impulse carries the curve it describes and is minimum-phase, that
minimum-phase conversion preserves magnitude and level while removing the leading delay, that the
true-stereo cross terms reach the channels they are supposed to, that the matcher recovers a
*known* filter, and that the library scans, sorts and persists as described.

Whether the eight cabinets sound like the enclosures they are named after is not a claim any test
makes, and it is the claim that matters most. Expect a pass of tuning by ear, and expect it to move
numbers in the two tables in `CabinetModel.cpp`.

**Cabinet Match fits a rig-against-record correction, not the cabinet in the room.** The
reconstruction has no separate DI — it renders each candidate through the reference itself and
says so in its own warnings — so the residual contains the amplifier's difference, the
microphone, the room and the mastering along with the speaker. The engine would become a true
cabinet fit unchanged if a DI capture path were added; only what is handed in would differ. The
page and both guides say this in as many words, and must keep doing so.

### What the shared cabinet costs, measured

Release, 48 kHz, stereo, as a percentage of the callback budget:

| Response length | One slot | Both slots | Slot B muted |
|---|---:|---:|---:|
| 512 taps (the model's default) | 0.25% | 0.39% | 0.25% |
| 1024 taps (the standard tier's ceiling) | 0.39% | 0.73% | 0.39% |
| 4096 taps (a long user response) | 0.10% | 0.11% | 0.10% |

The cost the neural and circuit engines newly carry — the price of making the cabinet global —
is **a quarter to four tenths of one percent** of the callback. Muting a slot returns its cost, and
the true-stereo path costs nothing at all when no true-stereo response is loaded: these figures are
unchanged to three decimal places from before that path existed.

**4096 taps is cheaper than 1024**: past `partitionedThresholdTaps` the section switches to the
partitioned FFT path, six times cheaper for four times the length. That is a standing invitation to
lower the threshold which has deliberately not been taken — the direct path is chosen for costing
*no latency*, and trading that for half a percent of one core is a decision about what this plug-in
is for rather than a constant to nudge. Recorded so whoever wants to make the trade can see it.

### What is built

- **The cabinet is a stage all three engines feed.** A preamp-only or pedal neural capture now has
  a speaker in front of it; a loaded impulse response now applies on the circuit engine. The
  circuit engine's own cabinet node is gone, and a full-rig capture switches the cabinet off as it
  loads because it already contains one.
- **Twenty-seven new parameters**, in a schema-6 block of their own rather than in `ampControlIds`.
- **A real built-in cabinet**: eight enclosures, five microphones, continuous position and
  distance, reconstructed as a minimum-phase impulse. `Legacy` keeps every project written before
  it sounding exactly as it did.
- **The page**: two slot strips, a response plot, automatic alignment, solo, per-slot level,
  placement, polarity, delay and mute, drag-and-drop, and the Auto Match badge and released-control
  marking the other pages carry.
- **An impulse-response browser** that scans a folder the user chooses rather than importing
  copies, with search, folders, favourites, recents, and load-on-selection.
- **Loaded responses handled properly**: length, normalisation mode, optional minimum-phase
  conversion, **four-channel true-stereo convolution**, export to a 24-bit WAV, and a content
  digest saved with the project so a response that changed on disk says so.
- **A cabinet can travel with a shared rig.** `.ntone` packages carry the responses when the user
  says they may — off by default, because most commercial responses are licensed for use rather
  than redistribution and the plug-in cannot read a licence.
- **Cabinet Match**: a residual estimator, a library ranking, a synthesised minimum-phase cabinet
  with a depth control, and a button that fits all three to the song the rig was matched from.
  `cabinetDarkness` is measured rather than being `1 - brightness`; `applyRecoveredRig` writes the
  cabinet it fitted; the controls it writes are in the Auto Match owned set; and the matched
  response itself is tracked as an asset that anything writing slot A releases.

### Defects found and fixed along the way

- **The bypass transparency test was under-settled.** `bypassMix` restarts its ramp every block, so
  it approaches unity geometrically rather than arriving; sixteen blocks left roughly a tenth of
  the wet signal mixed in. The test passed anyway because the residual was small against the old
  synthetic cabinet, and failed the moment the cabinet gained a real group delay — reading as a
  three-sample misalignment that was nothing of the kind. Now settled to a residual near 1e-4, and
  the assertion reports the best-fit offset so the next failure names its own cause.
- **`cabinetDarkness` was a rename, not a measurement.** It was literally `1 - brightness`: it fed
  `cabinet.highCutHz` in the reconstruction and occupied a slot in the tone embedding where it
  double-counted brightness against itself. Now derived from `highFrequencyRolloff` and
  `spectralSlopeDbPerOctave`, which is what separates a dark speaker from a dark amplifier.
- **A page override that would have hidden the Auto Match badge.** The drag-and-drop highlight
  overrode `paintOverChildren` without calling the base implementation, which silently removed the
  song-match veil and the Auto Match badge from the Cabinet page. Caught while building D6.
- **Four-channel responses were silently played as two.** The loader was asked for the output
  channel count, so the two cross terms of a true-stereo file were dropped on the floor.

---

## What exists today

Worth writing down plainly, because the request was "separate the cab sim into its own page" and
**the page already exists** — it is module 4 in the nav
([PluginEditor.cpp:257](../Source/PluginEditor.cpp#L257),
[moduleTable at :51-53](../Source/PluginEditor.cpp#L51)). What does *not* exist is a cabinet as a
separate **thing**. That distinction is the whole plan.

| Piece | Where | What it is |
|---|---|---|
| The DSP | `CabinetSection`, [TraditionalAmp.h:609-715](../engine/amp/include/nts/amp/TraditionalAmp.h#L609) | A member of `AmpVoice`. Two crossfading convolvers, a blend, a width split, a delay on B, a low cut and a high cut. Genuinely good code — the direct/partitioned switch, the idle-side skip and the measured mono-fold are all careful work. |
| The built-in responses | [TraditionalAmp.cpp:1329-1345](../engine/amp/src/TraditionalAmp.cpp#L1329) | Two 384-tap synthetic decays: `0.72·δ + 0.16·e^(-t/68)·sin(0.31t)` and a slightly darker sibling. Named "TubeForge 4x12 Edge" and "TubeForge 2x12 Center". They are not cabinet measurements and do not claim to be. |
| File loading | [PluginProcessorAssets.cpp:82-225](../Source/PluginProcessorAssets.cpp#L82) | Async decode, resample to the session rate, DC removal, trim, peak normalise, tier-dependent shortening with a fade, crossfaded swap. Solid. |
| The page | [CabinetPage.cpp](../Source/ui/CabinetPage.cpp) | 168 lines. Two Load/Clear button pairs, four controls, a mono-fold readout and a help paragraph. |
| Parameters | [PluginProcessorAudio.cpp:652, :700-710](../Source/PluginProcessorAudio.cpp#L700) | Four: `cabinet` (on/off), `cabinetBlend`, `cabinetWidth`, `cabinetAlignment`. |
| In the match | [SourceReconstruction.cpp:501-502](../engine/reconstruction/src/SourceReconstruction.cpp#L501) | Two lines: `cabinet.highCutHz` from `cabinetDarkness`, `cabinet.blend` from the gain offset. |
| In Auto Match | [AutoMatch.h:107-112](../Source/AutoMatch.h#L107) | Owns the four parameters, and states in a comment that the impulse responses themselves cannot be owned. |

---

## What is broken or missing today

Each of these is a specific, checkable defect, not a wish.

### F1 — The cabinet only exists on one of three engines

`CabinetSection` is a member of `AmpVoice`. The processor dispatches to one of three engines
([PluginProcessorAudio.cpp:327-365](../Source/PluginProcessorAudio.cpp#L327)):

- **Traditional** — has the IR cabinet.
- **Neural capture** — has no cabinet at all. A NAM capture of a *preamp* or a *pedal* — which is
  most of what people share, and exactly what `CaptureLibrary::isMismatch` sorts for
  ([CaptureLibrary.cpp:93-99](../engine/nam/src/CaptureLibrary.cpp#L93)) — plays into the output
  with no speaker in front of it. That is a fizzing, unusable sound, and the plug-in offers no way
  to put a cabinet after it.
- **Physical circuit** — has a *different* cabinet: a graph node with a `resonance`/`brightness`
  pair and three fixed model ids ([PluginProcessorAssets.cpp:679-686](../Source/PluginProcessorAssets.cpp#L679)).
  A user's loaded IR does nothing on this engine.

So "Cabinet" as a page is already a lie about the signal chain: it is the traditional engine's
cabinet page. **This is the single most valuable thing in this plan** — more than any control
added below.

### F2 — Four of the cabinet's own settings have no control

`CabinetParameters` ([TraditionalAmp.h:291-321](../engine/amp/include/nts/amp/TraditionalAmp.h#L291))
has eight fields. Four are reachable:

| Field | Reachable? |
|---|---|
| `blend`, `width`, `delaySamplesB`, `bypass` | Yes |
| `lowCutHz` (70 Hz), `highCutHz` (10.5 kHz) | **No** — set by the voicing, invisible, unchangeable |
| `phaseInvertB` | **No** — and this is the standard first move when blending two mics |
| `bassDiBlend` | **No** — a bass DI blend with no control on it |

`highCutHz` is the one the reconstruction fits. It fits a value the user cannot see, cannot audit,
and which `applyRecoveredRig` then **cannot write**, because there is no parameter for it — this is
row 5 of the auto-match plan's own F4 table
([auto-match-plan.md](auto-match-plan.md#f4--a-large-part-of-a-fitted-rig-has-no-host-parameter-at-all)).
The match's only cabinet fit is silently discarded on apply.

### F3 — There is no per-slot anything

Two responses are summed by one crossfade. There is no per-slot level, pan, mute, solo, phase,
delay or filtering. Every IR loader people compare this to — [mixIR³](https://redwirez.com/products/mixir3-ir-loader),
[Cabinetron](https://www.threebodytech.com/en/products/cabinetron), Trve Cab — gives each loaded
response at minimum a gain, a pan, a mute and a phase button, because *that is what a mic blend is*.
Two mics on a cabinet at different distances are also at different times, and the only alignment
control here is a single 0–256-sample delay on B, driven by a slider, with no measurement behind it.

### F4 — The built-in cabinets are placeholders, and they are the default

Blend defaults to 0.5, so the shipping sound of the plug-in is `0.5·(A + B)` of two hand-written
exponential-sine decays. A player who never loads an IR — which is most players, most of the time —
is hearing a stand-in. And because the reconstruction renders every candidate through these
([SourceReconstruction.cpp:1038-1041](../engine/reconstruction/src/SourceReconstruction.cpp#L1038)),
**every amp match ever produced was scored through them too.**

### F5 — A project remembers a path, not a cabinet

`AssetState::cabinetIrPathA/B` ([ProjectState.h:75-86](../engine/state/include/nts/state/ProjectState.h#L75))
stores absolute paths. `restoreCabinetIrPaths` handles the missing-file case carefully — falls back
to the built-in, keeps the path, shows `Missing:`
([PluginProcessorAssets.cpp:176-198](../Source/PluginProcessorAssets.cpp#L176)) — which is the right
behaviour for a defect that should not exist. A `.tforge` project or a `.ntone` profile sent to
another machine, or opened after an IR folder is reorganised, does not sound like the rig that was
saved. The tone-package format already knows how to carry assets
([TonePackage.h](../engine/ecosystem/include/nts/ecosystem/TonePackage.h)); the cabinet does not use it.

### F6 — The one cabinet field the match fits is derived, not measured

`report.cabinetDarkness.value = 1.0f - report.brightness.value`
([ToneAnalysis.cpp:347](../engine/tone-analysis/src/ToneAnalysis.cpp#L347)). It is a rename of
brightness. It carries no independent information about the speaker, and it feeds both
`cabinet.highCutHz` and a slot in the tone embedding
([ToneAnalysis.cpp:410](../engine/tone-analysis/src/ToneAnalysis.cpp#L410)), where it double-counts
brightness against itself.

### F7 — The amp search is asked to do the cabinet's job

The reconstruction searches four continuous axes and a topology across up to thirteen voicings.
Cabinet colouration lands on the same measurement the search reads as *brightness*, so a dark
reference pushes the fit towards a darker **amp** — treble down, presence down, `postHighDb` down —
when what was dark was the speaker. The plan's own comment at
[SourceReconstruction.cpp:368-372](../engine/reconstruction/src/SourceReconstruction.cpp#L368)
documents an earlier version of exactly this bias in the coarse grid.

A cabinet is linear and time-invariant. Fitting one needs **one FFT pass, not a search**. Spending
pool slots on it is spending the expensive tool on the problem that has a cheap exact answer.

---

## Decisions, and why

### D1 — The cabinet moves out of `AmpVoice` and becomes a stage the processor owns

Placed after the engine crossfader and before the sends, so all three engines feed it. This is what
makes "its own page" true rather than cosmetic, and it is what makes a preamp-only NAM capture
usable.

`AmpVoice` keeps a `CabinetSection` **for offline rendering only** — the reconstruction's
`renderOffline` path ([SourceReconstruction.cpp:1041](../engine/reconstruction/src/SourceReconstruction.cpp#L1041))
needs a cabinet in the render or every candidate is scored on a fizzing preamp, and giving it the
global stage would mean handing a live audio object to a worker thread. One class, two instances,
one of which is explicitly a measurement instrument (see D6).

**Cost, stated up front:** `AmpParameters::cabinet` is carried by every `.ntone` profile, every tone
package and every voicing preset. Those values must be *routed* to the new stage's parameters on
load rather than dropped. That is item A5 and it is the highest-risk item in Track A.

### D2 — The circuit engine's cabinet node is removed; the neural engine gains nothing to remove

Two cabinets in series is not a feature. With D1 in place, `currentCircuitGraph` stops synthesising
a cabinet node ([PluginProcessorAssets.cpp:679-695](../Source/PluginProcessorAssets.cpp#L679)) and
the three `cabinet.*.v1` model ids move to the built-in model table in Track C as three of its
presets, so nothing that could be selected before disappears. `circuitCabinetStyle` stays as a
parameter id — removing it would shift `ampControlIds` and corrupt saved projects — and becomes
inert with a note, the same way parameters are retired elsewhere in this codebase.

### D3 — The built-in cabinet becomes a model, not a stand-in

A parametric speaker+mic model rendered to an impulse on the message thread, then loaded through the
existing crossfade path. Not a sampled IR library: shipping third-party IRs is a licensing question
this project does not need, and a *model* gives continuous mic position and distance, which sampled
IRs cannot. It also gives Track F something to search over.

Guitar speakers are minimum-phase to a good approximation, which is what makes a synthesised
magnitude response a legitimate cabinet rather than an EQ curve wearing the name — this is the same
property the [Fractal wiki](https://wiki.fractalaudio.com/wiki/index.php?title=Impulse_responses_%28IR%29)
leans on to justify minimum-phase IR conversion.

### D4 — Cabinet parameters get their own state block, not six more `ampControlIds` slots

`ampControlIds` holds **58 of the 64** entries `ProjectState::validate` accepts
([PluginProcessorInternal.h:148-209](../Source/PluginProcessorInternal.h#L148),
[ProjectState.h:32](../engine/state/include/nts/state/ProjectState.h#L32)). Six free slots. This
plan needs roughly fifteen new parameters. Overrunning that list does not truncate a save — it makes
**every saved project fail to load**, and the comment at that list documents a positional-shift bug
that already shipped once.

So: a new `CabinetState` block at schema 6, with its own positional list and its own ceiling,
defaulting to empty so every schema-5 project loads unchanged. The cabinet's parameters do not go
near `ampControlIds`. This also keeps the door open for the pedal and effects blocks, which will hit
the same ceiling next.

### D5 — Auto Match can own a cabinet, which means owning an asset and not only a parameter

The current owned set is 47 of a hard 64-bit ceiling ([AutoMatch.h:122-129](../Source/AutoMatch.h#L122)),
and its comment states plainly that the IRs cannot be owned because they are not parameters. The
answer is not to widen the guard: it is to make *which cabinet* be a parameter (`cabinetSourceA/B`
— built-in model / user file / matched) plus a stored asset the applied snapshot carries. The
parameter is guarded by the existing listener; the asset is guarded by one rule — **loading a file
into a slot Auto Match filled releases that slot** — reported on the page in the same language the
released-parameter marking already uses.

### D6 — The candidate render gets a fixed, documented measurement cabinet

Track F fits the cabinet from the residual between the winning candidate's render and the reference.
That subtraction is only meaningful if the render's own cabinet is *known*. So `renderOffline` takes
an explicit measurement cabinet — the built-in model at a neutral setting, pinned by a test — rather
than "whatever `makeDefaultCabinetImpulse` currently returns". Changing it later then changes a
number a test is watching, instead of silently re-voicing every match.

### D7 — Cabinet Match produces a *cabinet*, not an EQ curve, and says which it is

Two outputs from the same residual, and the user picks:

- **Pick from the library** — score every model and user IR by how much of the residual it removes.
  The result is a real cabinet, with a name, that generalises to other songs.
- **Synthesise a matched response** — a minimum-phase FIR built from the smoothed residual, which
  fits better and generalises worse, because it also absorbs the reference's microphone, room, bus
  compression and mastering EQ. This is what [Fractal's Tone Match](https://wiki.fractalaudio.com/wiki/index.php?title=Tone_Match_block)
  does, and the honest description of it is "the difference between your rig and that record",
  not "that cabinet".

Both are offered. The synthesised one carries a depth control and a warning. Presenting the second
as if it were the first is the failure mode to avoid.

### D8 — A warning, not a lock, and no continuous re-fitting

Same as the rest of the analyzer surface: the veil warns rather than disables
([ModulePage.h:39-50](../Source/ui/ModulePage.h#L39)), and Auto Match re-derives on discrete events
only ([auto-match-plan.md, D2](auto-match-plan.md#d2--it-re-derives-on-events-never-continuously)).
Cabinet Match is cheap enough to re-run continuously and will not: a cabinet changing under a player
mid-take is alarming for the same reason a knob moving on its own is.

---

## Design

### The chain, after

```
input ─▶ tuner tap ─▶ pedals ─▶ [ traditional | neural | circuit ] ─▶ CABINET ─▶ delay ─▶ reverb ─▶ output
                                        engine crossfader              │
                                                                       ├─ slot A ─┐
                                                                       │          ├─ blend ─ width ─ low/high cut ─ DI blend
                                                                       └─ slot B ─┘
```

Each slot is `{ source, level, pan, phase, delay, mute, solo }` where `source` is one of:

| Source | Backed by |
|---|---|
| **Model** | Track C's parametric render: cab × speaker × mic × position × distance |
| **File** | A user IR, through the existing `CabinetIrLoader` path |
| **Matched** | Track F's synthesised response, or a library pick, held as a project asset |

### Parameters

New, all in the `CabinetState` block of D4 — none in `ampControlIds`:

| Group | Parameters |
|---|---|
| Slot A | `cabSourceA`, `cabModelA`, `cabMicA`, `cabPositionA`, `cabDistanceA`, `cabLevelA`, `cabPanA`, `cabPhaseA`, `cabDelayA`, `cabMuteA` |
| Slot B | the same ten, `…B` |
| Section | `cabLowCut`, `cabHighCut`, `cabDiBlend`, `cabOutputTrim`, `cabMatchDepth` |

Existing and unchanged in meaning, position and default: `cabinet`, `cabinetBlend`, `cabinetWidth`,
`cabinetAlignment`. `cabinetAlignment` stays the B-relative delay it always was; `cabDelayA/B` are
per-slot trims on top of it, defaulting to zero, so no existing project moves.

**Backward compatibility rule for the whole track: with every new parameter at its default, the
rendered output must be sample-identical to today's.** That is a test, not an aspiration (G3).

### Cabinet Match, concretely

Runs *after* a candidate is applied — on the final rig, so the residual is genuinely what remains
rather than something the post-EQ trio is about to fit again.

```
1.  Render the applied rig through the user's DI            → r(t)         [reuses renderOffline]
2.  Long-term average log-magnitude spectra, 1/6-octave smoothed:
        S(f) = reference,  R(f) = render                    [ToneAnalysis already computes both]
3.  D(f) = S(f) - R(f)   in dB, over 70 Hz - 8 kHz only
        below 70 Hz : held flat  (bass and kick leakage live there)
        above 8 kHz : held flat  (cymbal leakage, and no speaker has content to fit)
        clamped to +-12 dB       (a cabinet is not a 30 dB filter; that would be a mix decision)
4a. LIBRARY PICK:  for each candidate cabinet C, score  ||D(f) - (C(f) - M(f))||  weighted by
        an equal-loudness curve, where M(f) is the measurement cabinet of D6.
        Return the best three, with scores, as choosable slots.
4b. SYNTHESISED:   minimum-phase FIR from D(f) via  real cepstrum -> causal fold -> inverse FFT,
        1024 taps at 48 kHz, scaled by cabMatchDepth (0-100%, default 70%).
5.  Both go into slot A as source = Matched, with the previous contents recoverable.
```

Step 4b's minimum-phase construction is the standard one and is the reason this is legitimate: a
speaker is approximately minimum-phase, so a magnitude fit with minimum-phase reconstruction
produces a response with a plausible time structure rather than a smeared, pre-ringing linear-phase
filter.

**What this cannot do, stated so the UI can say it:** the residual is a *long-term average
spectrum*. It carries no information about the reference's transient behaviour, its nonlinearity, or
anything that differs between the DI performance and the recorded one. It will absorb the record's
mic, room, bus compression and mastering EQ into something labelled "cabinet". That is why D7 offers
the library pick first and why the depth control exists.

---

## Track A — The cabinet becomes a stage

✅ **A1 — Lift `CabinetSection` to a stage the processor owns.** New member beside the engines, prepared
in `prepareToPlay`, processed after the engine crossfade and before `delayEffect`. Its `tailSamples`
and `latencySamples` join the reporting at
[PluginProcessorAudio.cpp:490-501](../Source/PluginProcessorAudio.cpp#L490) and :747-753.
*Done when:* a preamp-only NAM capture plays through a loaded IR.

✅ **A2 — Bypass the amp's internal cabinet on the live path.** `AmpVoice` keeps its instance for
offline render only; live processing sets `cabinet.bypass = true` unconditionally.
*Done when:* a null test proves the traditional engine's live output is unchanged with the global
stage carrying the same settings.

✅ **A3 — Remove the circuit engine's cabinet node** (D2), and retire `circuitCabinetStyle` in place
with a comment, not by deletion.

✅ **A4 — Per-slot processing.** Level, pan, mute/solo, phase and delay, applied inside
`CabinetSection::process` ([TraditionalAmp.cpp:1511-1586](../engine/amp/src/TraditionalAmp.cpp#L1511)).
The existing loop already carries a per-channel delay line and a phase flip on B — this generalises
both to per-slot rather than adding a stage. *Risk:* that loop is the hot path; the idle-slot skip
(`firstEngaged`/`secondEngaged`) must account for mute and for a level at −∞, or muting a slot still
pays for its convolution.

✅ **A5 — The preset bridge.** `AmpParameters::cabinet` arriving from a voicing, a `.ntone` profile or
a tone package writes the new stage's parameters. *Risk:* the highest in this track — get it wrong
and loading any existing profile changes the cabinet silently. *Done when:* loading each of the
thirteen built-in voicings reproduces today's cabinet settings field by field.

✅ **A6 — Tier behaviour.** `TierLimits::singleCabinet` currently pins blend to A and zeroes width
([PluginProcessorAudio.cpp:702-710](../Source/PluginProcessorAudio.cpp#L702)). Extend to force slot
B muted, so the page's new controls read as unavailable for a stated reason rather than being
ignored — the pattern `CabinetPage::setPerformanceLimits` already follows
([CabinetPage.cpp:70-84](../Source/ui/CabinetPage.cpp#L70)).

## Track B — Parameters, project state, and the 64-slot ceiling

✅ **B1 — `CabinetState` in `ProjectState` at schema 6**, with its own positional id list, its own
`maximumCabinetControls`, and the append-only warning copied verbatim from
[PluginProcessorInternal.h:171-183](../Source/PluginProcessorInternal.h#L171). Default-empty, so a
schema-5 project loads with today's four parameters and every new one at its default.

✅ **B2 — The thirteen new parameters** of the Design table, registered in the layout and in the new
list. `static_assert` on the count, as `ampControlIds` does.

✅ **B3 — Expose `lowCutHz`, `highCutHz`, `phaseInvert` and `bassDiBlend`** (F2). These are the four
that already exist in the DSP and have no control.

✅ **B4 — Cabinet assets in the project.** `CabinetState` carries, per slot, either a model
description, a path *plus a content hash*, or an embedded matched response. Path-plus-hash is the
minimum honest fix for F5: a project can then say "this is not the file you saved" rather than only
"this file is gone". Embedding the matched response is not optional — it is generated, so there is no
file to point at.

✅ **B5 — `.ntone` and tone-package carriage.** A shared rig with a cabinet must arrive with it.
[TonePackage](../engine/ecosystem/include/nts/ecosystem/TonePackage.h) already validates assets;
add the IR as an asset kind rather than inventing a second mechanism.

## Track C — A built-in cabinet worth using

✅ **C1 — The model.** Rendered impulse from: speaker low-frequency resonance (a resonant peak plus its
roll-off), cone breakup (two to three peaked bands in 1.5–4 kHz), high-frequency roll-off from cone
mass, baffle/cab type (closed, open-back, ported, sealed bass), microphone response curve, on-axis
to off-axis position, and distance (proximity boost plus a first reflection). Rendered on the message
thread into ≤1024 taps and handed to the existing crossfaded loader — the audio thread learns nothing
new.

✅ **C2 — The table.** Cabs: 4x12 closed, 2x12 open, 1x12 combo, 4x10 bass, 8x10 bass sealed, plus the
three the circuit engine loses in A3. Mics: dynamic 57-alike, dynamic 421-alike, ribbon 121-alike,
condenser 87-alike, plus a room pair. Position: edge-to-cap, continuous. Distance: 1 inch to 24
inches, continuous. This is the axis set every comparable product exposes and players already know.

⬜ **C3 — Faces.** The project draws its own gear ([FaceplateArt](../Source/ui/FaceplateArt.h),
[PedalArt](../Source/ui/PedalArt.h)) and the picker takes a `paintFace` callback
([GearPicker.h:17-28](../Source/ui/GearPicker.h#L17)). Cabinets get drawn grilles the same way, so
the picker works exactly as the amp and pedal ones do.

⬜ **C4 — The measurement cabinet** (D6): one pinned configuration, used by `renderOffline`, with a
test asserting its response has not moved.

✅ **C5 — Defaults change, and that is a re-voicing.** Replacing `makeDefaultCabinetImpulse` changes
the sound of **every existing project that never loaded an IR**. The options are to keep the old
impulses as a "Legacy" model that old projects select on load, or to accept the change. *Recommended:*
keep them, select them for schema ≤5 projects, and make the new model the default for new ones. A
plug-in that re-voices somebody's finished track on update is the thing the bass-path plan already
refused to do ([TraditionalAmp.h:369-385](../engine/amp/include/nts/amp/TraditionalAmp.h#L369)).

## Track D — The page

✅ **D1 — Two slot strips**, each with its source chip (Model / File / Matched), the model or file name,
and the per-slot controls of A4. The `GearChip` + `GearPicker` pair is the established pattern.

✅ **D2 — The section controls:** blend, width, alignment, low cut, high cut, DI blend, output trim,
and the mono-fold meter, which stays where it is and keeps its warning behaviour
([CabinetPage.cpp:139-152](../Source/ui/CabinetPage.cpp#L139)).

✅ **D3 — A response plot.** A, B and the sum, plus the mono-fold curve. The Circuit page already
draws a computed response and is the model to copy. This is the control that makes every other one
on the page legible.

✅ **D4 — Auto-align.** One button: cross-correlate A against B, write the lag into `cabinetAlignment`.
The slider stays for people who want the comb filter on purpose. This is the measurement the
alignment control has always lacked.

✅ **D5 — A/B compare and per-slot solo**, so a mic blend can be judged rather than guessed at.

✅ **D6 — The Auto Match badge and released-slot marking**, matching what the Amplifier and Tone
Shaping pages already do (D2/D3 of the auto-match plan).

✅ **D7 — Drag and drop** of `.wav`/`.aiff`/`.flac` onto a slot.

## Track E — IR handling, completed

✅ **E1 — A browser.** Folders, recents, favourites, search — the `CapturePicker`/`GearPicker` pattern
over a scanned IR directory. Two file-chooser buttons is not a way to work with a library of four
hundred IRs.

✅ **E2 — Length and resolution control.** 128 / 256 / 512 / 1024 / 2048 / 4096 taps with the existing
fade-out shortening ([PluginProcessorAssets.cpp:141-163](../Source/PluginProcessorAssets.cpp#L141)),
exposed rather than tier-driven only. Shorter is cheaper *and* a legitimate tonal choice: past the
first few milliseconds an IR is mostly room.

✅ **E3 — Minimum-phase conversion, optional per slot.** Removes the pre-delay and the phase difference
that makes two IRs from different vendors comb when blended. Off by default.

✅ **E4 — Normalisation modes:** peak (today), RMS, or none. Peak-normalising a room-heavy IR against a
close-mic one makes the blend control lie about what it is doing.

✅ **E5 — True-stereo handling.** The loader already decodes multichannel
([CabinetIrLoader.h](../engine/ir/include/nts/ir/CabinetIrLoader.h)); state explicitly what a
4-channel true-stereo IR does — currently it silently takes the first two channels.

✅ **E6 — Export.** Write slot A, slot B, or the summed/matched response as a `.wav`. Required by
Track F to be useful outside the plug-in, and it is four lines once the response is in hand.

## Track F — Cabinet Match

✅ **F1 — Stop double-counting `cabinetDarkness`.** Either derive it from a real measurement — the
spectral slope above 2 kHz relative to the 200 Hz–1 kHz band is the obvious candidate and the
features are already computed — or remove it from the embedding, where it currently duplicates
brightness ([ToneAnalysis.cpp:347, :410](../engine/tone-analysis/src/ToneAnalysis.cpp#L347)).
Removing it from the embedding changes every stored profile's vector, so it is a database version
bump; deriving it properly is the better answer.

✅ **F2 — The residual estimator**, per the Design section. Pure function over two spectra, no audio
thread involvement, unit-testable against synthetic inputs where the answer is known.

✅ **F3 — The library pick.** Score every model configuration and every scanned user IR. Cheap: each is
one FFT of a ≤1024-tap response, done once and cached.

✅ **F4 — The synthesised response**, with `cabMatchDepth` and the disclosure of D7.

✅ **F5 — Wire it into Song Match.** Runs after apply, on a worker thread, reporting through the
existing progress channel. Results land in the Cabinet page's Matched source and are listed on the
Song Match page beside the candidate shortlist.

✅ **F6 — Auto Match ownership** per D5: `cabSourceA/B` and the model/match selection join the owned
set (47 → ~53, still under the 64-bit ceiling), and loading a file into an owned slot releases it
with a message, exactly as an external parameter write does today
([auto-match-plan.md, C4](auto-match-plan.md#track-c--the-guard-and-the-warning-window)).

✅ **F7 — `applyRecoveredRig` writes the cabinet.** With B3's parameters in place, the fitted
`highCutHz` stops being discarded, and the F4 disclosure list in the warnings panel loses its
cabinet row because the value is now actually applied.

## Track G — Cost, tests, documentation

✅ **G1 — Cost.** Two convolutions were already the traditional engine's most expensive operation
([TraditionalAmp.h:698-711](../engine/amp/include/nts/amp/TraditionalAmp.h#L698)). Making the
cabinet global means the neural and circuit engines now pay for it too. Measure, and make sure the
idle-slot skip covers mute and −∞ level (A4).

✅ **G2 — Null test:** every new parameter at its default reproduces today's output sample for sample,
on each of the thirteen voicings. This is the guard for the whole plan.

✅ **G3 — Schema 6 round trip**, and a schema-5 project loading with an empty `CabinetState`.

✅ **G4 — Cabinet Match tests:** feed the estimator a render and the same render through a *known*
filter; assert the recovered response matches the known one within the smoothing bandwidth. This is
the one part of the feature with an exact right answer, so it should be tested against it.

✅ **G5 — Documentation:** a user-guide section on the page and on what a matched cabinet is and is not,
and a developer-guide note on the new stage's position in the chain and on `CabinetState`'s
append-only rule.

---

## Out of scope, and why

- **Shipping sampled IRs.** Licensing, and a model does the job better for Track F.
- **More than two slots.** mixIR³ stacks unlimited responses; two covers the mic blend that is the
  actual use, and each additional slot is another convolution on the hot path.
- **A cabinet for the pedal chain.** The cabinet goes after the engine. A cab-before-amp routing is
  a different feature.
- **Fitting the reference's room or mic separately from its speaker.** Not separable from one
  long-term spectrum. The residual is one filter and is presented as one.
- **Continuous cabinet re-matching while playing.** See D8.
- **Any claim that a matched cabinet sounds like the record.** The testable claim is that the
  applied response reduces the measured spectral residual by a stated amount. That is what G4 checks
  and it is all this plan claims.

## Open questions

1. **C5 — legacy default.** Keep the old synthetic impulses as a selectable "Legacy" model so
   existing projects sound unchanged, or accept a one-time re-voicing on update? Recommended: keep.
2. **D2 — the circuit engine's cabinet.** Remove it in favour of the global stage as proposed, or
   leave it and let the circuit engine be the one engine with its own speaker?
3. **F1 — `cabinetDarkness`.** Derive it properly (better, and a profile-database version bump), or
   leave it derived and let the residual estimator carry the whole cabinet fit?
4. **D7 — default match mode.** Library pick or synthesised response as the first thing offered?
   Recommended: library pick, with the synthesised one one click away.
5. **Scope order.** A + B + D alone is about six days and already delivers "a real cabinet page that
   works on all three engines". Is that the first milestone, or should C land with it so the page
   has something worth showing?

---

Sources consulted for the feature survey:
[mixIR³](https://redwirez.com/products/mixir3-ir-loader) ·
[Cabinetron](https://www.threebodytech.com/en/products/cabinetron) ·
[APG guide to IR loaders](https://www.audiopluginguy.com/apg-guide-to-guitar-cabinet-ir-loaders/) ·
[TONE3000 on impulse responses](https://www.tone3000.com/guides/impulse-responses) ·
[Fractal wiki: impulse responses](https://wiki.fractalaudio.com/wiki/index.php?title=Impulse_responses_%28IR%29) ·
[Fractal wiki: Tone Match block](https://wiki.fractalaudio.com/wiki/index.php?title=Tone_Match_block)
