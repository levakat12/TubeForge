# Auto Match plan: the analyzer holds the rig, and warns before you fight it

An implementation-ready plan for **Auto Match** — a mode in which the song/tone analyzer owns every
amplifier control (Gain, Bass, Mid, Treble, Presence, Resonance, Master, and the whole Tone Shaping
page), sets them all from the analysis, keeps them consistent, and puts a warning window in front of
any manual edit that the analysis has already answered.

Two halves, and they are not equally hard:

1. **Set everything from the analysis.** Most of this exists — `RigReconstructor` already fits a full
   `AmpParameters` and `applyRecoveredRig` already writes about thirty host parameters. What is
   missing is that the apply path is *incomplete* and *unrecorded*: three controls it never touches
   silently re-voice the rig it just applied, and nothing remembers what it wrote.
2. **Warn when the user reaches for an owned knob.** This is new, and almost all of the risk is here:
   a modal dialog raised from the wrong thread, or raised by host automation, turns a plug-in into
   something a DAW cannot run.

Line references are against the working tree at commit `6a4c276`; re-check them before editing.

## Status

| Track | Items | Effort | State |
|---|---|---|---|
| A — Make an applied rig complete and reproducible | A1–A5 | 1.0 d | ✅ Complete |
| B — Ownership model in the processor | B1–B4 | 1.0 d | ✅ Complete |
| C — The edit guard and the warning window | C1–C5 | 1.5 d | ✅ Complete |
| D — Interface: the Auto switch and the owned-control marking | D1–D4 | 1.0 d | ✅ Complete |
| E — Re-derivation, and what counts as an event | E1–E3 | 1.0 d | ✅ Complete |
| F — Tests and documentation | F1–F4 | 1.0 d | ✅ Complete |

**Coverage: 100% by effort** (6.5 of 6.5 days). Every planned item is built. `ctest` is green: **23 of 23
suites**, including the real VST3 scan/instantiate/process/editor run; the VST3 and the standalone both build.
Auto Match carries seventy assertions of its own across three new wrapper tests plus an extended editor
lifecycle test, which is about a hundred and ten checks at run time because the recovered-rig parity test
loops a forty-row table.

### What this does not claim

**Nobody has judged how any of it feels.** The editor is now built, ticked at its real 20 Hz rate and painted
while a rig is held with one control overridden, so the badge, the released-control marking and the warning
poll all execute and render under real state — but "does the automatic apply read as helpful or as the
plug-in taking over" is a question no test answers. That is the remaining risk and it is a listening-and-
looking one, not a correctness one.

Two smaller things worth knowing:

- **Live tracking has been exercised only at its boundaries.** The test proves it moves nothing without a
  measured signal, and the bounds are 6 dB by construction. What has not been watched is a trim following a
  real guitar change over a minute of playing.
- **The re-derivation path is tested through `applyAutoMatchRig`**, not through a completed reconstruction.
  The override-preservation logic is asserted directly; the generation plumbing that calls it is asserted
  separately. A single end-to-end run — import a song, watch it apply itself — has not been done, because it
  needs a song and takes minutes.

### Deviations from the plan as written

- **The dialog and the automatic apply are polled, not pushed.** The plan said `AsyncUpdater`; the
  editor's existing 20 Hz tick does the same job with less machinery, at the cost of up to 50 ms.
- **Held controls are not tinted; released ones are.** In Auto Match nearly every control on a page is
  held, so marking them all would repaint the page in one colour and say nothing. The exception is
  what carries information, so a released control goes green and the badge states the rest are held.
- **Auto Match applies the winning candidate itself.** The plan left "apply" as the user's press; that
  turned out to be the wrong reading of the feature — a mode called Auto that waits to be told to
  apply is not automatic. Pressing Apply still works and still means "this candidate, now".
- **"Don't warn me again" is permanent, not per session,** and therefore has a visible way back: a
  **Warnings: off** button appears beside Auto Match once it is set. A preference with no way to undo it
  is a trap rather than a choice. It is stored in its own small file rather than in the assistant's
  preferences, so clearing the assistant's personalization does not silently turn a warning back on.
- **A project or profile recall restores or clears the hold** rather than being warned about. Those
  paths replace the whole rig, so the recorded snapshot no longer describes what is playing.
- **An accepted Tone Assistant action releases the controls it moves.** It is the user's decision
  arriving through a button rather than a knob, so it neither raises the dialog nor gets overwritten.
