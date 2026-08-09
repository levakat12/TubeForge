# Bass topology plan: six bass-native voicings on the existing amplifier graph

An implementation-ready plan for turning `Bass_Amp_Topologies_DSP_Reference.xlsx` — six catalogued bass
amplifiers across four topology classes — into playable voicings in TubeForge, plus one voicing the
workbook does not contain and should.

The headline decision is in [Decisions](#decisions-and-why): **six voicings, and four of them are
parameter tables with no new audio code.** The other two are blocked on three shared DSP gaps, and
those gaps are the actual content of this plan.

Line references are against the working tree at the commit that added the pedal catalogue; re-check
them before editing.

## Status

| Track | Items | Effort | State |
|---|---|---|---|
| A — The four free voicings | A1–A4 | 2.0 d | ✅ **Complete** |
| B — Gap 1: waveshape selection through the amplifier | B1–B3 | 1.5 d | ✅ **Complete** |
| C — Gap 2: a real bi-amp | C1–C3 | 1.5 d | ✅ **Complete** |
| D — Gap 3: parallel dry blend, correctly aligned | D1–D3 | 1.5 d | ✅ **Complete** |
| E — Liveries, shelving and the instrument filter | E1–E3 | 1.5 d | ✅ **Complete** |
| F — Tests | F1–F4 | 1.5 d | ✅ **Complete** |
| G — Documentation | G1 | 0.5 d | ✅ **Complete** |
| H — Song Match hookup *(added during implementation)* | H1–H4 | 0.5 d | ✅ **Complete** |

**Coverage: 100%** — 10.5 of 10.5 days, including Track H, which the plan did not foresee. Nothing is outstanding.

**All six voicings play.** The amplifier can clip with something other than a valve, the bass path
is a real bi-amp, and the parallel dry path is latency-aligned. All 23 test binaries pass. The seven
original voicings are proven bit-identical by a regression guard that did not exist before (F1) — and
still are, after Track B rewrote all three saturators, Track C rewrote the band sum and Track D added
a parallel path around the whole amplifier.

The picker filters by instrument, the shelves carry thirteen tiles, and the four new controls are laid
out on the tone page — **verified by launching the standalone and looking at it**, which is how the
worst bug in this whole plan was found (see E3a).

Song Match searches the new voicings correctly — **Track H**, which the plan did not foresee and which
existed because adding a voicing to the enum is not the same as adding it to the product.

**A4 shipped after being reframed.** It was blocked for as long as it was read as "four factory
presets"; it is not, and the reframing is in A4 below.

### What implementation changed

Two things the plan did not predict, one of them a defect in the plan itself.

**`applyBassCabinet` had to be taught about affinity, and this was not optional.** The bass instrument
reading of a livery ([FaceplateArt.cpp](../Source/ui/FaceplateArt.cpp)) darkens the covering, forces
`GrilleWeave::perforated`, desaturates the piping and appends " Bass" to the badge. That is right for a
guitar livery being restated as bass gear and wrong for one already drawn as a bass amplifier: it
flattened all four new weaves to one, undid their colour, and badged a bass head "Valve Flagship Bass".
`faceplateStyle` now applies it only to `TopologyAffinity::either`. **This is the first real consumer of
the affinity field**, ahead of the picker filter it was added for.

**A4 was blocked by its own framing, twice, before the framing turned out to be the bug.** Calling the
four switch settings "factory presets" sent the work looking for a preset mechanism that does not exist
and should not have been built. They are switches on two amplifiers; a switch is a control; the project
already saves controls. See A4.

**Ship Track A + E1 on their own** — done, and it delivers four bass-native voicings with no
audio-thread changes at all.

## What the research changed

The workbook is prose with no numbers in it. Every value below had to be reasoned or found, and doing
that turned up **seven corrections to the first proposal and one outright error in the source
workbook**. They are listed first because they are the reason this plan is not the same document as
the initial pitch.

### 1. The SVT does have a global feedback loop. The workbook says it does not.

`E4` states "No negative feedback loop." This is wrong. In the SVT power amp the feedback taps from
the **output transformer secondary and returns to the cathode of the phase inverter** — the
conventional arrangement, and the thing that sets the amp's damping factor and evens out transformer
nonlinearity.

**Consequence:** `powerAmp.feedback` goes to **0.32**, not the near-zero a literal reading would have
given. Encoding the workbook's claim would have produced a loose, flabby output stage — the opposite
of the controlled roar the same row describes two columns later. The row contradicts itself and the
schematic settles it.

### 2. The SVT has a driver stage between the phase inverter and the power tubes.

The chain is 12AX7 phase inverter → **two 12BH7 drivers, each driving three 6550s** → output. TubeForge
models phase inverter → power amp with nothing between them.

**Consequence:** the driver's gain and its own compression have to be folded into
`PhaseInverterParameters`, which is why `drive` is 1.75 and `headroom` 0.9 rather than the 1.6/0.85 a
bare phase inverter would want. Recorded as a deliberate simplification, not an oversight — a third
tier in the power section is a structural change for one voicing.

### 3. Exact figures for the Ultra Lo and Ultra Hi switches.

Ultra Lo is **+2 dB at 40 Hz with a −10 dB cut at 500 Hz**; Ultra Hi is **+6 dB at 5 kHz**. The mid cut
is the dominant term and the reason the switch reads as "more bass" without adding low-end power —
which is exactly what its designer intended.

**Consequence:** both switches are **off** in the default voicing, and the figures give two extra
factory presets for free (`Valve Flagship — Ultra Lo`, `Valve Flagship — Ultra Hi`) that are among the
most recognisable bass sounds there are. `PreEqParameters` already carries `lowShelfDb` and
`midEmphasisDb`, so these cost nothing but two rows.

### 4. The GK crossover runs 100 Hz to 1 kHz, centred at 500 Hz.

The first proposal put it at 320 Hz. The real range reaches **1 kHz**, and the manufacturer's
recommended starting point is **500 Hz** — well above anything the current bass path can do.

**Consequence:** two changes. The voicing default becomes 500 Hz, and `BassPathParameters::crossoverHz`
needs a range reaching 1 kHz, against the 120–220 Hz the existing bass presets use. This is the number
that makes a bi-amp a bi-amp: splitting high means the **whole note body stays clean** and only the
attack transient distorts.

### 5. The B-15's tone stack is Baxandall with no mid control at all.

The first proposal put it on `bassSemiParametric`. It is a shared **Baxandall** stack — active, and
two-band. There is no mid knob to model.

**Consequence:** `activeThreeBand` with the mid pinned at 0.5. And the distinction is real rather than
pedantic: the **SVT** keeps `bassSemiParametric` because its mid *is* a stepped frequency selector,
which is a semi-parametric control. Two Ampegs, two different stack types, for a reason visible on the
front panel.

### 6. The B-15 uses a split-load phase inverter, and its bias depends on the year.

**25 W cathode-biased before mid-1965; 30 W fixed-bias after.** A split-load (cathodyne) inverter has
poor drive capability and clips asymmetrically as it runs out of headroom.

**Consequence:** target the **25 W cathode-biased** version explicitly and say so in the blurb —
`biasCharacter` **+0.14** is only correct for that one. The cathodyne justifies `headroom` **0.55**
(lower than any other voicing here) and `differentialImbalance` **0.18**.

### 7. The Darkglass drive is a *single* overdriven CMOS stage, not a cascade.

The first proposal specified three cascaded clipping stages. The B3K-family circuit deliberately uses
**one** overdriven CMOS stage, which is what separates it from the cascaded-inverter designs it was
reacting against. Its Blend also has a specific topology: the **clean path stays at unity** while
`Level` sets the distorted side.

**Consequence:** `stageCount` drops from 3 to **2** — one hard clipping stage plus a makeup stage — and
the drive concentrates rather than spreads. Three soft-ish stages and one hard one are audibly
different things, and the first proposal had the wrong one.

### 8. The Aguilar switches have figures too.

**Deep: +5 dB at 30 Hz. Bright: +5 dB at 5–7 kHz.** Three 12AX7s into twelve lateral MOSFETs. Both
switches off in the default voicing, both available as preset variants.

## What exists today

`Topology` ([TraditionalAmp.h:37](../engine/amp/include/nts/amp/TraditionalAmp.h#L37)) is seven
voicings shared between both instruments. `makeOriginalPreset(topology, instrument)`
([TraditionalAmp.cpp:165](../engine/amp/src/TraditionalAmp.cpp#L165)) returns the bass *retune* of a
guitar circuit: lower cuts, `bassSemiParametric` instead of `passiveCoupled`, a different mid centre,
and one identical block at the end for all seven:

```cpp
p.bass = { 170.0f, 0.62f, 0.52f, 5.0f, 1.0f, false };
```

**There is no bass-native voicing in the product.** "Original Class-A Bass" is a Vox circuit with the
low cut moved down. That is the gap.

The header states the property this plan depends on:

> A topology is not a switch inside the DSP: it selects a block of values in `makeOriginalPreset` and
> nothing else, so a voicing is reachable by hand from any other and adding one costs no audio code.

That is true for four of the six. It is not true for the other two, and the three reasons are below.

### Gap 1 — every nonlinearity in the amplifier is hardcoded `tanh`

| Site | Line |
|---|---|
| `ResponsivePreampStage` saturator | [TraditionalAmp.cpp:680](../engine/amp/src/TraditionalAmp.cpp#L680), [:689](../engine/amp/src/TraditionalAmp.cpp#L689) |
| `PhaseInverter` | [TraditionalAmp.cpp:789](../engine/amp/src/TraditionalAmp.cpp#L789) |
| `PowerAmp` | [TraditionalAmp.cpp:841](../engine/amp/src/TraditionalAmp.cpp#L841) |

No parameter makes any stage clip like anything other than a valve. Both solid-state amps in the
workbook are *defined by not being tanh*, so neither is reachable at any setting.

### Gap 2 — `BassPathParameters` is a fixed split, not a bi-amp

```
crossover → low: compressor + optional tanh(x * 1.35f) → blend
            high: the whole amplifier chain
```

`{ crossoverHz, cleanBlend, lowCompression, highDriveDb, lowMono, lowSaturation }` has no low-band
drive, no per-band output level, and no per-band voicing. The `lowSaturation` flag applies a hardcoded
`tanh(x * 1.35f)` ([TraditionalAmp.cpp:1214](../engine/amp/src/TraditionalAmp.cpp#L1214)) — a magic
constant that is not exposed and cannot be changed.

### Gap 3 — there is no parallel dry blend around the drive path

Two controls look like one and neither is:

- `bass.cleanBlend` blends the **low band** clean — after the crossover, so it never carries the
  fundamental's harmonics.
- `cabinet.bassDiBlend` blends the cabinet's **input** against its output
  ([TraditionalAmp.cpp:1105](../engine/amp/src/TraditionalAmp.cpp#L1105)) — a cab-bypass DI blend, still
  fully distorted.

A full-range, pre-distortion dry tap is a third thing, and it is what makes a bass distortion usable.

## Decisions, and why

**Append to the shared enum; filter the picker, not the parameter.** The choice index *is* the
`Topology` value — [PluginProcessorHost.cpp:292](../Source/PluginProcessorHost.cpp#L292) builds the
parameter's string list from `topologyName` in enum order, and
[PluginProcessorHost.cpp:48](../Source/PluginProcessorHost.cpp#L48) packs factory programs as
`instrument * topologyCount + topology`. Six appended voicings take `topologyCount` **7 → 13** and
`factoryProgramCount` **14 → 26**, both derived rather than written down. Voicings gain an *instrument
affinity* that the **picker** filters on while the choice parameter keeps all thirteen, so host
automation lanes stay valid and a guitarist never sees "Solid-State Bi-Amp". `faceplateStyle` already
takes `(topologyIndex, instrumentIndex)`, so the art side is already shaped for this.

**Six voicings, not the workbook's six.** One of its amps is cut and one voicing it lacks is added —
see [What was cut](#what-was-cut-and-why).

**Order the enum so the free voicings come first.** Indices 7–10 need no audio code; 11–12 are blocked
on Tracks B–D. Since enum order is permanent and append-only, putting the free ones first makes Track A
shippable in isolation rather than as a half-finished prefix of something larger.

**Every new field defaults to today's behaviour.** `dsp::Waveshape shape` defaults to
`hyperbolicTangent`, the new bass fields default to the current fixed values, and the dry blend defaults
to zero. The seven existing voicings must be **bit-identical** after all seven tracks land, and F1 is
the test that says so.

## The six voicings

| # | Key | Name | Source | Needs |
|---|---|---|---|---|
| 7 | `valveFlagship` | Valve Flagship | Ampeg SVT | — |
| 8 | `cathodeVintage` | Cathode Vintage | Ampeg B-15, 25 W cathode-biased | — |
| 9 | `hybridMosfet` | Hybrid MOSFET | Aguilar DB751 | — |
| 10 | `shortPathGrind` | Short-Path Grind | Orange AD200B | — |
| 11 | `solidStateBiAmp` | Solid-State Bi-Amp | GK 800RB | ✅ shipped |
| 12 | `cmosModern` | CMOS Modern | Darkglass Microtubes | ✅ shipped |

Names are descriptive circuit facts, matching the register of the existing seven (`Sagging Rectifier`,
`Class-A Chime`) rather than the pedal catalogue's allusive naming. These are classes of circuit, so
there is nothing to allude to.

### 7 · Valve Flagship

What separates this from `vintageBloom` on bass is not gain: it is that the preamp stages **keep their
bottom octave** (`lowCutHz` 32, where every guitar-derived voicing cuts at 38–52), so the low end
saturates rather than being filtered out ahead of the valves. `frequencySaturation` at 0.30 is the
workbook's "frequency-dependent sag".

```cpp
p.stageCount = 3;
p.preEq = { 30.0f, 14000.0f, 0.35f, 1.0f, false, 0.0f, true, 2.5f };
p.stages[0] = { 9.0f,  0.05f, 0.14f, 32.0f, 11000.0f, -6.0f, 4, 0.30f, 0.30f, 0.20f, 0.35f };
p.stages[1] = { 12.0f, -0.04f, 0.17f, 34.0f, 10000.0f, -6.0f, 4, 0.32f, 0.32f, 0.22f, 0.38f };
p.stages[2] = { 14.0f, 0.06f, 0.20f, 38.0f,  9000.0f, -6.0f, 4, 0.34f, 0.34f, 0.24f, 0.42f };
p.toneStack = { ToneStackType::bassSemiParametric, 0.55f, 0.60f, 0.50f, 700.0f, 0.70f };
p.phaseInverter = { 1.75f, 0.90f, 0.15f, 0.07f, 0.22f };   // absorbs the 12BH7 driver — see R2
p.powerAmp = { -3.5f, 0.50f, 0.55f, 0.55f, -0.06f, 0.45f, 0.70f, 0.32f, 45.0f, 620.0f };
p.bass = { 140.0f, 0.35f, 0.40f, 4.0f, 1.0f, true };
p.cabinet.bassDiBlend = 0.15f;
p.outputGainDb = -5.0f;
```

`cleanBlend` is deliberately low at 0.35: this amp's identity is that *everything* goes through the
valves. A high clean blend would make it a DI with a valve garnish.

**Two preset variants, free:** `Ultra Lo` sets `lowShelfEnabled = true, lowShelfDb = 2.0` and
`midEmphasisDb = -10.0`; `Ultra Hi` adds `pickEmphasisDb = 6.0`. See R3.

**Livery:** black tolex, silver-blue grille, chrome-bezel panel, blue jewel lamp.
**Blurb:** "Three valve stages that keep their bottom octave, into a supply that blooms under a chord.
The reference big-amp sound."

### 8 · Cathode Vintage

`memoryAmount` 0.55 and `sag` 0.70 are the whole voicing — this amp is compressing constantly. The
9 kHz `highCutHz` is not a mistake: it has no top end, and that is why it sits under a vocal.

```cpp
p.stageCount = 2;
p.preEq = { 35.0f, 9000.0f, 0.20f, -1.5f, true, 2.0f, false, 0.0f };
p.stages[0] = { 11.0f, -0.07f, 0.22f, 30.0f, 7000.0f, -4.0f, 4, 0.45f, 0.20f, 0.10f, 0.52f };
p.stages[1] = { 13.0f,  0.06f, 0.28f, 30.0f, 6000.0f, -4.0f, 4, 0.48f, 0.22f, 0.08f, 0.58f };
p.toneStack = { ToneStackType::activeThreeBand, 0.60f, 0.50f, 0.35f, 500.0f, 0.50f };  // Baxandall — R5
p.phaseInverter = { 1.3f, 0.55f, 0.24f, 0.18f, 0.14f };    // split-load cathodyne — R6
p.powerAmp = { -1.0f, 0.66f, 0.30f, 0.70f, 0.14f, 0.30f, 0.60f, 0.10f, 60.0f, 780.0f };
p.bass = { 200.0f, 0.25f, 0.55f, 3.0f, 1.0f, true };
p.cabinet.bassDiBlend = 0.10f;
p.outputGainDb = -4.0f;
```

**This is the voicing the workbook does not have, and it is the most valuable of the six.** A 30-watt
flip-top valve combo is the most-recorded bass sound in existence — the entire Motown catalogue and most
session work where the bass is felt rather than heard. The workbook is a rock and metal document; it has
no entry anywhere near this corner, and neither do the current seven.

**Livery:** dark blue-grey tolex, cream basketweave grille, painted cream panel.
**Blurb:** "Twenty-five watts, cathode-biased, no top end and no headroom. Compresses the moment you dig
in. The sound of a hundred records."

### 9 · Hybrid MOSFET

`saturation` 0.08 against `damping` 0.95 and `feedback` 0.65 is the "near-infinite clean headroom" the
workbook describes. The low stage drives mean the valves contribute asymmetry without ever clipping.

```cpp
p.stageCount = 3;
p.preEq = { 28.0f, 19000.0f, 0.40f, 0.5f, false, 0.0f, true, -1.0f };
p.stages[0] = { 6.0f,  0.03f, 0.09f, 26.0f, 16000.0f, -3.0f, 4, 0.20f, 0.12f, 0.06f, 0.16f };
p.stages[1] = { 7.0f, -0.02f, 0.11f, 26.0f, 15000.0f, -3.0f, 4, 0.20f, 0.12f, 0.06f, 0.18f };
p.stages[2] = { 8.0f,  0.03f, 0.13f, 26.0f, 14000.0f, -3.0f, 4, 0.22f, 0.12f, 0.06f, 0.20f };
p.toneStack = { ToneStackType::bassSemiParametric, 0.55f, 0.48f, 0.55f, 600.0f, 0.65f };
p.phaseInverter = { 1.05f, 1.35f, 0.05f, 0.02f, 0.42f };
p.powerAmp = { -1.5f, 0.08f, 0.95f, 0.03f, 0.01f, 0.55f, 0.50f, 0.65f, 18.0f, 160.0f };
p.bass = { 120.0f, 0.50f, 0.30f, 2.0f, 1.0f, false };
p.cabinet.bassDiBlend = 0.35f;
p.outputGainDb = -5.0f;
```

**Two preset variants, free:** `Deep` sets `lowShelfEnabled = true, lowShelfDb = 5.0` with `preEq.lowCutHz`
held at 28; `Bright` sets `pickEmphasisDb = 5.0`. See R8.

This is where slap, funk and modern session playing live, and it is the closest thing to a "does what you
tell it" voicing in the set. It is also the natural home for a scooped-slap **preset**, which is why no
topology index was spent on one.

**Livery:** milled aluminium face, dark steel body, tight grille, white lettering, no lamp colour.
**Blurb:** "Three valve stages of warmth in front of a power section that will not distort. Hi-fi, fast,
and enormous underneath."

### 10 · Short-Path Grind

The *highest* preamp `lowCutHz` of the six at 55 Hz. The workbook is right that this amp has no
sub-bass, and that is the character rather than a deficiency — cutting the fundamental is also what
stops the grind turning to mud. Note the stack type: this is the only bass voicing where
`passiveCoupled` is correct, because the circuit genuinely has a passive interacting stack.

```cpp
p.stageCount = 2;
p.preEq = { 55.0f, 12000.0f, 0.45f, 1.5f, false, 0.0f, true, 3.5f };
p.stages[0] = { 14.0f, -0.09f, 0.30f, 60.0f, 10000.0f, -6.0f, 4, 0.42f, 0.35f, 0.28f, 0.50f };
p.stages[1] = { 17.0f,  0.07f, 0.34f, 55.0f,  9000.0f, -6.0f, 4, 0.44f, 0.36f, 0.30f, 0.54f };
p.toneStack = { ToneStackType::passiveCoupled, 0.50f, 0.62f, 0.48f, 480.0f, 0.85f };
p.phaseInverter = { 1.85f, 0.60f, 0.30f, 0.16f, 0.08f };
p.powerAmp = { -3.0f, 0.75f, 0.28f, 0.62f, -0.10f, 0.50f, 0.55f, 0.07f, 38.0f, 560.0f };
p.bass = { 220.0f, 0.30f, 0.45f, 6.0f, 1.0f, true };
p.cabinet.bassDiBlend = 0.10f;
p.outputGainDb = -4.0f;
```

**Livery:** orange tolex, picture-frame edge, white panel, black pointer knobs.
**Blurb:** "The shortest signal path here. No bottom octave and no restraint — it fuzzes out early and
cuts through anything."

### 11 · Solid-State Bi-Amp — *needs B and C*

**`memoryAmount` at 0.03 across every stage is the point.** A transistor has no supply memory, and every
valve voicing in the product runs 0.16 or more. Same for `sag` 0.02 and `damping` 0.92 — a solid-state
amp's high damping factor is literally what its spec sheet advertises.

```cpp
p.stageCount = 2;
p.preEq = { 40.0f, 18000.0f, 0.70f, 4.0f, true, -1.0f, true, -2.5f };
p.stages[0] = { 7.0f, 0.0f, 0.03f, 42.0f, 17000.0f, -3.0f, 8, 0.02f, 0.05f, 0.02f, 0.03f };
p.stages[1] = { 9.0f, 0.0f, 0.03f, 42.0f, 16000.0f, -3.0f, 8, 0.02f, 0.05f, 0.02f, 0.03f };
// Track B: both stages and the power amp take dsp::Waveshape::hardClip with ADAA.
p.toneStack = { ToneStackType::activeThreeBand, 0.55f, 0.40f, 0.62f, 800.0f, 1.10f };
p.phaseInverter = { 1.0f, 1.40f, 0.01f, 0.0f, 0.45f };   // there isn't one; get out of the way
p.powerAmp = { -2.0f, 0.35f, 0.92f, 0.02f, 0.0f, 0.60f, 0.40f, 0.70f, 15.0f, 120.0f };
// Track C: crossover 500 Hz, low band 0 dB drive / 0 dB level, high band +8 dB drive / -2 dB level.
p.bass = { 500.0f, /* extended fields */ 0.30f, 8.0f, 1.0f, false };
p.cabinet.bassDiBlend = 0.30f;
p.outputGainDb = -5.0f;
```

**500 Hz is the single most important number in this voicing** and is more than double anything the
current bass path can do. See R4.

**Livery:** brushed rack chassis, horizontal heat-sink fins, red power lamp, silver-cap knobs.
**Blurb:** "Two power sections either side of a high crossover. The lows stay clean while the highs hit
the rails and clank."

### 12 · CMOS Modern — *needs B and D*

One hard clipping stage, not a cascade (R7). The 150 Hz stage `lowCutHz` keeps the distortion engine off
the fundamental, and Track D's dry blend brings that fundamental back untouched.

```cpp
p.stageCount = 2;
p.preEq = { 45.0f, 20000.0f, 0.85f, 5.0f, true, -2.0f, true, 3.0f };
p.stages[0] = { 24.0f, 0.02f, 0.06f, 150.0f, 9000.0f, -12.0f, 8, 0.05f, 0.10f, 0.03f, 0.05f };
p.stages[1] = {  4.0f, 0.0f,  0.02f, 120.0f, 8000.0f,  -3.0f, 4, 0.03f, 0.05f, 0.02f, 0.03f };
// Track B: stage 0 takes dsp::Waveshape::hardClip with ADAA; stage 1 stays linear (makeup).
p.toneStack = { ToneStackType::activeThreeBand, 0.60f, 0.42f, 0.60f, 900.0f, 1.30f };
p.phaseInverter = { 1.0f, 1.45f, 0.02f, 0.0f, 0.50f };
p.powerAmp = { -1.0f, 0.06f, 0.95f, 0.0f, 0.0f, 0.55f, 0.45f, 0.75f, 12.0f, 100.0f };
// Track D: full-range pre-distortion dry blend at 0.40, latency-aligned.
p.bass = { 150.0f, /* extended fields */ 0.60f, 0.0f, 1.0f, false };
p.cabinet.bassDiBlend = 0.45f;
p.outputGainDb = -6.0f;
```

**Without Track D this voicing has no low end at all** and is not shippable as an approximation, unlike
the others. The dry blend is not a refinement here; it is half the circuit.

**Livery:** matte anodised aluminium, laser-etched graphics, white LED buttons, no grille.
**Blurb:** "A high-gain silicon engine on the upper band only, over a fundamental it never touches.
Tight, metallic, and unforgiving."

### What was cut, and why

**The Mesa Bass 400+.** Without a graphic EQ block it is Valve Flagship with more headroom and a
glassier top, and both are reachable from Valve Flagship's own controls plus the post EQ. It is the one
workbook amp whose identity lives almost entirely in a feature the product does not have, so a permanent
enum index and a livery buy the least. Revisit if a graphic EQ ever lands.

**A scooped slap/funk voicing.** It is an EQ setting on Hybrid MOSFET, not a circuit. Ship it as a
preset.

## Track A — The four free voicings

### A1 · Enum, keys, names and the label table

**Change.** Append `valveFlagship`, `cathodeVintage`, `hybridMosfet`, `shortPathGrind` to `Topology`;
`topologyCount` 7 → 11 for this track (13 after Track E). Add four rows to `topologyLabels`
([TraditionalAmp.cpp:135](../engine/amp/src/TraditionalAmp.cpp#L135)) — the `static_assert` there is
already the guard.

**Risk.** Low. Append-only, and `topologyFromKey` returning `nullopt` for an unknown key already keeps a
forward-dated preset partially useful.

**Done.** `factoryProgramCount` reports 22, and every new key round-trips through
`topologyKey`/`topologyFromKey`.

### A2 · `makeOriginalPreset` cases

**Change.** Four `case` bodies from the blocks above.

**The one real trap in this track:** [TraditionalAmp.cpp:303](../engine/amp/src/TraditionalAmp.cpp#L303)
currently assigns one identical `p.bass` block to *every* bass preset after the switch. Each new voicing
needs its own, so that tail must become a default the `case` bodies may already have overridden. Get it
wrong and all four silently collapse back to the same bass path — and it will still compile, still run,
and still sound plausible.

**Risk.** Medium, entirely because of the above.

**Done.** Each of the four produces a distinct `AmpParameters`, and A2's test in F2 proves it.

### A3 · Guitar readings

**Change.** `makeOriginalPreset(newTopology, Instrument::guitar)` must return *something*. Same block
with the cuts raised and the bass tail skipped, exactly as the existing seven do in reverse.

**Risk.** Low. The picker filter in E3 means nobody normally sees these, but a host that writes topology
index 8 with instrument 0 must not get an uninitialised preset.

### A4 · The four panel switches — ✅ **complete, after being reframed**

**They were never presets, and that is why this was blocked.** The plan called Ultra Lo, Ultra Hi,
Deep and Bright "factory presets" and then discovered there is no factory preset mechanism: programs
are packed as `instrument * topologyCount + topology`, `.ntone` is a user-save format, and the profile
library is `nts::ecosystem::TonePackage` — signed packages with a manifest, an author, a quality tier
and signature verification, which is an ecosystem for sharing rigs and not a home for four EQ
settings. Two rounds of investigation kept landing on "this needs a format decision".

It needed no format at all. **These are switches on the front of two particular amplifiers**, and a
switch is a control — the project already saves controls. `PanelSwitch` is one choice parameter,
applied by `applyPanelSwitch` after the voicing block, saved and automatable like anything else.

**What it needed instead was three filters that did not exist.** The pre-EQ built its shelf at a
hardcoded 140 Hz and its mid bell at 950 Hz, and had no high shelf whatsoever — so none of the four
researched figures was reachable. A −10 dB cut at 950 Hz is not an approximate Ultra Lo, it is a
different control. `PreEqParameters` now carries `lowShelfHz`, `midEmphasisHz` and a disabled-by-default
high shelf, every default reproducing the literal it replaced.

| Switch | Voicing | Figures |
|---|---|---|
| Ultra Lo | Valve Flagship | +2 dB @ 40 Hz **and −10 dB @ 500 Hz** |
| Ultra Hi | Valve Flagship | +6 dB @ 5 kHz |
| Deep | Hybrid MOSFET | +5 dB @ 30 Hz |
| Bright | Hybrid MOSFET | +5 dB @ 5–7 kHz |

**One flat parameter list for every voicing, filtered only in the interface** — the same rule the
topology list follows, for the same reason: a per-voicing list would make an automation lane's meaning
depend on the topology parameter's value. `applyPanelSwitch` ignores a switch the current voicing does
not carry, so an out-of-panel pairing from a preset or a lane is inert rather than an amplifier that
never existed. The control is hidden entirely on the eleven voicings that offer nothing, rather than
shown holding "Standard" — a box with one inert entry reads as broken rather than as absent.

**A measurement note worth keeping.** The first version of the tests asserted each shelf at its stated
figure and two of three failed: an RBJ shelf reaches exactly *half* its dB at its corner frequency, so
a +5 dB shelf at 30 Hz measures +2.5 dB there. The bounds are set from that arithmetic, not from slack.

## Track B — Gap 1: waveshape selection

### B1 · `shape` on the preamp stage and power amp — ✅ **complete**

**Change.** `dsp::Waveshape shape` and `bool antialiasedSaturation` on `PreampStageConfig`;
`dsp::Waveshape shape` on `PowerAmpParameters`; all three hardcoded `tanh` sites routed through the shape
dispatch; both serialised, both defaulting to the valve curve so a preset written before the fields
existed reads back exactly as it sounded.

The bias-offset hazard the plan flagged was real and is handled: `setConfig` now derives
`biasOffsetPositive`/`biasOffsetNegative` **through whichever curve the stage will actually run**. An
offset computed through `tanh` while the stage clips with a clamp does not centre the stage, it displaces
it, and the DC blocker downstream then spends every note's attack recovering from a step it should never
have seen.

**Two things the plan did not have:**

- **`Oversampler::processIndexed`.** ADAA is stateful per channel and the existing `process` gives its
  callback no channel index. The traversal is channel-major, so a caller *could* recover the channel by
  counting calls and dividing by the block length — which works today and breaks silently the moment that
  loop is restructured or vectorised. An indexed overload costs nothing and cannot go quietly wrong.
- **A range guard on the stored shape.** `dsp::shapeSample` switches on the enum and returns its input
  unchanged for a value outside it, so casting a stored integer straight to the enum turns a distorting
  stage into a silently *linear* one — no error, no NaN, an amplifier that has stopped being an
  amplifier. A preset written by a later build with more curves in the enum is exactly how that arrives.
  `shapeFromIndex` falls back to the valve curve, the same policy `topologyFromKey` already applies.

**Done.** The seven existing voicings are still bit-identical under F1 after all three saturators were
rewritten, and a `hardClip` stage is measurably linear below its threshold where a valve stage is
already compressing.

### B2 · ADAA for the hard shapes — ✅ **complete, and the measurement changed the design**

**Intended change.** Enable `dsp::AntialiasedWaveshaper` on stages whose shape is hard-edged, *and* set
`oversamplingFactor = 8` on both solid-state voicings — the plan asserted both halves were required.

**What was measured.** Sweeping ADAA against asymmetry and oversampling factor, as a ratio of
antialiased fold-back to plain fold-back at the 1100 Hz probe:

| asymmetry | ×1 | ×2 | ×4 | ×8 |
|---|---|---|---|---|
| 0.00 | 0.16 | 0.29 | 1.37 | 0.97 |
| 0.06 | 0.70 | 0.30 | 1.39 | 0.97 |
| 0.30 | 1.51 | 0.36 | 1.18 | 0.99 |

**The ×4 and ×8 columns are not results.** The ADAA figure is pinned near 2.4 × 10⁻⁴ in every run
regardless of factor, and the *plain* figure at ×4 and ×8 is already at that same value — that number is
the probe's floor, because unwindowed correlation against a strong fundamental leaks into every bin.
A broadband sum was tried as an alternative and is worse: leakage then dominates every bin and every
configuration reads 0.022–0.035, which is the trap the pedal plan's Track C recorded.

**Two conclusions, both acted on:**

1. **ADAA is real at ×2** — about a threefold reduction, stable across asymmetry. That is what the test
   asserts, at asymmetry 0.06 because that is what the solid-state voicings run.
2. **8× oversampling alone already puts fold-back below the floor, so the solid-state voicings do not
   enable ADAA.** The plan was wrong to require both. Paying half a sample of delay and some top end for
   an improvement that cannot be measured is not a trade. The field stays — it earns its place at lower
   factors, which is where a performance tier would put a stage.

**The ×1 row is the polarity-switching limitation, seen directly.** The stage's curve is
`shape(x · polarity)` where the polarity switches on the sign of the biased input; ADAA integrates
`shape`, so an interval straddling that breakpoint is averaged under the wrong polarity. At ×1 those
intervals are a large fraction of all of them, and ADAA goes from helping at asymmetry 0 to actively
hurting by 0.3. Documented in the saturator rather than fixed: splitting the interval at the crossing
costs a branch and a root-find in the hottest loop in the amplifier.

**One thing the plan did not anticipate: the power amp needed ADAA and could not be given oversampling.**
`PowerAmp::process` runs at base rate, and it cannot cheaply be wrapped in an oversampler because the
supply sag and global feedback make it an IIR loop around the nonlinearity rather than a memoryless
curve. So a hard clipper there folds with nothing to catch it, and ADAA is engaged automatically for any
non-valve shape — a mitigation rather than a solution, and labelled as such in the header.

### B3 · Cost — ✅ **complete**

A stereo amplifier with two 8× hard-clip stages and a hard-clip output stage is held to the same 50% of
the callback budget that the 4× valve configuration is held to, in `nts_amp_tests`, and passes. Doubling
the oversampling factor does not double the whole graph's cost — the cabinet convolution, tone stack and
power section are unchanged.

Written as a **guard rather than a measurement**: the two voicings blocked on Tracks C and D will land in
this graph, and the budget has to have been checked before they arrive rather than after a user finds it.

## Track C — Gap 2: a real bi-amp

### C1 · Extend `BassPathParameters` — ✅ **complete**

`lowDriveDb`, `lowLevelDb`, `highLevelDb` and `dsp::Waveshape lowShape` added and serialised, each
defaulting to the value the path behaved as before it existed. The hardcoded `tanh(x * 1.35f)` is gone;
`lowDriveDb` defaults to 2.6 dB, which is that same 1.35×.

**The crossover ceiling went 500 Hz → 1 kHz, and it was worse than the plan thought.** The host
parameter was `Range { 60, 500 }` — so the manufacturer's recommended 500 Hz starting point sat
*exactly on the top rail* and the entire upper half of the real control was missing.

**The cost, stated rather than discovered:** a host automation lane stores a normalised position, so an
existing lane written against 60–500 now points at a higher frequency. Saved projects and presets are
unaffected — both store the value in Hz. Accepted deliberately, and recorded at the parameter site: the
alternative is a voicing whose defining number is unreachable.

### C1a · A ceiling nobody was watching — *found while wiring C3*

Adding three host parameters took `ampControlIds` to **54**, and `nts::state::validate` rejects an
`ampControls` array longer than **64**. Crossing that does not truncate a save or warn — it makes
**every saved project fail to load** as "Amp control state is invalid". A valid project the user can no
longer open, caused by adding a knob.

The bound was a literal in `ProjectState.cpp` with nothing connecting it to the list in
`PluginProcessorInternal.h` that has to respect it. It is now `nts::state::maximumAmpControls`, and the
writing side carries a `static_assert` against it — verified to fire, with its message, by temporarily
lowering the constant. Track D's dry blend takes this to 55; **nine entries of headroom remain.**

A runtime test was written first and thrown away: `PluginProcessorInternal.h` says in its own header
comment that nothing outside `Source/PluginProcessor*.cpp` should include it, and a test that has to
breach that boundary to check an invariant is the wrong tool for an invariant that is knowable at
compile time.

### C2 · The LR4 phase-modulation hazard — ✅ **complete**

Recorded on `BassPathParameters` itself, where the fields are, so the next person cannot add an envelope
follower to band gain without reading why they should not. Summary below unchanged.


`dsp::LinkwitzRileyCrossover` is two cascaded biquads per band — **fourth order**. Published guidance on
dynamic multiband processing is that where band levels change during playback, a fourth-order
Linkwitz-Riley recombination can be phase-modulated by **up to ±180°**. LR4 sums flat only while both
bands are linear and static; distorting one band and moving its level breaks both assumptions.

**Decision: per-band gain is set by parameters, never by an envelope follower.** That is what a real
GK 800RB does — its crossover and band levels are front-panel settings, fixed for a given sound — so this
is faithfulness rather than a workaround. Anything that makes band gain track the signal must be a
separate, deliberate feature with its own test.

**Done.** Recorded in the header next to `BassPathParameters` so the next person does not add an envelope
to it by accident.

### C3 · Host parameters and UI — 🟡 **parameters done, page layout outstanding**

`lowBandDrive`, `lowBandLevel` and `highBandLevel` are registered, wired through the X-macro that drives
both the `Param` enum and the id table — the mechanism whose own comment explains why two parallel lists
guarded by a test does not work — and read on the audio thread. Additive: a project saved before this
loads with them at their defaults, which reproduce the old fixed weights.

Laying them out on the amplifier page is outstanding, and is the same work as E2/E3.

### C4 · The voicing — ✅ **complete**

**Solid-State Bi-Amp is in the enum at index 11**, with its livery, and renders as rack gear rather than
a cabinet. Its test asserts the claim the voicing exists for: a 110 Hz note through a 500 Hz crossover
carries measurably less harmonic content than the same note through a 150 Hz one, because at 500 Hz the
whole body of the note takes the clean side and only the attack and harmonics reach the driven one.

## Track D — Gap 3: parallel dry blend, correctly aligned

### D1 · The blend itself

**Change.** A full-range dry tap taken **before** the preamp stages and summed **after** the power amp,
with `dryBlend` on `AmpParameters`, defaulting to 0. Per R7 the clean path stays at unity and the level
control acts on the distorted side, which is the topology the circuit being modelled actually uses.

### D2 · Latency alignment — ✅ **complete, and the number was worse than predicted**

`AmpVoice::dryDelay` holds the parallel path back by `latencySamples()`, re-derived after the stages
are configured — earlier would align to the *previous* configuration, which shows up as a voicing that
sounds thin only after a preset change.

**F3 was written first and the discipline paid for itself twice.**

*First:* the test initially passed with the alignment disabled — because it was never called from
`main`. A test function that compiles, is never invoked, and reports nothing is the most expensive kind
of green. Wiring it in and disabling the delay produced the real numbers:

| | 200 Hz | 1000 Hz | 1500 Hz |
|---|---|---|---|
| 2 stages (16 samples) | −1.2 dB | −5.2 dB | **−27.9 dB** |
| 3 stages (24 samples) | −1.3 dB | **−17.8 dB** | −3.7 dB |

The nulls land exactly at `fs / 2D` — 1500 Hz for 16 samples, 1000 Hz for 24 — and 200 Hz barely
moves, which is why a test that only looked at the bottom would have passed on a completely broken
blend. With alignment on, the same points read −0.26 dB and −0.59 dB.

*Second:* the first version of the assertion compared the blend against the **input** and demanded
1 dB flatness. It failed at 200 Hz by 1.9 dB *with alignment working perfectly*, because the drive
path is not flat — even at minimum drive it has about 4 dB more loss at 200 Hz than at 1.5 kHz. That
test was measuring frequency response and calling it alignment. It now measures **phase coherence**:
the blend against `0.5 · (|dry| + |wet|)`, with `|wet|` taken from the same amplifier at
`dryBlend = 0`, which cancels the drive path's own response from both sides.

**200 Hz is excluded from the assertion, deliberately.** The residual there is genuine minimum-phase
shift from the chain's high-pass filters — the pre-EQ cut, a cut and a DC blocker per stage, more in
the phase inverter — and it grows with stage count, exactly as the numbers show. No delay can undo it
because it is not a delay, and a real amplifier with a parallel clean blend behaves the same way.
Asserting there would mean a bound the correct implementation only just meets.

### D2a · The same defect, already shipped, in the bass crossover — *found while building D2*

Only the high band goes through the preamp stages. The low band reached the sum directly, so the two
halves of a Linkwitz-Riley split — which sums flat only while its bands keep their designed phase
relationship — were rejoining **16 to 24 samples apart**. About 20 degrees at the 120–180 Hz the valve
voicings split at; **60 degrees at the 500 Hz a bi-amp wants**, sitting right in the crossover region.

`lowBandDelay` fixes it, and **F1 immediately failed on all seven original bass voicings and none of
the guitar ones** — which is precisely the diagnosis, delivered by the test rather than by a listener.

**The goldens were not regenerated.** A correction that silently re-voices every bass project anyone
has already saved is not a correction from where they are sitting. It is `BassPathParameters::alignBands`,
defaulting **off**: the six bass-native voicings are born with it on, everything older keeps the sum it
shipped with, and a player who wants it on a valve voicing can have it.

### D3 · Host parameter — ✅ **complete**

`dryBlend` registered at 0–100%, defaulting to 0, wired through the X-macro and read on the audio
thread. Zero is a true bypass and the sum is skipped entirely there — not as an optimisation but
because the block has to be provably a no-op for F1 to mean anything.

### D4 · The voicing — ✅ **complete**

**CMOS Modern is in the enum at index 12**, the last of the six. Its first stage cuts at 150 Hz so the
distortion engine never sees the fundamental, and `dryBlend` at 0.40 brings that fundamental back
untouched — asserted directly: a 55 Hz note comes back more than 1.5× louder at its fundamental with
the blend engaged than without.

Built as **one** hard-clipped stage at +24 dB plus a linear make-up stage, per research finding R7,
rather than the cascade the first proposal specified.

## Track E — Liveries, shelving and the instrument filter

### E1 · `FaceplateStyle` rows — ✅ **complete for the four voicings that exist**

**Change.** Rows in `styleTable()` ([FaceplateArt.cpp](../Source/ui/FaceplateArt.cpp)) from the livery
notes above. That table is sized against `topologyCount`, so a missing livery is a compile error rather
than something to catch by eye. `AmplifierPage` also checks the choice parameter's option count against
`faceplateStyleCount` at run time ([AmplifierPage.cpp:77](../Source/ui/AmplifierPage.cpp#L77)).

**Done.** All four render and were inspected: `nts_art_renderer` now writes 18 faceplates rather than 14.
The count is not 22 because the bass-native voicings now render *identically* on both instruments — which
is the intended consequence of the `applyBassCabinet` change above, since they are already bass amplifiers
and there is nothing to restate. Both palette rules hold on all four: the panel stays dark, and the grille
stays the darkest large region even under the orange covering.

### E2 · Shelving — ✅ **complete**

`AmpCharacter` gained a **`vintage`** shelf, taking it to four. The bass picker shows all thirteen
voicings — the seven guitar-derived ones keep their bass readings and saved projects depend on them —
so three shelves that carried seven tiles comfortably carried thirteen badly. Vintage Bloom, Class-A
Chime and Cathode Vintage moved onto it, which leaves about three a shelf.

Safe to reorder the enum because nothing outside the picker reads it: no DSP, no saved state, no preset
field. It is a browsing concept and only ever a browsing concept.

### E3 · Instrument affinity — ✅ **complete**

The **view** filters; the parameter never does. `AmplifierPage` skips bass-native tiles for a
guitarist, which is safe because `GearTile::parameterIndex` carries the value it writes — the eighth
tile still writes 7 whether or not the sixth was drawn.

**`ToneShapingPage`'s combo box lost its `ComboBoxAttachment`,** which is the risk the plan flagged and
it was real. `juce::ComboBoxParameterAttachment` maps the box's **selected position** to the parameter
value, not its item id — fine for a list that always shows every choice and silently wrong for one that
does not. It is now driven by hand with the item id carrying the topology index, because ids are stable
identifiers and positions are not.

A guitar rig sitting on a bass-native voicing — reachable from a preset, a project or automation —
shows nothing selected rather than snapping to a voicing nobody asked for. Rewriting the parameter to
make the list look tidy would change the sound.

### E3a · The worst bug in this plan, found by running the app

Adding the four host parameters, I put them in `ampControlIds` next to `crossover` and `cleanBlend` —
where they belong by meaning, and where they must never go. `applyProjectState` walks a saved project's
values **positionally** against that table, so four ids inserted in the middle shift every id after
them: an old project's Cab alignment arrives in Dry blend, its Tightness in Low drive, and so on to the
end of the list.

**Every test in the suite passed while this was broken.** A positional remap produces perfectly valid
numbers in the wrong fields — nothing out of range, nothing NaN, nothing to fail on. It was found by
launching the standalone and reading the panel: Low drive sat at 5.4 and Low level at −1.0 where the
defaults are 2.6 and 0.0.

Fixed by appending, as the three existing comments in that table already instruct. Guarded by a new
test that loads a synthesised pre-change project and asserts three parameters past the insertion point
are undisplaced — verified to fail, by name, when the insertion is reintroduced.

**The lesson is the plan's own:** the table that reads well is not the table that loads correctly, and
no amount of unit testing substitutes for opening the thing.

## Track F — Tests

### F1 · The seven existing voicings are bit-identical — ✅ **complete**

`testOriginalVoicingRegression` in [AmpTests.cpp](../Tests/AmpTests.cpp) renders all fourteen original
(instrument, topology) pairs through the complete graph and compares an FNV-1a hash of the output against
values **captured from the build immediately before the bass block was added**. This is the test that lets
Tracks B, C and D touch shared audio code at all.

Two notes on how it was built, because both matter for trusting it:

- **The goldens came from the pristine code, not from the current code.** The three touched files were
  clean at `HEAD`, so `git show HEAD:` gave true pre-change sources; those were built, the hashes
  captured, and the changes restored. Generating goldens from the post-change build would have produced a
  green test that proves nothing.
- **The guard was verified to fail.** Perturbing `tightModern`'s power-amp saturation by 0.01 makes it
  report `voicing changed sound: tightModern/guitar` and `.../bass` by name, and exit non-zero. A
  regression test nobody has seen fail is a regression test nobody should believe.

**The hashes are never to be regenerated to make a failing build pass.** That note is in the test itself.

### F2 · The new voicings are distinct — ✅ **complete**

`AmpTests.cpp`'s existing pairwise-distinctness loop is bounded by `topologyCount` and so covered the new
voicings for free. Added alongside F1: each bass-native voicing keeps its own `p.bass` block (the A2
trap), every bass-native voicing returns a real preset for guitar (A3), and `topologyAffinity` splits at
the expected index.

### F3 · Dry blend alignment — ✅ **complete**

Written before D1 as planned. Measures phase coherence at the two comb-null frequencies; reads 0.02 to
0.06 dB aligned and 18 to 28 dB unaligned, both verified by disabling the delay. See D2 for the two
ways the first attempts at it were wrong.

### F4 · Boundedness and calibration — ✅ **complete, and half of it was asking the wrong question**

**Boundedness.** Every voicing on both instruments, with every waveform-shaping control at its stop,
stays finite and under +6 dBFS. Master, output level and the two band *levels* are pinned at unity
rather than maximised — with them at +12 dB a loud output is arithmetic, not a defect, and a bound
loose enough to allow it catches nothing. What is left after pinning them is the curves, which are the
only things here that can diverge.

**One curve is an outlier and it is now recorded.** Swept across all eight shapes through four maxed
stages, every one peaks between 1.10 and 1.43 except `asymmetricPolynomial`, which reaches **2.73** —
roughly twice any other. Bounded, not divergent, and *not* caused by the asymmetric bias offset: with
the stages centred it reads higher still (2.73 against 2.08), so the curve simply passes more. It is
also the one shape `dsp::supportsAntiderivative` rejects, so it cannot be antialiased. No factory
voicing selects it, and a second assertion keeps it that way.

**Level.** With `loudnessMatch` off, the factory voicings sit within **15.7 dB on guitar and 15.4 dB on
bass**. Wider than ideal and not a defect — loudness matching defaults on — but the bound exists so
that a voicing 20 dB down never ships.

**Measured with a harmonically rich note, after a pure sine gave a wrong answer.** A single sine at the
fundamental made three voicings read 9 to 15 dB quiet and none of them were: CMOS Modern filters
everything below 150 Hz out of its drive path and Solid-State Bi-Amp splits at 500 Hz, so a 110 Hz sine
lands entirely on one side of each. The measurement was reading their crossovers and calling it level.

**Calibration: the plan asked for something that does not apply.** F4 called for a per-voicing review of
`CalibrationProfile`, reasoning that CMOS Modern's 24 dB first stage is a very different gain structure.
It is — but `InputCalibrator` never sees it. The calibrator runs on the signal arriving at the plug-in,
before the trim and before the gate, and suggests a trim to land that *input* at a target. Its numbers
depend on the player's pickups and interface and on nothing downstream, so a profile per instrument is
exactly the right granularity and a profile per voicing would model a dependency that does not exist.
The real version of the concern is output level across voicings, which is the check above.

## Track G — Documentation — ✅ **complete**

`docs/user-guide.md` gains the six voicings and what each is for, the instrument-dependent list, and a
short section on the difference between Clean blend and Dry blend — which look like the same control
and are not.

`docs/developer-guide.md` gains an **Amplifier voicings** section stating the three rules that
constrain changes here and naming the guard for each: append to the enum, default every new field to
today's behaviour (with the F1 hashes and the rule against regenerating them), and keep `ampControlIds`
append-only. It also records the latency-compensation rule and why `testDryBlendAlignment` measures
phase coherence rather than flatness.

## Track H — Song Match hookup *(not in the original plan)*

The plan assumed a voicing added to the enum was a voicing added to the product. It is not: the
reconstruction engine behind Song Match searches over topologies, so all six became findable with no
code change — and inherited three problems in doing so.

### H1 · Guitar was searching bass amplifiers — ✅

Both search sites were bounded by `topologyCount` with no affinity check, so a guitar reference
rendered and ranked all six bass-native voicings. That is not merely wasted work: each is an extra
offline render on the topology re-check, and each is a candidate that can take a shortlist slot from
an amplifier the player might actually have used.

`searchableTopologies` now filters both, and is **published** rather than kept private — its length
*is* the cost of a reconstruction, which is worth being able to ask about and worth being able to
test. A guitar reconstruction is back to the render count it had before the bass voicings landed;
bass pays the full thirteen.

### H2 · The fitter destroyed the bi-amp's identity — ✅

`setCandidateParameters` overwrote `bass.crossoverHz` with an absolute `90 + tightness * 180`.
Reasonable while every voicing splits at 120–220 Hz to keep a low B out of the distortion — and it
flattened the one voicing whose 500 Hz split *is* its identity. Solid-State Bi-Amp could be selected
and could never sound like itself, so it would lose matches it should win while sounding like nothing
in particular.

Now scaled to 0.5×–1.5× of the voicing's own value: tightness stays meaningful everywhere, and the
shared 170 Hz default sweeps 85–255 against the previous 90–270.

### H3 · Hard-clip candidates were scored on their own aliasing — ✅

The same function pinned `oversamplingFactor = 4`, with a comment explaining that candidates must be
rendered as they are played so the analyser measures the amplifier and not the render's fold-back.
Exactly right, and it needed to be a **floor** rather than a value: a clamped curve folds far harder
than a valve one, so forcing a voicing that asks for 8× down to 4 scored precisely the artefact that
line exists to remove, on the candidates most sensitive to it.

### H4 · Candidate names — ✅

The label was `topology == tightModern ? " tight" : " bloom"`, so twelve of thirteen voicings read
"bloom". It now uses `topologyName`.

### What the tests could not say, and why that is recorded

The first version of H's tests asserted that a fitted bi-amp candidate keeps its 500 Hz split and
that a hard-clipping candidate keeps its 8×. **Both were vacuous.** A shortlist is filled by refining
the *winning* point, and topology is deliberately excluded from the "is this a different rig" test, so
a synthetic reference that favours one voicing returns ten refinements of it and never reaches
another — asking for ten candidates instead of three changed nothing.

They were replaced rather than kept. An assertion over candidates that never appear reads as coverage
and is silence. What is decidable is the searchable set, which is now tested directly and verified to
fail when the filter is inverted, plus a ratio property on the crossover that the old absolute fit
could not have satisfied.

### The neural conditioning question, closed

Raised as a possible compatibility break: [PluginProcessorAudio.cpp:339](../Source/PluginProcessorAudio.cpp#L339)
normalises the topology parameter across `topologyCount`, so going 7 → 13 changes the value fed to a
conditioned model for an unchanged topology index.

**It does not bite, and the concern was over-stated.** The captures in `amp_learning/` are NAM-style
and convert to WaveNet, whose `controlCount()` is **0** — conditioning is ignored entirely.
`PackedTanhModel::setControls` rejects any vector that is not exactly its own control count, so only a
five-control `ConditionedModel` consumes it, and that model's fourth control is a continuous −1..+1
axis named `channel`, not an index. The normalisation being `topologyCount`-relative is the *designed*
behaviour — the comment at that line says so explicitly, having been changed from a hardcoded 2 for
exactly this reason.

## Data corrections needed in the source workbook

Carry these regardless of whether the spreadsheet is fixed:

- **The two sheets contradict each other on Mesa.** The Overview (`A8`) files it under "Hybrid
  Tube/MOSFET"; the database (`C7`) correctly says "All-Tube (500W)" with twelve 6L6s. The database row
  is right.
- **`E4` "No negative feedback loop" is wrong for the SVT.** See R1. This one would have changed a
  shipped sound.
- **No complexity tier.** The pedal workbook's tier column drives `TierLimits::pedalCostBudget`; there is
  no equivalent here, and the CPU spread between Hybrid MOSFET and an 8×-oversampled CMOS Modern is
  large. Track B3 has to derive one.
- **No numeric content anywhere.** Unlike the pedal catalogue this cannot be imported. Every figure in
  this plan was reasoned or researched, and that is a per-voicing cost that does not amortise.

## Open questions

1. **Does `loudnessMatch` fight these voicings?** It defaults true. Cathode Vintage's identity is that it
   compresses and gets loud; Valve Flagship's is that it blooms. Loudness matching is right for
   *comparing* amps and arguably wrong for *playing* one. No change proposed — but it should be a
   deliberate answer rather than an inherited default.
2. **Should the existing seven keep their bass retunes?** They must, for saved projects. But once six
   bass-native voicings exist, "Original Class-A Bass" is a Vox circuit competing for shelf space against
   amps that were actually built for the instrument. Deprecating them in the *picker* while keeping the
   indices valid is possible and is a product call, not a technical one.
3. **Is a graphic EQ worth its own track?** It would recover the Mesa Bass 400+ and make CMOS Modern more
   faithful. It is a new parameter block, new host parameters and new UI, for two voicings.
4. **Three tone-stack types are now doing real work.** All three appear across these six —
   `passiveCoupled` on Short-Path Grind, `activeThreeBand` on three, `bassSemiParametric` on two. Worth
   confirming `activeThreeBand` is genuinely non-interacting before two Ampegs depend on the distinction.

## Out of scope, and why

- **Neural capture of any of these amps.** The `neural` engine plays captures already, and a capture of an
  SVT is a better SVT than any parameter table. Different workflow, and the Captures page owns it.
- **A third tier in the power section** for the SVT's 12BH7 drivers. See R2 — folded into the phase
  inverter instead.
- **Cabinet impulses for 8×10 and 1×15.** Real, and a separate piece of work from voicings.
- **Modelling the workbook's "Iconic Applications" column.** Useful prose for the picker blurbs and
  nothing more.

## Research sources

- [Prowess Amplifiers — SVT 6550a power amp schematic](http://www.prowessamplifiers.com/schematics/ampeg/SVT_Poweramp_6550a.html) and [Dr. Tube — Ampeg schematics](https://www.drtube.com/ampeg/) — R1, R2
- [TalkBass — driver tubes in the SVT](https://www.talkbass.com/threads/driver-tubes-svt.638781/) — R2
- [TalkBass — what the Ultra Lo actually does](https://www.talkbass.com/threads/what-does-the-ultra-lo-feature-in-ampeg-actually-do.1528195/) and [Ampeg SVT-CL owner's manual](https://ampeg.com/pdf/SVTCL_OM.pdf) — R3
- [Gallien-Krueger 800RB bi-amp mode](https://midi-audio-expert.com/how-to-use-gallien-krueger-800rb-bi-amp-mode/) and [Plugin Alliance 800RB](https://www.plugin-alliance.com/products/800rb) — R4
- [TalkBass — B-15 cathode vs fixed bias](https://www.talkbass.com/threads/ampeg-b15-cathode-bias-fixed-bias-and-output-transformer.1684277/), [Premier Guitar — Ampeg B-15N Portaflex](https://www.premierguitar.com/ampeg-b-15n-portaflex), [Reverb — the golden age of the B-15](https://reverb.com/news/the-golden-age-of-the-ampeg-b-15-1960-1980) — R5, R6
- [Aion FX — Maelstrom Bass Drive (B3K trace)](https://aionfx.com/project/maelstrom-bass-drive/) and [Darkglass B3K manual](https://www.darkglass.com/manual-microtubes-b3k/) — R7
- [Aguilar DB 751 user manual](https://www.manualshelf.com/manual/aguilar/db751/user-manual-english.html) — R8
- [Chowdhury — practical considerations for antiderivative anti-aliasing](https://jatinchowdhury18.medium.com/practical-considerations-for-antiderivative-anti-aliasing-d5847167f510) and [DAFx-20 — ADAA in nonlinear WDFs](https://dafx2020.mdw.ac.at/proceedings/papers/DAFx2020_paper_35.pdf) — B2
- [Rane note 160 — Linkwitz-Riley crossovers](https://www.ranecommercial.com/legacy/note160.html) and [KVR — splitting bands with Linkwitz-Riley](https://www.kvraudio.com/forum/viewtopic.php?t=500121&start=15) — C2
