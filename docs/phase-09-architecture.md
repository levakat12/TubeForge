# Phase 9 intelligent tone-assistant architecture

## Evidence boundary

The assistant does not receive mutable DSP objects. On the audio callback, TubeForge makes two bounded passes
over the current block and writes a compact trivially copyable summary frame to a fixed-capacity SPSC queue.
The frame contains input/output peak and RMS, clipped-sample counts, zero-crossing rate, stereo correlation,
sample rate, block size, and reported algorithmic latency. It contains no recorded audio and performs no heap
allocation, locks, strings, FFTs, or recommendation work.

The existing editor timer drains frames off the callback, aggregates decaying peaks, smoothed RMS/crest,
minimum observed noise floor, intermittent-drop ratio, phase correlation, clip rates, and latency, then runs the
assistant at most once every 750 ms. Evaluation pauses while a preview is active so it cannot react to its own
parameter movement. Phase 7 tone reports add spectral, nonlinear, dynamic, spatial, instrument, and confidence
evidence when the user has analyzed a clip. A Phase 8 reference report is optional for reference matching.

## Interpretable recommendation engine

The first assistant is intentionally rule based. It distinguishes measurable faults from requested style goals
and covers input clipping/quietness/noise/intermittency/active-pickup overload, output and gain structure,
mud/thinness/boom/harshness/fizz/attack/string noise, compression/transients/sustain/level variation, gate
chopping, bass fundamental loss/pumping, parallel-blend and stereo phase, cabinet darkness, and latency.

Each rule emits one structured action with:

- stable action and problem identifiers;
- confidence and concrete evidence values;
- a musician-facing explanation and a technical explanation derived from the same condition;
- exact current/target parameter values;
- expected effect and measurable/style classification.

Eight style goals and diagnose mode translate language such as tighter, warmer, smoother, clearer, more attack,
or preserved low end into bounded macros. Reference matching uses descriptor deltas only and never changes pitch
or timing. Accepted/rejected action IDs may adjust ranking slightly; no learned layer can directly mutate state.

## Safe preview transaction

The assistant knows only a fixed schema of existing TubeForge parameters. Each parameter has minimum, maximum,
and maximum preview delta. Preview validation rejects unknown identifiers, non-finite values, stale `from`
values, out-of-range targets, and oversized changes. This prevents arbitrary project or circuit-graph mutation.

A preview stores complete exact before/after snapshots. Applying the after snapshot uses normal host-notifying
parameters; the traditional/circuit engines retain their existing coefficient smoothing and graph crossfade.
Accept pushes the before snapshot into undo history. Reject restores it exactly. Undo restores the last accepted
snapshot. No suggestion applies without an explicit Preview action, and a second preview cannot begin until the
first is accepted or rejected.

## Local preferences and interface

Preferences are saved below the TubeForge application-data directory as versioned JSON. They contain only the
enabled flag, preferred instrument/gain/brightness/clean blend, internal cabinet usage, and accepted/rejected
action IDs. Audio and song identity are not stored. Personalization can be disabled or cleared; clearing also
removes assistant undo history and the local file.

The Tone Assistant page exposes goal and suggestion selection, Preview, Accept, Reject, Undo, local
personalization, and Clear Preferences. Its evidence panel shows confidence, beginner guidance, technical
evidence, validated parameter deltas, and expected effect. Network or language-model access is not required.
