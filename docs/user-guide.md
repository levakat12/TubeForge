# TubeForge user guide

## Install and configure

Run the Windows x64 installer and select Standalone, VST3, and optional factory models. The VST3 is installed
to the system Common Files VST3 folder. In a DAW, rescan plug-ins and insert TubeForge on a mono or stereo
audio track. In standalone mode choose the audio device, input/output channels, sample rate, and the smallest
stable buffer. Start at 48 kHz and 64 samples; increase the buffer if the diagnostics bar reports dropouts.

Connect a guitar or bass through a high-impedance instrument input. Keep interface peaks below clipping.
Use Input for calibration, Gain and the tone knobs for the amp, Master for power-stage level, and Output for
final level. The Advanced page exposes topology, stages, filters, sag, feedback, bass crossover/clean blend,
cabinet alignment, and 1x/2x/4x/8x oversampling. Higher oversampling costs more CPU and latency.

The **Voicing** control on the Amplifier page opens a panel of amplifiers grouped by what they sound
like — Clean, Crunch and Hi-gain — with a picture of each and a line saying what it does. Click one to
load it; the faceplate changes to match. Arrow keys move between them, Return chooses, and Escape closes
without changing anything. The same voicing is also on the Tone Shaping page as an ordinary drop-down if
you would rather pick it by name.

Topology is the starting point for every other control, and each of the seven is a different amplifier
rather than a variation on one. **Studio Direct** is the cleanest and is the only one with an active,
non-interacting three-band EQ — start here for a bass DI or a modern preamp sound. **American Clean** has
the most headroom of the valve voicings and a stiff, high-feedback power section. **Class-A Chime** runs
almost no negative feedback and is cathode-biased, so it is bright and compresses early. **Vintage Bloom**
and **British Crunch** both break up in the power section; Bloom is looser and darker, Crunch is
mid-forward and cuts. **Tight Modern** and **Sagging Rectifier** are the two high-gain voicings, and the
difference is the supply — Tight Modern recovers fast and stays controlled, Sagging Rectifier is still
recovering when the next chord lands. Changing topology resets the panel to that amp's starting values,
so dial it in first and refine afterwards.

## Captures

The Captures page is your library of Neural Amp Modeler captures. **Import .zip or .nam** takes an archive,
a single `.nam`, or a folder of either, and converts everything inside — no command line and no Python.
Whole archives are converted at once because that is how the packs come: one pedal archive can hold
eighty-nine captures of the same box at different settings. Conversion is keyed by each capture's digest,
so re-importing an archive you already have is free and cannot create duplicates.

Captures are sorted into amps, full rigs and pedals from what the capture's author recorded in it. Pick one
and **Load into** the amp engine or any pedal slot; that destination switches to Neural capture for you. A
capture labelled for somewhere else is still offered and still loads — it is flagged, not blocked.

The **Tier** control chooses which of the two models inside a capture is converted: standard is full
quality, lite costs roughly a third as much to run. See [nam-capture-guide.md](nam-capture-guide.md) for
what is and is not verified about a capture converted in the plug-in.

## Pedals

The Pedals page holds four slots in front of the amplifier, in signal order left to right. Click a slot's
pedal to open a panel of everything that can go in it, grouped by what each is for — Clean & Dynamics,
Overdrive, Distortion, Fuzz and Captures — with a picture of each and a line saying what it does. **Empty**
is pinned at the top, because clearing a slot is something you do often. Arrow keys move, Return chooses,
Escape closes without changing anything.

Choosing **Neural capture** takes you straight to the capture library for that slot, so a slot never ends up
set to a capture with no model in it. The slot's own **Load capture** button opens the same list later, and
can import an archive without leaving the page.

Every slot defaults to Empty, and Empty means the slot is absent rather than running at unity: the default rig
is amplifier and cabinet alone, and leaving the page untouched is a supported way to use it. **Bypass** is the
separate control for switching a pedal out temporarily without losing its settings or its loaded capture.
Both switch without clicking, so either is safe to use while playing.

Every kind shares the same four controls. **Drive** is gain into the pedal — for a neural capture it is the
level going into the model, which is what a capture is most sensitive to. **Tone** is a roll-off above it.
**Level** is the pedal's output. **Mix** blends the result back against the dry signal; at zero the slot is
exactly transparent whatever else it is set to.

Because the board is in front of the amplifier, a boost drives the amplifier's own front end harder — the
reason to reach for one — rather than simply making the output louder. It also sits ahead of the noise gate,
so the gate reacts to what the pedals produce.

## Workflows

- Neural Capture plays a capture from the library, or loads a model folder exported by the capture wizard;
  either way its hashes, schema, test vectors, and model must validate before it can reach the audio thread.
- Circuit Engineering lets you switch the preamp tube, power tube and topology, tone-stack network, solver, and cabinet model. Amp/Advanced knobs update the underlying component values; the response plot is calculated from the selected tone network. Structural edits compile off the audio thread and are restored with the project.
- Tone Analyzer analyzes a local isolated clip and compares its embedding with the local tone database.
- Song Reconstruction uses local neural guitar/bass stems and lists timestamped clean, crunch, and distorted
  parts with dominant pitch, likely tuning family, cent offset, and confidence. Select a part and choose
  **Use selected part** to rebuild the playable candidates from that section; source audio remains excluded
  from exports.
- Tone Assistant explains bounded recommendations and requires Preview before Accept; Reject and Undo are exact.
- Profile Library searches by name/author/tag, filters instrument or favorites, imports `.ntone` directories,
  applies compatible rigs, and exports the current rig. Unsigned local packages show a warning.

Projects use `.tforge`; portable tone profiles use `.ntone`. Back up both. If audio glitches, disable 8x
oversampling, shorten convolution IRs, raise the host buffer, confirm the reported latency, and check the DSP,
callback, peak, and dropout readings. The installer includes a diagnostic collector that packages logs and
system metadata but no audio, project/profile files, or source file names.