- **`applyRecoveredRig` and `applyAutoMatchRig` are public.** Every guarantee Track A adds, and the one
  behavioural difference between pressing Apply and a re-run, are only checkable by calling them.
- **`AutoMatchState::isolatedChain` is stored, not derived.** A pedal bypass reads the same whether the
  match set it or the player did, so inferring it would sometimes claim a matched rig owned a board it
  never touched.
- **Live tracking is open-loop by construction.** It corrects from the assistant's pre-trim measurement,
  so raising the trim cannot raise the measurement that asked for it. A closed loop would run away or
  hunt, and neither is something a player should have to diagnose.

---

## Decisions, and why

### D1 — Auto Match is an ownership state, not a fourth engine

`engineMode` already selects between Traditional, Neural capture and Physical circuit
([PluginProcessorHost.cpp:319](../Source/PluginProcessorHost.cpp#L319)). Auto Match is not a fourth
one: it does not change how audio is made, it changes **who writes the parameters**. Implemented as
an engine mode it would have to duplicate the traditional path; implemented as ownership it is a
flag, a recorded snapshot and a guard, and every existing engine, preset and project keeps working.

### D2 — It re-derives on events, never continuously

`RigReconstructor::reconstruct` renders `poolSize + refinementSteps * refinementAxes * 2` candidates
plus a topology re-check across up to thirteen voicings
([SourceReconstruction.cpp:978-1135](../engine/reconstruction/src/SourceReconstruction.cpp#L978)) —
seconds to minutes on a worker thread. A mode that re-fitted the rig while the user plays is not a
tuning problem, it is architecturally impossible on this search.

So Auto Match re-derives on **discrete events**: a new candidate applied, a new region chosen, the
instrument changed, a re-analysis finishing. Between events it *holds*. The one thing that may track
continuously is the input trim and the gate, from the assistant's existing per-block summary frames
([ToneAssistant.h:36-64](../engine/assistant/include/nts/assistant/ToneAssistant.h#L36)) — see E3,
which is optional and off by default.

### D3 — The warning is a warning, not a lock

This matches the choice already made for the song-match veil, and for the same stated reason:
"A warning rather than a lock … disabling a page someone is already working in is a worse surprise
than letting them work in it" ([ModulePage.h:39-50](../Source/ui/ModulePage.h#L39)). The dialog has
three answers, and one of them is always "change it anyway".

### D4 — Only user gestures from this editor raise a dialog. Ever.

Three sources can move a parameter, and only one of them may be interrupted:

| Source | Behaviour |
|---|---|
| The user, in this editor | Warning dialog, on the message thread |
| Auto Match itself | Suppressed — see B3 |
| Host automation / a MIDI program change / preset recall | **Never warns.** The parameter is silently released from ownership and the page says so |

A modal raised while a DAW writes an automation lane is a hang. `parameterGestureChanged` can arrive
on the audio thread, so nothing in the guard may allocate, lock or open a window at the call site;
everything goes through an `AsyncUpdater`, which the processor already uses for the same class of
problem ([PluginProcessor.h:418](../Source/PluginProcessor.h#L418)).

### D5 — Ownership is per parameter, and survives a save

"Turn Auto off" is the blunt answer; "let me have this one knob" is the useful one. Releasing a
single parameter leaves the other twenty-nine owned, which is what a player who wants the matched
rig with three more decibels of presence actually wants. The owned/released set is saved with the
project — a session reopened must not quietly start overwriting a knob the user had released.

### D6 — Auto owns every control the match implies, including the ones the apply path forgets today

See F1–F3 below. Anything Auto is going to *guard*, it must first actually *write*.

---

## What is broken today

These are not hypothetical. Each is a defect in the current apply path that Auto Match would inherit
and amplify, because Auto Match promises the applied rig is the analyzed rig.

### F1 — The Gain macro is never reset, so an applied rig is louder or quieter than the one that won

`applyRecoveredRig` writes all four stage drives
([PluginProcessorHost.cpp:174-177](../Source/PluginProcessorHost.cpp#L174)) and never touches
`gain`. But `gain` is not a knob beside them — it is a macro added *on top* of them:

```
const auto simpleGainOffset = (parameterOf(Param::gain) - 5.0f) * 3.0f;   // PluginProcessorAudio.cpp:645
parameters.stages[stage].driveDb = parameterOf(stageParam) + simpleGainOffset;   // :660
```

A user whose Gain knob sits at 8 gets **+9 dB on every preamp stage** relative to the render the
candidate was scored from. At 2 they get −9 dB. The rig they audition on the Song Match page is not
the rig that won, and the error is exactly the axis the search spends five of its twelve pool slots
exploring ([SourceReconstruction.cpp:399](../engine/reconstruction/src/SourceReconstruction.cpp#L399)).

**Change:** write `gain = 5.0` in `applyRecoveredRig`, so the macro contributes zero and the stage
drives mean what the candidate said. **Risk:** a user who had Gain somewhere deliberate loses it —
which is the point of the mode, and is what the warning window is for.

### F2 — The panel switch is never reset, so a leftover switch re-voices the applied rig

`applyPanelSwitch` runs *after* everything else in `currentAmpParameters`
([PluginProcessorAudio.cpp:694-696](../Source/PluginProcessorAudio.cpp#L694)) and rewrites the
parameter block. `applyRecoveredRig` never writes `panelSwitch`, so a Bright or Fat switch left on
from a previous voicing is applied on top of the matched rig, silently. **Change:** write
`panelSwitch = 0` (`none`) with the rest.

### F3 — Candidates are rendered at ≥4x and played back at whatever the user left

`setCandidateParameters` pins every stage to `std::max(4, voicingOversampling[stage])`
([SourceReconstruction.cpp:477](../engine/reconstruction/src/SourceReconstruction.cpp#L477)) and the
comment there says why in detail: at 1x the analyser measures the render's aliasing rather than the
amplifier. Live playback takes its factor from the `oversampling` parameter and the performance tier
([PluginProcessorAudio.cpp:663-671](../Source/PluginProcessorAudio.cpp#L663)). On Eco, or with the
control at 1x, the applied rig is played through exactly the fold-back the search excluded.

**Change:** Auto Match writes `oversampling` to the "Auto (follows gain)" index and states in the
warnings panel when the tier caps it below 4x. **Deliberately not** forced past the tier — the tier
is an explicit user choice about their machine and this plan does not overrule it
([PluginProcessor.h:336-348](../Source/PluginProcessor.h#L336)).

### F4 — A large part of a fitted rig has no host parameter at all

`setCandidateParameters` fits these, and **no control in the plug-in can show or change them**:

| Fitted field | Set from | Reachable by the user? |
|---|---|---|
| `stageCount` | gain (2 / 3 / 4 stages) | No |
| `stages[].asymmetry` | `gain * 0.35` | No |
| `stages[].attackReduction` | dynamics axis | No |
| `powerAmp.saturation` | gain | No |
| `cabinet.highCutHz` | cabinet darkness | No |
| `postLowDb` / `postMidDb` / `postHighDb` | low blend, brightness | No |
| `preEq.lowShelfDb` / `midEmphasisDb` | reference EQ fit | No — already disclosed as a warning ([PluginProcessorHost.cpp:246-252](../Source/PluginProcessorHost.cpp#L246)) |
| `outputGainDb` (preset's own) | fixed −9 dB | No |
| `phaseInverter.*` | voicing only | No |

This is the honest core of "auto adjusts *every* setting": today it adjusts a set of internal values
the user cannot see, cannot audit and cannot undo — and the apply path silently drops several of
them because there is no parameter to write. Two options, and this plan recommends the second:

- **Expose them all as parameters.** Nine-plus new automatable parameters, each needing a range, a
  name, a place in `ampControlIds` and a page to live on. Large, and most of them are not controls a
  player would ever reach for.
- **Recommended: expose the three that change the sound most and disclose the rest.** Promote
  `attackReduction` ("Attack softening"), `stageCount` ("Preamp stages") and the post-EQ trio as a
  single "Voicing EQ" group on Tone Shaping; list everything else in the Auto Match panel as
  "fitted, not adjustable", reusing the existing disclosure pattern rather than inventing one.

Whichever is chosen, the rule stands: **Auto Match must not claim to own what it cannot write.**

### F5 — Nothing records what the apply path wrote

`appliedRecoveredRig` holds the `AmpParameters`
([PluginProcessor.h:549](../Source/PluginProcessor.h#L549)), not the parameter values that were
written. There is therefore nothing to compare a later user edit against, and no value to restore if
the user says "keep auto". Track B fixes this by recording a `std::vector<ParameterValue>` snapshot —
the same type the assistant already uses for its preview/undo history
([ToneAssistant.h:77](../engine/assistant/include/nts/assistant/ToneAssistant.h#L77)).

---

## Design

### The four states

```
        setAutoMatchEnabled(true)          applyCandidate / region / re-analysis
  off ──────────────────────────▶ armed ─────────────────────────────────▶ holding
   ▲                               │                                          │
   │                               │ no analysis available                    │ user edits an
   │                               ▼                                          ▼ owned control,
   └───────────── setAutoMatchEnabled(false) ◀────────── partially overridden ─┘ chooses "anyway"
```

- **off** — today's behaviour, unchanged in every respect.
- **armed** — the switch is on, no analysis has been applied yet. Nothing is owned, nothing warns.
- **holding** — a candidate has been applied. Every parameter in the owned set is guarded.
- **partially overridden** — at least one parameter has been released. The page says which, and
  offers "Give them all back to Auto" as a one-click return to *holding*.

### The owned set

One table, in the processor, next to `ampControlIds` — the single place a parameter is declared
auto-ownable. Everything `applyRecoveredRig` writes, plus the three F1–F3 additions:

| Group | Parameters |
|---|---|
| Core tone | `gain`, `bass`, `mid`, `treble`, `presence`, `resonance`, `master` |
| Voicing | `instrument`, `topology`, `panelSwitch`, `engineMode` |
| Front end | `input`, `lowCut`, `highCut`, `tightness`, `pickEmphasis`, `bias` |
| Stages | `stage1`–`stage4`, `oversampling` |
| Power | `sag`, `feedback`, `loudnessMatch` |
| Bi-amp | `crossover`, `cleanBlend`, `dryBlend`, `lowBandDrive`, `lowBandLevel`, `highBandLevel` |
| Gate | `gateEnabled`, `gateThreshold`, `gateDepth`, `gateAttack`, `gateHold`, `gateRelease` |
| Cabinet | `cabinet`, `cabinetBlend`, `cabinetWidth`, `cabinetAlignment` |
| Chain (only with Isolate chain on) | four `pedalNBypass`, `delayMix`, `reverbMix` |

**Never owned**, and worth stating so the boundary is deliberate: `output` (a level control, the
user's own), `bypass`, `tunerMute`, `tunerReference`, `performanceTier` (a machine choice, not a
tone choice), the pedal voicing controls other than bypass, and everything on the Circuit and Neural
pages — Auto Match fits a *traditional* rig, and pretending otherwise would guard controls it never
computed.

### The guard

```
juce::AudioProcessorParameter::Listener on every owned parameter
   │
   ├─ parameterGestureChanged(index, true)     ← a user grabbed a control
   │     if (autoWriteDepth > 0) return;             // B3: Auto is writing, not the user
   │     if (state != holding && != overridden) return;
   │     if (!editorPresent) { release(id); return; }  // D4: no editor ⇒ not a user gesture
   │     pendingWarning.store(index); triggerAsyncUpdate();
   │
   └─ parameterValueChanged(index, value)      ← anything moved it, including automation
         if (autoWriteDepth > 0) return;
         if (no gesture is open for this index) { release(id); noteExternalWrite(id); }
```

`autoWriteDepth` is an `std::atomic<int>` raised by an RAII scope around every write in
`applyRecoveredRig`, which is required because `setParameterValue` issues real begin/end gestures
([PluginProcessorHost.cpp:472-482](../Source/PluginProcessorHost.cpp#L472)) and would otherwise
warn about its own writes.

### The dialog

Raised on the message thread from `handleAsyncUpdate`, at most **one at a time**, and at most **once
per parameter per session** — a `std::bitset` over the owned set, cleared when Auto is re-enabled.
Without that rule a single knob drag raises a dialog per pixel.

> **Auto Match is already setting this**
>
> Presence is currently set from the analysis of *&lt;song name&gt;* — the match put it at 6.4 to
> get the top end of the reference. Changing it by hand will move the tone away from the match, and
> Auto Match will not move it back.
>
> [ Keep the matched value ]   [ Change it anyway ]   [ Turn Auto Match off ]
>
> ☐ Don't warn me again this session

- **Keep the matched value** — restores the recorded value from the applied snapshot (F5) and leaves
  ownership intact.
- **Change it anyway** — releases that one parameter; state becomes *partially overridden*. The knob
  keeps whatever the user just dialled.
- **Turn Auto Match off** — releases everything, state becomes *off*. Nothing is reverted.
- **Don't warn again** — suppresses the dialog for the rest of the session; edits still release
  their parameter, and the page badge still shows what has been released. Persisted in the assistant
  preferences file if the user asks for it to stick
  ([PluginProcessorSupport.cpp:45-52](../Source/PluginProcessorSupport.cpp#L45)).

`juce::AlertWindow::showAsync` with a callback, never `showModalDialog` — the plug-in must not spin
a modal loop inside a host.

### Persistence

- One new `Bool` parameter `autoMatch`, appended to the layout, appended to `ampControlIds` (54 → 55,
  under the 64 the project validator enforces — read the append-only warning at
  [PluginProcessorInternal.h:165-191](../Source/PluginProcessorInternal.h#L165) before touching that
  list).
- The released set and the applied snapshot go in a new `ProjectState` block at schema 5, alongside
  `PedalboardState`, defaulting to empty so every schema-4 project loads unchanged
  ([ProjectState.h:120-141](../engine/state/include/nts/state/ProjectState.h#L120)).

---

## Track A — Make an applied rig complete and reproducible

*Everything here is worth doing on its own merits, with or without Auto Match.*

✅ **A1 — Reset the Gain macro on apply.** `gain = 5.0` in `applyRecoveredRig`. *Done when:* applying a
candidate twice from different starting Gain positions produces identical `currentAmpParameters()`.

✅ **A2 — Reset the panel switch on apply.** `panelSwitch = 0`. *Done when:* the same test as A1 passes
with a Bright switch engaged beforehand.

✅ **A3 — Align oversampling with the render.** Write the Auto index; warn when the tier caps the result
below 4x. *Done when:* `reconstructionApplyWarnings` carries the cap message on Eco and not on Studio.

✅ **A4 — Disclose what cannot be applied.** Extend the existing warnings
([PluginProcessorHost.cpp:207-255](../Source/PluginProcessorHost.cpp#L207)) with the F4 list —
attack softening, stage count, power saturation, cabinet high cut and the post-EQ trio — reporting
values rather than merely naming the fields. *Risk:* a long warning panel; it is already laid out to
shorten the shortlist rather than overlap it ([SongMatchPage.cpp:95-114](../Source/ui/SongMatchPage.cpp#L95)).

✅ **A5 — Record what was written.** Capture a `std::vector<nts::assistant::ParameterValue>` of every
owned parameter immediately after `applyRecoveredRig` completes, held beside `appliedRecoveredRig`.
*Done when:* a unit test can ask the processor what value Auto put on `presence`.

---

## Track B — Ownership model

✅ **B1 — The owned-set table**, next to `ampControlIds`, with a `static_assert` that every id in it
resolves to a real parameter (the same failure mode `parameterIdList` already guards against at
[PluginProcessorSupport.cpp:28-33](../Source/PluginProcessorSupport.cpp#L28)).

✅ **B2 — State and API** on the processor: `setAutoMatchEnabled`, `autoMatchState`, `autoMatchOwns(id)`,
`releaseAutoMatchParameter(id)`, `restoreAutoMatchParameter(id)`, `reclaimAllAutoMatchParameters`.

✅ **B3 — The `autoWriteDepth` scope**, wrapped around every write path Auto uses — `applyRecoveredRig`,
the re-derivation in E1, and the restore in the dialog's "keep" answer.

✅ **B4 — Persistence** — schema 5 block, plus the `autoMatch` parameter. *Risk:* the highest-consequence
item in the plan. A positional mistake in `ampControlIds` corrupts every saved project silently; the
comment at that list documents exactly how that happened before.

---

## Track C — The guard and the warning window

✅ **C1 — the guard**, a `juce::AudioProcessorParameter::Listener` + `AsyncUpdater`, owned by the
processor, registered only while an editor exists (`setEditorActive`, already plumbed at
[PluginProcessor.h:241](../Source/PluginProcessor.h#L241)).

✅ **C2 — Gesture routing and suppression**, per the pseudocode above. *Risk:* the audio thread. No
allocation, no lock and no window at the listener call site — one atomic store and a
`triggerAsyncUpdate`.

✅ **C3 — The dialog**, async, one at a time, once per parameter per session.

✅ **C4 — External-write release.** Host automation, MIDI program change and preset recall release the
parameter without a dialog and record that they did, so the page can say *"Presence was changed by
the host and is no longer matched"*.

✅ **C5 — The "keep" restore path**, from the A5 snapshot, wrapped in the B3 scope so restoring does not
warn about itself.

---

## Track D — Interface

✅ **D1 — The Auto Match switch on Song Match**, beside `isolateChain`
([SongMatchPage.cpp:159-165](../Source/ui/SongMatchPage.cpp#L159)), with a status line: *holding 31
controls from "&lt;song&gt;" · 2 released*.

✅ **D2 — Owned-control marking on the Amplifier and Tone Shaping pages.** A tint on the knob ring for
owned, normal for released. Not a disabled control — see D3 in Decisions. Both pages already take a
push from the shell for a rig-wide condition (`setPerformanceLimits`,
[ModulePage.h:37](../Source/ui/ModulePage.h#L37)); this follows the same route rather than polling.

✅ **D3 — A page badge**, reusing the veil's visual language but permanent and non-blocking: *"Auto
Match is holding these controls."*

✅ **D4 — "Give them back to Auto"** on the Song Match panel, visible only in *partially overridden*.

---

## Track E — Re-derivation

✅ **E1 — Event list.** Re-derive on: candidate applied, region changed
([SongMatchPage.cpp:174-178](../Source/ui/SongMatchPage.cpp#L174)), reconstruction finishing, and
`instrument` changing while holding. Released parameters are skipped, always.

✅ **E2 — Instrument change is a re-search, not a re-apply.** `searchableTopologies` returns a different
set per instrument ([SourceReconstruction.h:203](../engine/reconstruction/include/nts/reconstruction/SourceReconstruction.h#L203)),
so switching to bass while holding a guitar match must re-run the search, not re-write guitar values.
Until it completes the state shows *armed*, not *holding*.

✅ **E3 — Live trim tracking (default off).** Input trim and gate threshold followed from the
assistant's `SignalObservation`, which is measured already and costs nothing extra
([ToneAssistant.h:55-75](../engine/assistant/include/nts/assistant/ToneAssistant.h#L55)). Bounded to
±6 dB from the matched value, and only these two parameters. *Risk:* a control moving on its own is
alarming; hence default off, a visible indicator, and the tight bound.

---

## Track F — Tests and documentation

✅ **F1 — `nts_wrapper_tests`:** apply-idempotence (A1, A2), suppression (an Auto write raises no
warning), external-write release, restore-on-keep, and per-parameter release leaving the other
owned parameters still written by a re-derivation.

✅ **F2 — schema 5 round trip:** schema 5 round-trip, and a schema-4 project loading with an empty owned set.

✅ **F3 — recovered-rig parity:** that the parameters `applyRecoveredRig` writes reproduce the
`AmpParameters` the candidate carried, field by field, for the fields that have a parameter — the
regression test A1–A3 exist to make pass.

✅ **F4 — Documentation:** a user-guide section on what Auto Match owns and how to take a control back,
and a developer-guide note on the guard's threading contract.

---

## Out of scope, and why

- **A continuously re-fitting mode.** See D2 — the search cost forbids it.
- **Auto Match for the Neural capture and Physical circuit engines.** The search fits a traditional
  rig; there is nothing to own on those pages.
- **Auto-selecting pedals.** The reconstruction renders without a board and scores without one; a
  mode that chose pedals would be guessing, and `isolateChain` exists precisely because the board is
  the player's own work ([PluginProcessorHost.cpp:191-202](../Source/PluginProcessorHost.cpp#L191)).
- **Exposing every fitted internal as a parameter.** See F4 — recommended as the three-control
  compromise, not the nine-parameter expansion.
- **Listening validation.** Nothing in this plan claims an auto-matched rig sounds like its reference.
  It claims the applied rig is the rig that was scored, which is a different and testable statement.

## Open questions for you

1. **Which analyzer arms it?** This plan assumes Song Match (a whole song → a rig). Tone Analyzer
   produces a `ToneAnalysisResult` from a clip but no candidate search runs on it — wiring it in is
   a further half-day. Should Auto Match work from a Tone Analyzer clip too?
2. **Does Auto own the pedals?** Currently only via `isolateChain`, which bypasses them. Should Auto
   Match force isolation, or leave the board alone and warn?
3. **F4 — how much do you want exposed?** Three new controls and a disclosure list, or the full set
   of nine-plus new parameters?
4. **E3 — live trim tracking:** worth building at all, or is "the analysis sets it once and holds"
   the whole feature?
