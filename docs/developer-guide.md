# TubeForge developer guide

TubeForge is a C++20 CMake project built on JUCE 8. Engine libraries are wrapper-independent; the VST3 and
standalone targets share `Source/PluginProcessor.*`. Never allocate, lock, perform file/network work, parse
JSON, log, or destroy heavyweight state in `processBlock`. Publish prepared immutable state at a block
boundary and crossfade audible transitions.

Configure and validate on Windows:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
cmake --build build --config Release --target nts_release_bundle
```

Tests cover unit/integration DSP, IRs, amp, neural parity/performance, circuit, analysis, reconstruction,
assistant, package security, wrappers, and a real JUCE VST3 scan/instance/process/state/editor lifecycle.
Timing acceptance tests should also be run individually on an otherwise idle machine. Python tests require
`python -m pip install -e ml`.

### The cabinet is a stage, not a member of the amplifier

`nts::amp::CabinetSection` is instantiated **twice**, and the distinction is load-bearing:

- `TubeForgeAudioProcessor::cabinetStage` is the one you hear. It runs in `processBlock` after the engine
  dispatch and after `runSharedGate`, so all three engines feed it. Before this, only the traditional engine
  had a cabinet at all — a preamp-only neural capture played out with no speaker in front of it, and the
  circuit engine had a different cabinet built from a graph node.
- `AmpVoice::cabinet` still exists, for **offline rendering only**. `RigReconstructor` renders candidates
  through `TraditionalAmpProcessor` on a worker thread and needs a cabinet in that render, and handing it the
  live stage would mean sharing an audio object across threads. On the live path that copy is bypassed:
  `liveAmpParameters()` is `currentAmpParameters()` with `cabinet.bypass = true`, and it is what the two
  `traditionalAmp.setParameters*` call sites use. `currentAmpParameters()` keeps a full cabinet because it
  answers "what does this rig say", which preset export and the assistant both need.

Position in the chain is after the gate, not before it. That is what a real rig does — a gate sits ahead of
the power section, not ahead of the microphone — and it is the only order that works: a cabinet before the
attenuation rings on past a closed gate for the length of its impulse.

Two consequences for reporting, both easy to get wrong. `getTailLengthSeconds` **adds** the stage's tail to
whichever engine is longest, because the stage is downstream of all of them. `refreshProcessingLatency`
**adds** the stage's buffering latency to the engine's for the same reason — and that number is what
`dryDelay` is set to, so under-reporting it makes engaging bypass step the signal in time.

The cabinet's own parameters are saved through `cabinetControlIds` into `nts::state::CabinetState`, **not**
through `ampControlIds`. Read the note above both lists before adding one: `ampControlIds` holds 58 of the 64
entries `validate` accepts, both lists are positional, and both are append-only. A control inserted in the
middle of either does not fail — it silently loads every saved project's values into the wrong controls.

Every one of those parameters defaults to the value its field held while it was unreachable, which is what
lets a schema-5 project restore the cabinet it actually described. `testCabinetSlotControls` in
`nts_amp_tests` is the guard: it pins the hard left/right split `width` used to perform to the two `pan`
defaults, and unity level, no delay on A and no output trim to theirs.

### The built-in cabinet is a model, and Legacy is why it can be

`nts::ir::renderCabinetImpulse` builds a magnitude response from a description — cone resonance, baffle
loading, cone breakup, mass roll-off, the microphone's curve, its angle and its distance — and reconstructs a
**minimum-phase** impulse from it via the real cepstrum: log-magnitude spectrum, inverse transform, fold the
anticausal half onto the causal one, exponentiate, transform back. That construction is legitimate because a
guitar speaker is approximately minimum-phase, so a synthesised magnitude response reconstructed this way has
a plausible time structure rather than the pre-ringing one a linear-phase filter would give it.

Two traps in that code, both commented at the site. The cepstrum is defined on the **natural** logarithm, and
feeding it decibels produces a filter 8.686 times too aggressive. And the transform has to be several times
the requested tap count, or the impulse's tail wraps onto its own head.

`CabinetKind::legacy` is not a voicing anybody would choose and has to exist: every project saved before the
model was voiced against the two synthetic decays in `nts::amp::makeDefaultCabinetImpulse`, and those are used
**verbatim** for that entry rather than approximated. The rule that selects it is in `applyProjectState` — an
empty `CabinetState` block is exactly the set of projects that predate schema 6 — and
`testCabinetModelCompatibility` in `nts_wrapper_tests` asserts both directions: a fresh instance gets a real
cabinet, a schema-5 project gets Legacy.

Rendering is message-thread work, so the audio thread only ever *notices*: `cabinetModelHash` folds the eight
model controls and the tier's impulse ceiling into one word, `processBlock` compares it against
`renderedCabinetModelHash`, and a mismatch triggers an async update. The bulk-write paths call
`refreshCabinetModels` directly instead, because a project is most often recalled with the transport stopped
and a cabinet that only appears once somebody presses play looks broken.

`cabinetHoldState` is the asset half of Auto Match ownership. The parameter guard watches for change gestures
on registered controls, and an impulse response has none — so a matched cabinet is tracked with one rule
instead: anything that writes slot A releases it. That is enough because there is no partial state, and it is
deliberately not saved with the project, since a matched cabinet is generated and a reopened project would
have nothing to restore it from.

`cabinetResponseCurve` is cached for the same reason — a loaded impulse response needs a transform, and the
page asks twenty times a second. It falls back to evaluating the model directly when nothing is cached, which
is the state a host leaves the plug-in in if it creates the editor before calling `prepareToPlay`.

### Cabinet Match fits a filter; it does not search

`nts::ir::measureCabinetResidual` takes the reference and a render of the same material, reduces both to
long-term average power spectra in 41 sixth-octave bands, levels each to its own mean, and differences them.
Levelling is what makes the result a *shape* rather than a loudness — without it a reference mastered 6 dB
hotter reads as a flat 6 dB cabinet, and the one thing a cabinet is not is a gain control. Power is averaged,
not decibels: a mean of logarithms is dominated by the quietest frames, so a passage with rests would read as
darker than the same passage without them.

`matchFromLibrary` subtracts the **measurement cabinet's** own curve before scoring, because swapping cabinet
A for cabinet B changes the output by `B − A`. Getting that wrong produces a shortlist that is confidently
upside down. `synthesizeMatchedCabinet` reuses `minimumPhaseImpulse` with the measurement cabinet plus the
scaled residual, so the result is a complete cabinet rather than an EQ to stack on one — which is why at depth
zero it is bit-for-bit the measurement cabinet, and `nts_ir_tests` asserts exactly that.

**The honest limitation, which the interface repeats and must keep repeating.** `StudioServices` renders each
candidate through the reference itself — there is no separate DI, and its own warnings say so. The residual
`matchCabinetToReference` measures is therefore the difference between *this rig playing that reference* and
*the reference*, which contains the amplifier's difference, the microphone, the room and the mastering along
with the speaker. That is a legitimate corrective filter and is what Fractal's Tone Match produces too; it is
not a recovery of the cabinet in the room, and no wording in the UI may imply that it is. If a real DI capture
path is ever added, this becomes a true cabinet fit with no change to the engine — only to what is handed in.

The test that matters is in `nts_ir_tests`: feed the estimator a signal and the same signal through a known
one-pole, and assert the recovered residual carries that filter's slope. It is the only part of this feature
with an exact right answer, so it is tested against it rather than against a golden output.

### The impulse-response library scans, it does not import

`nts::ir::CabinetLibrary` walks a folder the user chooses and keeps nothing but favourites and recents. That
is the opposite of `nts::nam::CaptureLibrary`, which owns converted copies — and the difference is not a
preference. A `.nam` has to be converted before the audio thread can read it; an impulse response is a WAV
file, so copying it would produce a second copy of every pack, a sync problem, and a list that disagrees with
the disk the first time somebody renames a file.

It is bounded on both count and directory depth, because a user will eventually point it at a drive root. The
failure there is not a slow scan, it is a list of forty thousand rows the interface then has to draw, so the
scan stops and `truncated()` says it stopped. Favourites and recents are keyed by *path*: a response that has
moved stops appearing rather than pointing at whatever took its place.

`CabinetPicker` loads on selection rather than behind a Load button. A shelf of four hundred responses is
something you listen through, and an extra click per candidate turns auditioning into a chore nobody
finishes — the load is already crossfaded and already off the message thread, so being wrong costs a response
you hear and move on from.

**`CabinetPage::paintOverChildren` must call the base implementation.** `ModulePage::paintOverChildren` draws
the song-match veil and the Auto Match badge, and an override that forgets it does not fail loudly — it
silently removes the one thing saying the analyzer is holding these controls.

### True stereo is a second convolver pair, not a four-channel one

A true-stereo capture is the matrix `[[LL, LR], [RL, RR]]`. `CabinetSection` convolves it with **two** stereo
convolvers per slot rather than one four-channel one, because the two-channel machinery already existed and is
the part that had to be right. The direct pair holds `(LL, RR)` and sees `(in_L, in_R)`; the cross pair holds
`(RL, LR)` and is fed the input channels **swapped**, so its channel 0 produces `in_R * RL` and its channel 1
produces `in_L * LR`. Adding the two is the matrix product with no new inner loop anywhere.

The cross ordering is the trap: loading `(LR, RL)` instead of `(RL, LR)` still sounds like a cabinet, just not
the one that was measured, and nobody would catch it by ear. `testCabinetTrueStereo` in `nts_amp_tests` pins
it with four single-spike impulses at four positions and reads the routing off the output.

Everything about it is gated — `crossPrepared`, the per-slot flag, and a channel count above one. A rig with
no true-stereo response loaded executes none of it, and the benchmark below is unchanged to three decimal
places from before the path existed. The convolvers are allocated on first use, like the buffered pair.

`applyCabinetIr` deliberately does **not** apply minimum-phase conversion to the cross terms: their arrival
time is the information they carry, and flattening it collapses a true-stereo capture into something with no
stereo depth at all.

### What the shared cabinet costs

Measured by `nts_amp_benchmarks` (Release, 48 kHz, stereo), as a percentage of the callback budget:

| Response length | One slot | Both slots | Slot B muted |
|---|---:|---:|---:|
| 512 taps (the model's default) | 0.25% | 0.39% | 0.25% |
| 1024 taps (the standard tier's ceiling) | 0.41% | 0.69% | 0.39% |
| 4096 taps (a long user response) | 0.10% | 0.11% | 0.10% |

Three things to read off it. The cost the neural and circuit engines newly carry is **a quarter to four tenths
of one percent** of the callback — the honest answer to "what did making the cabinet global cost everybody",
and it is small. Muting a slot really does return its cost, which is the idle-slot skip covering mute as it
was extended to. And **4096 taps is cheaper than 1024**, because past `partitionedThresholdTaps` the section
switches to the partitioned FFT path: 0.11% against 0.69%, six times cheaper for four times the length.

That last row is a standing invitation to lower the threshold, and it has not been taken. The direct path is
chosen for costing **no latency**, and the buffered one costs a 128-sample partition; trading zero latency for
half a percent of one core is a decision about what this plug-in is for, not a tuning constant to nudge. It is
recorded here so that whoever wants to make that trade can see the numbers behind it.

### Amplifier voicings

`nts::amp::Topology` is thirteen voicings over one amplifier graph: a topology selects a block of values
in `makeOriginalPreset` and nothing else. Indices 0–6 are guitar circuits that bass reaches by retuning;
7–12 are bass-native. `topologyAffinity` publishes that distinction so the picker can filter on it —
**the choice parameter always offers all thirteen**, because filtering it would make a host automation
lane's meaning depend on another parameter's value.

Three rules constrain changes here, and each has a guard:

- **Append to the enum, never insert.** The choice index *is* the `Topology` value and the factory
  program list is `instrument * topologyCount + topology`.
- **Every new field defaults to today's behaviour.** `PreampStageConfig::shape` and
  `PowerAmpParameters::shape` default to the valve curve; the bi-amp band levels default to unity;
  `dryBlend` and `BassPathParameters::alignBands` default to off. `testOriginalVoicingRegression` in
  `nts_amp_tests` renders all fourteen original (instrument, topology) pairs and compares against hashes
  taken from the build before the bass block was added. **Those hashes are never regenerated to make a
  failing build pass** — a diff there is either a sound change in a voicing that is not allowed one, or
  a decision that has to be written down.
- **`ampControlIds` is append-only.** `applyProjectState` walks a saved project's values positionally
  against it, so an id inserted in the middle displaces every id after it and every saved project loads
  wrong — silently, with valid numbers in the wrong fields. Its length is `static_assert`ed against
  `nts::state::maximumAmpControls`.

`PanelSwitch` is a per-voicing front-panel switch (Ultra Lo, Deep, …), applied by
`applyPanelSwitch` *after* the voicing block because it is a control the player moved. The host
parameter is **one flat list for every voicing**, filtered only in the interface — a per-voicing
list would make an automation lane's meaning depend on the topology parameter's value, and the
engine already ignores a switch the current voicing does not carry, so an out-of-panel pairing is
inert rather than wrong.

**Adding a voicing is not free outside the amplifier.** `SourceReconstruction` searches over
topologies, so a new one becomes findable in Song Match with no code change — and costs an extra
offline render on every reconstruction, because the topology re-check renders the winning point under
each voicing the coarse pass did not reach. `searchableTopologies` narrows that by instrument. If a
voicing's identity lives in a parameter the candidate fitter also fits, check
`setCandidateParameters`: it scales the crossover from the voicing's own value and treats stage
oversampling as a floor precisely so a bi-amp and a hard clipper survive being fitted.

Anything that rejoins the signal after the preamp stages without passing through them must be delayed to
match: each oversampled stage contributes `dsp::antiAliasTapsPerPhase` base-rate samples, and summing an
undelayed copy is a comb filter with its first null between about 1 and 1.5 kHz. `AmpVoice::dryDelay` and
`AmpVoice::lowBandDelay` are that compensation; `testDryBlendAlignment` measures phase coherence at the
null frequencies rather than flatness, because the drive path is not flat and comparing against the input
measures its frequency response instead.

The editor draws its own artwork rather than shipping images. `Source/ui/FaceplateArt.*` renders an
amplifier from one row of a style table sized against `nts::amp::topologyCount`, and
`Source/ui/PedalArt.*` does the same for pedals against `nts::pedals::kindCount` — so a voicing or a
pedal kind appended to an engine enum without a livery is a compile error, not something to notice
by eye. Both are procedural: any noise lattice must use a cell count that divides its tile edge
exactly, or the pattern beats against the pixel grid and reads as mottled stone. Review changes to
either with `nts_art_renderer`, which writes every face to PNGs and is the only way to tell whether
a palette change actually improved anything:

```powershell
cmake --build build --config Release --target nts_art_renderer
build\nts_art_renderer_artefacts\Release\nts_art_renderer.exe out
```

The pedalboard is an **engine/model split**, and it is the pattern to follow when adding to it.
`nts::pedals::PedalEngine` is a DSP architecture — shaper, compressor, neural, modulation, crush,
delay, reverb, bass preamp, pitch — and `PedalModel` is a row of data selecting one and supplying its
coefficients, control names, tier and description. Adding a pedal is a table row in
`engine/pedals/src/PedalBoard.cpp` plus a face row in `Source/ui/PedalArt.cpp`; adding a *kind* of
pedal is a new engine. Two rules are load-bearing:

- **Model indices 0–6 are the built-in archetypes and never move.** They are host-visible choice
  values and saved project state, so appending is the only safe edit. `nts_wrapper_tests` asserts it.
- **`PedalModel`'s voicing blocks are positional**: shaper, modulation, crush, delay, reverb,
  bassPreamp, pitch. A reverb row needs four leading `{}` and a pitch row six. Getting that wrong
  assigns a model's numbers to the wrong engine and can still compile.

Every model is checked by `nts_pedal_tests` for finiteness through four slots at full drive and for
boundedness on one slot at unity — the second exists because published pedal blueprints contain
unbounded curves that build, load and play fine until the drive comes up.

`Source/ui/GearPicker.*` is the shared selection component behind both the amplifier voicing and the
pedal kinds: a chip showing what is loaded, expanding into a grid of faces shelved by the character
of the sound. It knows nothing about amplifiers or pedals — a page hands it a `GearCatalogue` of
tiles carrying a parameter index and a paint callback. It cannot use a `ComboBoxAttachment`, so it
writes its parameter through `beginChangeGesture` / `setValueNotifyingHost` / `endChangeGesture`;
the gesture pair is not optional, because a choice written without one appears as an un-writable
automation lane in several hosts.

### Auto Match, and the one rule its guard has

`Source/AutoMatch.h` is the single place the *owned set* is written down — the controls a matched rig
writes and then holds. `Source/PluginProcessorAutoMatch.cpp` is the state and the guard;
`Source/ui/AutoMatchDialog.*` is the question. The design is in `docs/auto-match-plan.md`.

The guard is an `AudioProcessorParameter::Listener` on the owned parameters, and it exists to answer one
question: **who moved this control?** Three answers, three behaviours — a user gesture asks the question,
a host automation write hands the control back silently, and Auto Match's own writes are ignored.

That last one is the rule everything else depends on. `setParameterValue` issues real
`beginChangeGesture` / `endChangeGesture` pairs, so a programmatic bulk write is indistinguishable from
forty-seven simultaneous knob drags. **Any code that writes host parameters in bulk must hold an
`AutoWriteScope`** — applying a rig, recalling a project, applying a profile, accepting an assistant
action, and a MIDI program change all do. Adding a new bulk-write path without one does not fail to
compile and does not fail a test that exists today; it raises a dialog at the user, or silently releases
every control the path touched.

The second rule is threading. Both listener callbacks can arrive on the audio thread, because that is
where a host applies an automation lane. Nothing in them allocates, locks or touches the interface: they
set bits in `std::atomic<std::uint64_t>` masks, which is why `tf::automatch::ownedCount` has a
`static_assert` holding it at 64 or fewer. The dialog is *polled* out of that state by the editor's
20 Hz tick rather than pushed, and so is the automatic apply — `refreshAutoMatch` compares
`StudioServices::reconstructionGeneration` against the last one it applied.

Live tracking (`updateAutoMatchTracking`) is the one part that writes parameters continuously, and it is
built around one property: the assistant's `inputPeak` is measured **before** the input trim is applied,
so the correction is a function of the incoming instrument alone and the loop is open. Closing it — by
measuring after the trim — would make the control either run away or hunt. It is rate limited to 2 Hz with
a 0.75 dB deadband because every write is a real change gesture in the host's automation lane.

The held rig is saved at **project schema 5** as an opaque block of numbers: `nts::state::AutoMatchState`
carries one value per entry of the plug-in's owned table, in that table's order, plus the released
positions. `engine/state` knows nothing about parameters, so the same append-only rule that governs
`ampControlIds` governs `tf::automatch::owned` — inserting into the middle of that table silently
re-points every saved hold. A saved rig whose length does not match the current table is refused rather
than mapped, which is what `restoreAutoMatchProjectState` checks first.

Format changes require an independent version, bounds validation, migration or explicit rejection, fixtures,
and compatibility-matrix update. A package parser must use the allowlist and limits in
`engine/ecosystem`; do not extract arbitrary archive paths. New telemetry fields require privacy review and
must fit the compile-time allowlist. Pull requests should include tests and must pass Debug and Release CI.
