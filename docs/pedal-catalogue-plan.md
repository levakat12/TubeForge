# Pedal catalogue plan: 55 modelled units on a shared set of engines

An implementation-ready plan for turning `Expanded_Master_Pedal_Architecture.xlsx` — 55 catalogued
pedals with circuit notes, DSP blueprints and a complexity tier each — into playable pedals in
TubeForge.

The headline decision is in [Decisions](#decisions-and-why): **55 pedals are not 55 `PedalKind`
values.** They are 55 rows of data over roughly nine DSP engines, in exactly the way seven
amplifier voicings are seven rows over one amplifier graph.

Line references are against the working tree at the commit that completed the gear picker; re-check
them before editing.

## Status

| Track | Items | Effort | State |
|---|---|---|---|
| A — Engine/model split and the control surface | A1–A5 | 3.0 d | ✅ Complete |
| B — The drive family: 30 models, no new architecture | B1–B4 | 3.0 d | ✅ Complete |
| C — New engines: modulation, delay, reverb, crush, pitch | C1–C6 | 5.0 d | ✅ Complete |
| D — The remaining 25 models | D1–D3 | 3.0 d | ✅ Complete |
| E — 55 faces | E1–E2 | 2.0 d | ✅ Complete |
| F — Cost, latency, tests | F1–F4 | 2.0 d | ✅ Complete (F2 struck — see C6) |
| G — Documentation | G1 | 0.5 d | ✅ Complete |

**Coverage: 100% by effort** (18.5 of 18.5 days). **Every track is complete.** The model table holds
**62 pedals** — the seven built-in archetypes, unchanged in sound and in saved state, plus **55
modelled units** across nine engines, each with its own drawn enclosure, its own control names and
its own description, shelved under ten characters.

### What has not been done, and will not be by this plan

**Nobody has listened to any of it.** Every model's values are physically reasoned and every one is
measured — finite through four slots at full drive, bounded on one slot at unity, and for the
engines where a claim can be measured, verified to make the change it claims. But "Green Scream
sounds like the pedal it is modelled on" is not a claim any of these tests make, and it is the claim
that matters most. Expect a pass of tuning by ear, and expect it to move numbers.

**The catalogue's own defects were carried, not fixed.** The source workbook miscounts its
categories, has three pedals no criterion matches, six wrong tier averages, and one unbounded
blueprint. Those are recorded under [Data corrections](#data-corrections-needed-in-the-source-workbook)
and were worked around here; the spreadsheet itself is unchanged.

| Family | Models |
|---|---|
| Overdrive | 11 |
| Distortion | 9 |
| Fuzz | 8 |
| Modulation | 5 |
| Bit & sample reduction | 5 |
| Bass DI | 6 |
| Reverb | 4 |
| Pitch | 3 |
| Delay | 2 |
| Boost, compressor | 2 |

The arithmetic, so it can be checked rather than taken on trust:

### What changed against the plan in E, F and G

- **`PedalShape` was added to `PedalFace`.** Colour separates one pedal from its neighbour; shape
  separates one *family* from another. Four enclosures: the standard cast box, a wider squarer one
  for the big-box fuzzes, a sloped wedge for the heavy distortions, and a wide corner-screwed case
  for the bass DIs and reducers. A shelf of fuzzes visibly larger than the overdrives above it is
  information a player reads before any text.
- **F1 is advisory and deliberately does not act.** `TierLimits::pedalCostBudget` and
  `PedalBoard::activeCost` exist so the Pedals page can turn its help line amber when a board is
  heavier than the tier is meant to carry — and then do nothing else. Silently degrading a pedal
  somebody chose is the worse failure, and it is the one a load-watching heuristic makes on its own.
- **F2 is struck rather than deferred.** The pitch engine adds no latency (see C6), so there is
  nothing left for the board to report.
- **F4 was already satisfied** by `nts_art_renderer`, which sheets every model as the table grows.

**Total: 18.5 days.** This is a multi-week feature, not a session.

### What changed against the plan in Track A

- **`PedalKind` survives as named constants rather than being replaced.** The plan implied
  `PedalParameters::kind` would become an index; it did (`model`), but the enum stays for the seven
  archetypes so call sites that mean a specific one still read as that one, and `modelIndexOf` is
  the bridge. `PedalTests` uses it, which makes the tests themselves the guard that the archetypes
  keep their indices.
- **`PedalFace` lost its name and blurb.** They duplicated the model table. The face now carries a
  short `plateText` for the enclosure and nothing else: the engine owns what a pedal *is*, the art
  owns only what it looks like.
- **The face table became a `std::vector`.** Neither count is a compile-time constant once the model
  table grows, so the array/`static_assert` pairing is gone and the run-time check in
  `nts_wrapper_tests` is now the only guard. Slightly weaker, and the reason F3 matters.
- **Controls hide rather than grey out.** A model names the controls it has and the rest are removed
  from the card, which meant splitting `layOutSlot` out of `resized` so rows re-flow on a model
  change and not only on a resize. A knob that is present and inert reads as broken; an absent one
  reads as absent.
- **State validation's ceiling moved from 64 to 512.** It was a deliberately loose bound on an
  enumeration of seven; it is now a deliberately loose bound on a catalogue.
- **F3's runaway guard landed early, in `nts_pedal_tests`.** It had to be split in two: the existing
  four-slot test runs +24 dB of make-up per slot, which is 96 dB of legitimate gain and swamps any
  threshold small enough to catch a diverging curve. Boundedness is now checked on one slot at unity
  level, where the only thing that can make the output large is the shape itself. It runs over every
  model in the table, so it now guards all 37.

### What changed against the plan in Track B

- **Two waveshapes were added, not just the three DSP blocks.** `germanium` (half silicon's forward
  voltage, soft knee, asymmetric) and `ledClip` (three times the forward voltage, then an abrupt
  ceiling) do more to separate models than any amount of coefficient tuning, and both are bounded by
  construction. The eleven overdrives would otherwise have been eleven sets of the same curve.
- **The gyrator pair is swept by the aux controls rather than fixed.** The plan described it as a
  twin-peak EQ; making each band's gain a control is what turns it from a voicing into the thing
  those two knobs actually do on the boxes it models.
- **The slew ceiling is specified at 48 kHz and scaled.** A per-sample limit is meaningless without a
  rate: at 96 kHz the same figure would let the signal move twice as fast per second, so a model
  would sound different depending on the session. The scaling divides by the rate ratio, which is
  the direction that is easy to get backwards.
- **B3 (ADAA) landed, opt-in per model.** `dsp::AntialiasedWaveshaper` takes the difference of the
  curve's antiderivative across each sample interval, with a midpoint fallback inside a 1e-5 band
  where the quotient goes singular — which is not an edge case, it happens on silence and on any
  held DC. Ten of the thirty hard-edged models enable it; the seven archetypes deliberately do not,
  because it costs half a sample of delay and a little top end and they have to keep sounding as
  they always have.
- **`asymmetricPolynomial` is the one shape ADAA does not cover.** Its outer clamp binds partway up
  the curve, at about c = 0.957 where the polynomial first reaches 1, so the antiderivative is
  piecewise around a breakpoint that has to be found by solving a cubic. The shape is soft and
  bounded and aliases far less than a hard clipper, so an approximate antiderivative would be worse
  than none. `dsp::supportsAntiderivative` says so and the engine checks it.
- **The ADAA test took two goes to be worth anything, and both failures are instructive.** The
  first compared a hard-clipped model against the soft-clipped archetype and was measuring the
  difference between two curves rather than the difference the integration makes. The second used a
  4 kHz fundamental at 48 kHz — where every harmonic folds back exactly onto another harmonic, so
  the measurement reads zero however bad the aliasing is. It now runs one curve at one gain, at
  4100 Hz, and asserts the foldback at least halves.
- **`nts_art_renderer` now wraps into a grid.** Thirty-seven pedals in one strip is seven thousand
  pixels wide, which is a picture nobody can look at.

### What changed against the plan in Track C

- **`ModulatedDelayLine` is a new class, not an option on `DelayLine`.** The existing one delays by
  whole samples and steps when its length changes, which is right for latency alignment and wrong
  for anything that sweeps. Four-point Hermite rather than linear: linear interpolation on a swept
  delay is a low-pass whose cutoff moves with the sweep, so a chorus dulls at one end of its travel
  and brightens at the other — the artefact lands exactly where it is most audible.
- **The phaser uses first-order all-pass sections written by hand, not `dsp::Biquad`.** Biquad is a
  block processor with interpolated coefficients and cannot move its corner *within* a block, which
  is where a phaser's sweep lives. A real phaser stage is one JFET and one capacitor — first order —
  so a biquad would also be modelling something that is not there.
- **Chorus and flanger are one topology with different numbers.** A chorus is a 10-to-25 ms delay
  swept a little with no feedback; a flanger is a sub-millisecond one swept wide with a lot. That is
  not a shortcut, it is what they are.
- **`PedalCharacter` went from five shelves to seven** rather than the twelve D3 floated. Modulation
  and Bit & Sample earn their own; splitting further would give shelves of one.
- **Three tests were written wrong before one was written right, and all three failures are
  recorded in the test's own comment.** Counting distinct output levels fails because the resonant
  filter smooths the steps back into a continuum. Measuring inharmonic energy fails because an
  unwindowed transform of a strong sine leaks across every bin — and the crushed signal, being
  quieter, scored *lower* than the clean one. And the test harness wraps its source buffer, so a
  tone that does not fit a whole number of cycles clicks at every wrap, which is broadband and
  swamps whatever is being measured. The test now measures the ratio of largest to mean first
  difference, which is what "holds flat then jumps" means and cannot be confounded by any of those.
- **Delay and reverb wrap `dsp::Delay` and `dsp::Reverb` rather than reimplementing them**, as the
  plan intended. Both are driven at full wet, because the slot already has a blend and mixing twice
  would apply the control twice. The one thing `dsp::Reverb` does not provide is spring dispersion,
  which is four all-pass sections ahead of the tank — the same hand-written first-order section the
  phaser uses, and the entire difference between a spring and a small room.
- **The delay's tone control moves the repeat bandwidth *around* the model's own ceiling** rather
  than replacing it, so a bucket-brigade line stays analogue at every tone setting instead of
  turning digital at 10.
- **The bass preamp reuses the amplifier's crossover approach rather than a new split.** Drive is
  applied to the upper band only; the low band passes clean. That is the whole reason a bass
  distortion is not a guitar distortion on a bass — overdriving the fundamental of a low B loses
  the note.
- **`PedalCharacter` went to nine shelves** (adding Delay & Reverb and Bass DI), still short of
  D3's twelve. Nine shelves for fifty-nine tiles averages six or seven each, which is a shelf worth
  scrolling to.
- **A voicing-block ordering mistake is worth recording**, because the compiler caught it only by
  arity: `PedalModel`'s blocks run shaper, modulation, crush, delay, reverb, bassPreamp, pitch, so a
  reverb row needs *four* leading `{}` and a pitch row *six*. Getting that wrong assigns a model's
  numbers to the wrong engine, and would have built cleanly had the field counts happened to match.

### C6 was built differently, and F2 is moot as a result

**The pitch engine is not the phase vocoder the plan specified**, and the substitution is the single
most consequential decision in the whole track.

A vocoder needs an FFT, which needs a frame of buffering, which means the plug-in must report
latency to the host and the dry path must be delayed to match. That is a change reaching well
outside this engine, and the plan flagged it as its own highest risk and its own open question 2 —
for the sake of three pedals out of fifty-five.

Reading a delay line at a ratio does the same job in the time domain. A tap walking towards the
write head plays back faster and sounds higher; one walking away sounds lower. The tap has to wrap
when it runs out of window, so there are two taps half a window apart, each faded by a Hann window
— and Hann windows at fifty per cent overlap sum to exactly one, which makes the wrap inaudible.

- **What it costs:** on a chord the two taps read different parts of the same waveform, so dense
  material warbles in a way a vocoder would not. That is the honest trade, and it is the same one
  every hardware pitch pedal of this type makes.
- **What it buys:** zero added latency. The effect is causal and needs no compensation anywhere,
  so **F2 (latency reporting) has nothing left to report** and is struck rather than deferred.

`ModulatedDelayLine` was split into `write` / `readAt` / `advance` to support it, because a pitch
shifter writes once and reads two or four times into the same history. `processSample` remains as
the single-tap convenience, which is what every other caller uses.

**Ship the first slice on its own.** Tracks **A + B + the drive half of E** is about 7 days and
delivers **30 of the 55 pedals** — every overdrive, distortion, fuzz, boost and compressor — because
that family needs almost no DSP that does not already exist. Everything after that is new engines.

## What exists today

`PedalSlot` ([PedalBoard.h:75](../engine/pedals/include/nts/pedals/PedalBoard.h#L75)) is already one
architecture driven by a data table, which is the pattern this plan extends rather than replaces:

```
input HPF → ( waveshaper | compressor | neural model ) → tone LPF → level → wet/dry blend
```

`Voicing` ([PedalBoard.h:97](../engine/pedals/include/nts/pedals/PedalBoard.h#L97)) is the per-kind
data: input high-pass, drive range in dB, waveshape, bias, tone sweep, make-up trim, and whether the
clipper runs parallel to the dry path. Seven kinds, one graph. That is the engine/model split already
— it just has one engine.

What the shared DSP library brings:

| Already in `nts::dsp` | Where |
|---|---|
| `Delay` with damping and tail reporting | [Effects.h:31](../engine/dsp/include/nts/dsp/Effects.h#L31) |
| `Reverb`, comb-and-allpass | [Effects.h:89](../engine/dsp/include/nts/dsp/Effects.h#L89) |
| `DelayLine`, `Biquad`, `LinkwitzRileyCrossover`, `Compressor`, `Oversampler`, `SmoothedParameter` | `engine/dsp` |
| `Waveshape`: tanh, atan, hard clip, soft clip, diode, asymmetric polynomial | [Nonlinear.h:50](../engine/dsp/include/nts/dsp/Nonlinear.h#L50) |

What the spreadsheet asks for that does **not** exist: a slew-rate limiter, fractional-delay
interpolation (Hermite or Lagrange), bit and sample-rate reduction, a feedback delay network, gyrator
twin-peak EQ, a phase vocoder, and antiderivative anti-aliasing.

The four host controls per slot are fixed at Drive, Tone, Level, Mix
([PluginProcessorHost.cpp:405](../Source/PluginProcessorHost.cpp#L405)), and the picker built in the
[gear picker plan](gear-picker-plan.md) already scrolls, shelves and searches an arbitrary number of
tiles — so the *browsing* problem is solved and none of this plan needs to touch it.

## Decisions, and why

**55 models over 9 engines, not 55 kinds.** An engine is a DSP architecture; a model is a row of
coefficients, knob names and a face. The spreadsheet is already organised this way — its "DSP &
Plugin Implementation Blueprint" column names the engine and everything else is model data. This is
the same shape as `Topology` (seven voicings, one amplifier graph) and `FaceplateStyle` (seven
liveries, one renderer), and both of those are load-bearing precedents in this codebase.

Proposed engines, with the models each carries:

| Engine | Models | New DSP needed |
|---|---|---|
| `shaper` | 30 — every overdrive, distortion, fuzz, boost | slew limiter, gyrator EQ, ADAA |
| `compressor` | 1 | none |
| `neural` | any capture | none |
| `bassPreamp` | 6 | crossover + parallel voicing (amp engine has most of it) |
| `modulation` | 5 — chorus, flanger, phaser | BBD fractional delay, LFO |
| `delay` | 3 | wrap `dsp::Delay`, add BBD degradation |
| `reverb` | 4 | FDN, spring dispersion |
| `crush` | 5 | sample/bit reduction, SVF |
| `pitch` | 2 | phase vocoder |

**The first seven model indices must stay exactly what they are.** `PedalKind` is a host-visible
choice parameter *and* saved project state
([ProjectState.h:88](../engine/state/include/nts/state/ProjectState.h#L88)), so indices 0–6 must keep
producing None, Boost, Overdrive, Distortion, Fuzz, Compressor and Neural capture. The named units
append from index 7. This is the same constraint the topology work hit, and it happens to give a good
ordering anyway: generic archetypes first, then modelled units.

**Four controls become six.** The HM-2 has Level, Color L, Color H and Distortion; the Metal Zone has
six. Four generic knobs cannot carry them. The host parameters stay generically named — `Drive`,
`Tone`, `Level`, `Mix`, `Aux A`, `Aux B` — and the *interface* shows each model's real knob names from
its table row.

**State this cost plainly rather than discovering it later:** a host automation lane will read
"Pedal 1 Aux B", not "Color H". A `juce::AudioParameterFloat`'s name is fixed at construction, so the
alternative is 55 × 6 parameters registered per slot, which is absurd. Generic-but-stable names are
the right trade, and the mapping is visible in the interface where the user actually turns the knob.

**Per-slot parameter count goes 6 → 8**, so the board goes 24 → 32 host parameters. Additive only:
a project saved before this loads with the two new controls at their defaults.

**Models are data, faces are data, and both tables are sized against the same count.** `PedalFace`
([PedalArt.h](../Source/ui/PedalArt.h)) already asserts its length against `nts::pedals::kindCount`.
That assert becomes the guard for all 55 — a model added without a face stays a compile error.

## Track A — Engine/model split and the control surface

### A1 · `PedalEngine` and `PedalModel`

**Change.** In `engine/pedals`, add `enum class PedalEngine` and a `PedalModel` table row:

```cpp
struct PedalModel
{
    std::string_view name;        // "TS9 Tube Screamer"
    std::string_view maker;       // "Ibanez"
    PedalEngine engine {};
    /// 1..5 from the catalogue. Drives the cost policy in F1 and a badge on the tile.
    int tier { 3 };
    /// Which knobs this model actually has, and what they are called. Empty means absent.
    std::array<std::string_view, 6> controls {};
    ShaperVoicing shaper {};      // the existing Voicing, renamed
    ModulationVoicing modulation {};
    // ... one block per engine; a model fills the one its engine reads
};
```

Plain sub-structs rather than a variant: they are small, trivially copyable, and the audio thread
reads one of them per block with no branching on a type tag.

**Risk.** Low. `Voicing` already is this for one engine.

**Done.** `PedalSlot` takes a `PedalModel` and today's seven behave bit-identically.

### A2 · Six controls

**Change.** `PedalControl` gains `auxA` and `auxB`
([PluginProcessor.h:159](../Source/PluginProcessor.h#L159)); `pedalParameterStride` 6 → 8;
`PedalParameters` gains two floats; the registration loop
([PluginProcessorHost.cpp:399](../Source/PluginProcessorHost.cpp#L399)) adds two.

**Risk.** Medium, and it is all in the parameter-pointer table. `parameterPointers` is resolved once
by index; a stride change that misses one site reads the wrong control on the audio thread and does
so silently. Change the stride and the id table together, and let the existing `jassert` on unresolved
ids do its job.

**Done.** A slot exposes eight parameters, an old project loads with the two new ones defaulted.

### A3 · Per-model control labels in the UI

**Change.** The slot card reads its model's `controls` array and labels the six sliders from it,
hiding the ones the model does not use.

**Risk.** Low.

**Done.** Selecting the HM-2 shows Level, Color L, Color H and Distortion; selecting Boost shows one
knob and hides five.

### A4 · State and preset migration

**Change.** `ProjectState`'s pedal block gains the two controls. A saved `kind` of 0–6 still means
what it meant.

**Risk.** Medium — this is the one that breaks user projects if it is wrong. Add a fixture project
saved by the current build and assert it loads to the same rig.

**Done.** A project saved before this plan recalls the identical board.

### A5 · Retire the duplicated hint text

**Change.** `PedalModel` carries the blurb; `PedalFace` stops duplicating it.

**Done.** One sentence per pedal, in one place.

## Track B — The drive family: 30 models

The high-value slice. 11 overdrive-ish, 9 distortion-ish, 8 fuzz, 1 boost, 1 compressor — all on the
`shaper` engine, which already exists.

### B1 · Slew-rate limiter

**Change.** `dsp::SlewLimiter`, the RAT's actual mechanism: `dy/dt = clamp(dx/dt, ±max)`. A
signal-dependent low-pass, which is why an LM308 sounds like an LM308.

**Done.** The RAT model is audibly distinct from a hard clipper with the same curve.

### B2 · Gyrator twin-peak EQ

**Change.** Two resonant peaking sections with a shared control, the HM-2's Color L / Color H at
100 Hz and 1.2 kHz, and the Metal Zone's mid sweep.

**Done.** Both models respond to their own knobs.

### B3 · Antiderivative anti-aliasing

**Change.** First-order ADAA for the hard-clip and diode shapes. The spreadsheet calls it mandatory
for hard clipping and it is right: oversampling alone at these gains still folds.

**Risk.** Medium. ADAA needs the antiderivative of each shape and careful handling near
`x[n] ≈ x[n-1]`, where the difference quotient goes singular — fall back to direct evaluation inside
a small epsilon band. Also costs one sample of state per channel, so it must not change the existing
kinds' output unless deliberately enabled.

**Done.** A 1 kHz sine into the hard clipper shows measurably less energy above Nyquist/2 than the
same setting without it, and the existing seven kinds are unchanged with it off.

### B4 · The 30 model rows

**Change.** Fill them from the spreadsheet's circuit and blueprint columns.

**Two blueprint entries need fixing before they are used, not after.** The SD-1's
`f(x) = x − 0.2x² − 0.1x³` is unbounded — past |x| ≈ 2 it runs away instead of saturating, and it will
produce a very loud surprise. Clamp it or replace it with a bounded asymmetric shape. The Klon's
germanium `V_f ≈ 0.3 V` is right, but its parallel clean path is the thing that makes it a Klon;
`parallelClip` already exists for exactly this.

**Done.** 30 models play, each distinguishable from its neighbours.

## Track C — New engines

### C1 · Fractional delay interpolation

**Change.** 4-point Hermite in `dsp::DelayLine`. Underpins C2, C3 and C4 — do it first.

**Done.** A slowly swept delay produces no zipper artefacts.

### C2 · `modulation` engine — chorus, flanger, phaser

**Change.** BBD-style short delay with an LFO, feedback for the flanger, and a cascade of all-pass
sections for the Phase 90. 5 models.

**Note.** Phaser has no category row in the source workbook — see [Data corrections](#data-corrections).

### C3 · `delay` engine

**Change.** Wrap `dsp::Delay`, add BBD degradation (bandwidth loss and compander noise per repeat)
for the Carbon Copy. 3 models.

### C4 · `reverb` engine

**Change.** An FDN with prime delay lengths beside the existing comb-allpass `Reverb`, plus dispersive
all-pass chains for the spring tank. 4 models.

**Risk.** Medium on CPU. Four reverb slots at once is not a rig anyone builds, but nothing currently
stops it — see F1.

### C5 · `crush` engine

**Change.** Sample-and-hold downsampling, bit quantisation, and a resonant SVF after it. 5 models.

**Note.** The catalogue's snippet references an undeclared `count`; it is pseudo-code, not a
transcription target.

### C6 · `pitch` engine

**Change.** Phase vocoder for the POG2 and Whammy DT. 2 models.

**Risk.** High, and the highest in the plan. It is the only engine that **introduces latency**, which
means `PedalBoard` needs a `latencySamples()` and the processor must fold it into what it reports to
the host. Today the board reports none and the dry path assumes none. Both Tier 5.

**Done.** Pitch tracking is stable on single notes, the plug-in reports the added latency, and the
dry path stays aligned.

## Track D — The remaining 25 models

### D1 · `bassPreamp` engine

**Change.** Crossover, parallel clean and drive paths, and a voiced EQ. The amplifier's
`BassPathParameters` already does most of this — reuse rather than rewrite. 6 models.

### D2 · Model rows for the modulation, delay, reverb, crush and pitch families

**Change.** 19 rows.

### D3 · Category reconciliation

**Change.** The picker has five `PedalCharacter` shelves; the catalogue has twelve categories. Twelve
shelves for 55 tiles is a better fit than five. Decide once and change `PedalCharacter`.

## Track E — 55 faces

### E1 · Face rows from the catalogue

**Change.** The spreadsheet has a "Visual Aesthetics & Appearance" sentence for every unit — lime
green die-cast with a red LED, gold hammertone with oxblood knobs, glow-in-the-dark on sloped black
steel. Those are the colours; `PedalArt` already draws the hardware.

### E2 · Enclosure variety

**Change.** `PedalArt` draws one enclosure shape. The catalogue describes sloped wedges, oversized
fuzz boxes and compact Boss chassis. Three or four form factors, chosen per row.

**Risk.** Low, but this is where the lattice lesson applies: a noise cell count must divide its tile
edge exactly, or the finish beats against the pixel grid.

## Track F — Cost, latency, tests

### F1 · A cost policy

**Change.** Four slots × Tier 5 is not a viable rig. Use the catalogue's tier as a cost weight and
have the performance tier ([PluginProcessor.h:337](../Source/PluginProcessor.h#L337)) cap the total,
refusing or degrading the fifth expensive thing.

**This is a design decision, not only an implementation one:** silently degrading a pedal the user
chose is worse than telling them. Prefer showing the limit in the picker.

### F2 · Latency reporting

**Change.** `PedalBoard::latencySamples()`, folded into the processor's reported latency.

### F3 · Tests

**Change.** Every model index resolves to an engine and a face; every model's declared controls are a
prefix of the six; the seven original kinds are bit-identical before and after Track A; a fixture
project round-trips; no model produces NaN or a level above +6 dBFS from a 0 dBFS sine at every
extreme of its controls. That last one is the guard against another unbounded polynomial.

### F4 · Rendering check

**Change.** Extend `nts_art_renderer` to sheet all 55 faces.

## Data corrections needed in the source workbook

Carry these into the model table regardless of whether the spreadsheet is fixed:

- **Three pedals match no category**: PED-036 Sweet Tea (`Overdrive/Dist`), PED-040 Phase 90
  (`Phaser`), PED-055 Compressor Plus (`Compressor`). Phaser and Compressor have no category row at
  all, so the breakdown totals 52 against 55 rows.
- **The Category column is free text** — 24 distinct strings for 12 intended categories
  (`Delay` / `Delay (Digital)` / `Delay / Pitch`, `Reverb` / `Reverb (Digital)` / `Reverb (Spring)`).
  Normalise before importing, or the import inherits the mess.
- **Six of twelve stated tier averages are wrong** (Distortion 3 vs 2.75, Delay 3.5 vs 4.0, and four
  others). Column C is typed in rather than computed. Take tiers from the per-pedal column, never the
  summary.
- **The SD-1 polynomial is unbounded.** See B4.

## Open questions

1. ~~**Naming.**~~ **Decided: allusive names.** The modelled units get names of their own; the
   catalogue keeps the originals as the record of what each was derived from. What is being modelled
   is a circuit topology, which is nobody's trademark — the name on the box is. The rule is recorded
   at the head of the model table in `PedalBoard.cpp` so it is visible where the rows are written.
2. **Does the pitch engine earn its place?** C6 is the only engine that adds latency and the only one
   that forces a change to how the plug-in reports it, for two models out of 55. Deferring it costs
   two pedals and removes the plan's highest risk.
3. **Five shelves or twelve?** D3. Twelve is a better fit for 55 tiles; five is what is built.
4. **Do all 55 need to be selectable in every slot?** A reverb in slot 1 ahead of the amplifier is a
   legitimate thing to want and an unusual one. Filtering by slot is easy and might be unwelcome.

## Out of scope, and why

- **Stereo pedals and true-stereo modulation.** The board is mono into the amplifier; changing that
  is a signal-flow change, not a pedal.
- **Per-pedal impulse responses or neural captures of these units.** The `neural` engine already
  plays captures, and a capture of a TS9 is a better TS9 than any coefficient table — but it is a
  different workflow, and the Captures page owns it.
- **Drag-to-reorder slots.** Still real, still unrelated.
- **Modelling the spreadsheet's "Iconic Applications" column.** It is useful prose for the picker
  blurbs and nothing more.
