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

## Workflows

- Neural Capture loads an exported capture artifact after its hashes, schema, test vectors, and model validate.
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
