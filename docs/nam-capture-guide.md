# Using Neural Amp Modeler captures

TubeForge can use a `.nam` capture two ways: **converted**, which reproduces the capture exactly,
and **distilled**, which trains one of the built-in recurrent models to imitate it. Converted is
almost always what you want. Distilled exists for cases where the CPU cost of the original matters
more than the last few percent of accuracy.

Converting needs no tools at all — the plug-in does it. Distilling is an offline job in the
`nts-ml` package. Neither redistributes anything: a converted artifact carries the capture
author's name in its `license.txt`, and converting does not grant you the right to pass the
result on.

## Importing in the plug-in

Open the **Captures** page and press **Import .zip or .nam**. Point it at an archive, a single
capture, or a folder of either, and every capture inside is unpacked and converted in place. The
same button is on the picker that opens from **Load capture** on the Pedals page and from **Load a
capture** on Neural Capture, so an archive can go straight into a pedal slot without leaving the
page.

Whole archives are converted rather than one capture at a time because that is the shape the real
packs come in: one pedal archive holds up to eighty-nine captures of the same box at different
control settings, so there is no answer to "which one" until you are choosing a sound. Converting
the lot turns that into browsing a list. Conversion is keyed by each capture's own SHA-256, so
re-importing an archive you already have costs a stat per capture and cannot create duplicates.

Captures are sorted into **amps**, **full rigs** and **pedals** from the `gear_type` the capture's
author recorded. That ordering is advice, not a rule: a capture is offered first where it fits and
flagged where it does not, but nothing is withheld — running a full-rig capture in a pedal slot is
a strange thing to want and a legitimate thing to try.

Converted captures live in `%APPDATA%\TubeForge\captures`, one directory per capture, and the
originals are never modified. Deleting a capture from the page removes only the converted artifact.

### What the plug-in checks, and what it does not

An artifact produced by `nts-nam-import` carries test vectors rendered by the Python
implementation, which is itself checked against upstream `neural-amp-modeler`. Re-running those
vectors at load time is therefore a real parity gate on the C++ runtime.

An artifact converted inside the plug-in cannot carry vectors like that — the thing rendering them
would be the thing they were meant to check. Those artifacts record `"testVectorSource": "runtime"`
in their manifest, and their vectors prove the model loads, primes and runs deterministically
rather than proving parity.

Parity for that path is enforced somewhere better: `nts_nam_parity` requires the plug-in's
converter and `nts-nam-import` to produce **byte-identical** `model.bin` for every capture, at
both tiers. Byte identity is a stronger claim than any audio tolerance, and it carries the whole
existing chain across. All 354 corpus captures pass at both tiers.

## Converting from the command line

Still useful for converting in bulk outside the plug-in, for scripting, and for producing
artifacts whose test vectors are independent of the runtime.

```bash
nts-nam-import path/to/capture.nam --output ~/TubeForge/models
```

Point it at a folder to convert everything underneath. Captures the reader does not understand are
reported and skipped rather than aborting the run. Each capture becomes an artifact directory —
`model.bin`, `manifest.json`, `normalization.json`, `license.txt` and `test-vectors/` — which is the
same layout the plug-in already loads for its own trained models. Conversion is keyed by the source
file's digest, so re-importing the same capture reuses its artifact instead of duplicating it.

### Tiers

Corpus captures contain two independently trained models. `--tier standard` (the default) is the
full-quality one; `--tier lite` is roughly a third of the cost. On the development machine, at
48 kHz with 64-sample blocks:

| Tier | Channels | Cost per instance | Realtime factor |
|---|---|---|---|
| standard | 8 | 17.1% of one core | 5.8x |
| lite | 3 | 5.5% of one core | 18.3x |

Those are scalar figures with no SIMD. Stereo doubles them, since each channel runs its own model
state.

### Sample rate

Every capture in circulation is 48 kHz, and the runtime **rejects a model whose rate does not match
the audio device**. At 44.1 kHz a converted capture will refuse to load, and the status line says
so. This is deliberate: silently resampling around a model changes its tone and its latency.

### What the conversion checks

The converter refuses captures using the v0.7 conditioning features the runtime does not implement —
FiLM modulation, gated activations, grouped convolutions, an active head 1x1, secondary activations.
It also refuses a capture whose weight vector does not match the geometry its own configuration
declares. In every case the message names the feature, so an unsupported capture is identifiable
rather than mysterious.

Exported test vectors are rendered by the Python implementation, which is checked against upstream
`neural-amp-modeler`. The plug-in re-runs those vectors before it will activate a model, so a
runtime that disagrees with the capture cannot load at all.

## Distilling a capture

Distillation renders a DI corpus through the capture and trains a built-in model on the result.

```bash
nts-nam-distill --capture path/to/capture.nam --di path/to/di-folder --output runs/deluxe
```

Requirements worth knowing before you start:

- **At least two distinct DI files.** Training holds one session out for validation, and two halves
  of the same performance are not independent. The tool refuses rather than producing a number that
  looks like a validation score but is not one.
- **DI must already be at the capture's sample rate.** The tool does not resample silently.
- **Expect a real gap.** A capture's own reported error is around 0.002; a 48-unit LSTM distilled
  from one lands near 0.2 on held-out material. That is the trade being made. If the result sounds
  thin or unstable in the bass, raise `hidden_size` before adjusting loss weights — the teacher has
  a 132 ms receptive field and a small student loses low-frequency behaviour first.

Rendering runs at roughly 24x realtime, so a ten-minute DI corpus takes about 25 seconds; training
dominates the total and needs the torch backend to be practical (`backend = "auto"` in the shipped
config).

## Pedal captures

A pedal capture converts exactly like an amplifier capture — same reader, same artifact layout,
same digest — and loads on the **Pedals** page instead of Neural Capture. Set a slot's Kind to
*Neural capture*, press **Load capture**, and pick one from the list; the picker's own Import
button takes the archive if you have not imported it yet.

Two things are worth knowing about pedal captures specifically:

- **Drive is the input level, not a gain control on the model.** A capture is trained at one input
  level and reproduces its pedal faithfully only near it. The slot's Drive knob trims ±12 dB into
  the model, which is the control that decides whether a captured screamer sounds like the pedal or
  like a much quieter or much louder one.
- **Cost is per slot and per channel.** Four standard-tier captures in stereo is eight model
  instances — see the tier table above and budget accordingly. The lite tier exists for this.

An empty *Neural capture* slot passes audio through rather than going silent, so selecting the kind
before loading anything is harmless.

## Cabinet impulse responses

Capture packs often ship `.wav` impulse responses alongside the models. Those need no conversion —
load them on the Cabinet page. Prefer a 48 kHz file when running at 48 kHz to avoid resampling.

## Cataloguing a collection

```bash
nts-nam-catalogue path/to/captures --output catalogue.json
```

Indexes every capture with its metadata and the settings recoverable from its filename — amplifier
control positions, microphone, boost pedal, wattage, channel. Useful for finding your way around a
large collection, and as reference data for tone matching.
