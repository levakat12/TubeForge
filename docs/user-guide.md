# TubeForge user guide

## Install and configure

Run the Windows x64 installer and select Standalone, VST3, and optional factory models. The VST3 is installed
to the system Common Files VST3 folder. In a DAW, rescan plug-ins and insert TubeForge on a mono or stereo
audio track. In standalone mode choose the audio device, input/output channels, sample rate, and the smallest
stable buffer. Start at 48 kHz and 64 samples; increase the buffer if the diagnostics bar reports dropouts.

Connect a guitar or bass through a high-impedance instrument input. Keep interface peaks below clipping.
Use Input for calibration, Gain and the tone knobs for the amp, Master for power-stage level, and Output for
final level. The Advanced page exposes topology, stages, filters, sag, feedback, dry blend, the bass
bi-amp controls, cabinet alignment, and 1x/2x/4x/8x oversampling. Higher oversampling costs more CPU and
latency.

The **Voicing** control on the Amplifier page opens a panel of amplifiers grouped by what they sound
like — Clean, Vintage, Crunch and Hi-gain — with a picture of each and a line saying what it does. Click
one to load it; the faceplate changes to match. Arrow keys move between them, Return chooses, and Escape
closes without changing anything. The same voicing is also on the Tone Shaping page as an ordinary
drop-down if you would rather pick it by name.

**The list depends on the Instrument setting.** Six of the voicings were designed for bass rather than
adapted to it, and they are only offered when Instrument is set to Bass. Switching to Bass adds them;
switching back to Guitar takes them away. Nothing you have already saved is affected either way.

Topology is the starting point for every other control, and each one is a different amplifier rather
than a variation on one. Changing it resets the panel to that amp's starting values, so dial it in
first and refine afterwards.

### The guitar voicings

**Studio Direct** is the cleanest and has an active, non-interacting three-band EQ. **American Clean**
has the most headroom of the valve voicings and a stiff, high-feedback power section. **Class-A Chime**
runs almost no negative feedback and is cathode-biased, so it is bright and compresses early. **Vintage
Bloom** and **British Crunch** both break up in the power section; Bloom is looser and darker, Crunch is
mid-forward and cuts. **Tight Modern** and **Sagging Rectifier** are the two high-gain voicings, and the
difference is the supply — Tight Modern recovers fast and stays controlled, Sagging Rectifier is still
recovering when the next chord lands.

### The bass voicings

These six are on the Bass instrument setting only. What separates them from a guitar amp with its
controls moved is that their preamp stages **keep the bottom octave** — on a guitar that is mud ahead of
a distorting stage, and on a bass it is the note.

**Valve Flagship** is the big all-valve head: three stages, a supply that blooms under a chord, and the
roaring midrange that goes with it. Start here if you want the reference rock and metal bass sound.

**Cathode Vintage** is a small cathode-biased combo — twenty-five watts, no top end and no headroom, so
it compresses the moment you dig in. It is the most-recorded bass sound there is, and it sits under a
vocal rather than fighting one.

**Hybrid MOSFET** is valve warmth in front of a power section that will not distort. Reach for it when
the amplifier should disappear: slap, funk, and modern session playing. It is also the right starting
point for a scooped sound.

**Short-Path Grind** has the least bottom end here and that is deliberate — it is built to cut through a
wall of guitars rather than to sit under one. It fuzzes out early.

**Solid-State Bi-Amp** splits the signal at a much higher frequency than the others, so the whole body
of the note passes through clean while only the attack and the harmonics are driven. That is what makes
it clank without ever sounding fuzzy. Its controls are on the **Bass bi-amp** card on the Tone Shaping
page: **Bass crossover** sets where the split happens, and **Low level** and **High level** balance the
two halves.

**CMOS Modern** is the high-gain one. Its distortion engine is filtered so it never touches the
fundamental, and **Dry blend** brings that fundamental back at full weight. Turning Dry blend down on
this voicing takes its low end away, which is the point of the control rather than a fault.

### Panel switches

Two of the amplifiers have a **Panel switch** control next to Voicing. It appears only on the
voicings that have one, because these are switches on those particular amplifiers rather than
general-purpose EQ.

**Valve Flagship** has **Ultra Lo** and **Ultra Hi**. Ultra Lo is the interesting one: it lifts the
bottom a little *and cuts the midrange a lot*, and the cut is what makes it sound bigger — it was
designed to give the impression of weight without actually asking more of the output stage. Use it
when you want size without mud. Ultra Hi is a treble lift for cutting through.

**Hybrid MOSFET** has **Deep** and **Bright**, which are broad lifts at the bottom and top. Deep is
what you reach for when a five-string needs to feel like one.

Both are saved with the project and can be automated.

### The two blend controls, which are not the same thing

**Clean blend** returns the *low band* clean, from below the crossover. **Dry blend** returns the whole
untouched input, summed over everything the amplifier does. On a heavily distorted bass sound Dry blend
is usually what you want: it keeps the note while the top stays destroyed.

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
pedal to open a panel of all **62 pedals** that can go in it, grouped by what each is for — Clean &
Dynamics, Overdrive, Distortion, Fuzz, Modulation, Bit & Sample, Delay & Reverb, Pitch, Bass DI and
Captures — with a picture of each and a line saying what it does. **Empty** is pinned at the top,
because clearing a slot is something you do often. Arrow keys move, Return chooses, Escape closes
without changing anything.

Every pedal names its own controls, and hides the ones it does not have: a Boost shows one knob, the
Chainsaw shows Distortion, Tone, Level and two Colour controls. The first four always mean roughly the
same thing — drive in, tone above it, output level, and how much is blended back — so a rig transfers
between pedals without relearning it. The last two belong to the particular pedal, and the label above
each says what it is. In a host's automation list those two read as "Aux A" and "Aux B", because a
plug-in parameter cannot change its name; the interface shows the real one.

Each pedal carries a modelling cost, and the help line at the top of the page turns amber if the board
is heavier than the current **Performance** tier is meant to carry. Nothing is changed when that
happens — if it plays cleanly, ignore it; if it does not, raise the tier or bypass a slot.

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

## Cabinet

The **Cabinet** page is the speaker, and it runs after whichever engine is selected — the traditional amp, a
neural capture, or the physical circuit. That matters most for captures: a Neural Amp Modeler capture of a
preamp or a pedal has no speaker in it, and previously there was no way to put one in front of it. A capture
whose author labelled it a full rig already contains a cabinet, so loading one switches the cabinet section
off and says so; switching it back on is your call.

Two responses, **A** and **B**, each either the built-in cabinet or an impulse response you load. Each has its
own **Level**, **Placement**, **Delay**, **Invert polarity** and **Mute**, because that is what two microphones
on a cabinet actually differ by. **Solo** listens to one on its own; it is a listening action, is not saved
with the project, and ends by unmuting both.

### The built-in cabinet

The built-in response is a **model** rather than a recording: a speaker in a box, with a microphone in front of
it, described by **Cabinet**, **Microphone**, **Position** and **Distance**. That is why the last two are
continuous instead of a handful of files — Position sweeps from the dust cap to the cone edge, and Distance
from one inch to two feet.

- **Position** is the most useful control on a real cabinet. At the cap it is bright and hard and the cone
  breakup peaks are at their strongest; out at the edge the top end goes and the low-mid comes up.
- **Distance** under about six inches adds low end through the proximity effect — a great deal of it on a
  ribbon, much less on a moving coil. Further back costs top end and picks up the floor the cabinet stands on.
- The microphones are archetypes, not measurements of particular boxes: a moving coil with a presence peak, a
  large-diaphragm dynamic, a ribbon, a condenser, and a room pair.

**Legacy** is the last entry in the Cabinet list, and it is the pair of responses TubeForge used before the
model existed. A project saved before this feature opens on it and sounds exactly as it did; a new session
starts on a real cabinet. If you want an old rig re-voiced, choose a cabinet — nothing does it for you.

### The response plot

Underneath the controls is the section's magnitude response: slot A and slot B thin, and what the section as a
whole does bright on top. It follows a loaded impulse response as readily as the model, so it is also the
quickest way to see what an unfamiliar IR file actually is.

The plot sums the two slots as though they were in phase, which over-states the sum wherever they are not. The
reading for that is the mono-fold meter, which is measured from the audio rather than predicted — so the plot
shows the voicing and the meter shows the cancellation.

### Cabinet Match

Once you have matched a rig to a song on the Song Match page, **Match to the song** fits a cabinet to it. It
measures the difference between your rig playing that song's reference part and the reference itself, and
builds a cabinet from that difference into slot A. **Match depth** decides how much of the measured difference
to apply; it is an argument to the fit rather than a control on the rig, so once a matched cabinet is loaded
the depth it was built with is part of it.

**What it produces, and what it does not.** A cabinet is a linear filter, so the difference between two
spectra *is* the filter that turns one into the other — which is why this costs one measurement rather than
the minutes a rig search takes. But the reconstruction has no separate DI: it plays the reference through each
candidate amplifier and scores the result. So the difference measured here contains the amplifier, the
microphone, the room and the mastering as well as the speaker. It is a corrective filter for **your rig
against that record**, which is a genuinely useful thing to have. It is not a recovery of the cabinet that was
in the room, and the page names the closest built-in cabinet as "closest to" rather than as an identification
for that reason.

The confidence figure says how much correction was needed. High means your rig was already close and the
cabinet only had to nudge it; low means the two are not very comparable and what you are applying is a large
correction rather than a close fit — which usually means the amplifier is the thing to work on first.

Slot A has to be on a built-in cabinet for this to run. The match needs a known starting point to measure
against, and a loaded impulse response is not something it can subtract.

- **A / B blend** mixes the two. At either end only one convolution runs, which is half the cabinet's CPU cost.
- **Stereo width** separates the two responses across the field instead of summing them — one guitar through
  two different rigs, which is how a wide rhythm sound is actually made. Each response's **Placement** decides
  where it goes; the defaults are hard left and hard right.
- **Delay** holds a response back. Two microphones at different distances from a speaker are at different
  times, and a few samples of that is most of what a mic blend sounds like. **Align automatically**
  cross-correlates the two loaded responses and writes the measured lag into whichever one is early, with a
  confidence figure — it will tell you when two responses are too unalike to align rather than inventing a
  number.
- **Mono fold** reports what a mix bus will do to the stereo image. Two different responses split left and
  right fold to mono nearly intact; a delay between them combs. Past −6 dB something is cancelling.
- **Low cut**, **High cut**, **DI blend** and **Cabinet output** were previously set by the voicing with no way
  to see or change them. High cut is the one that decides how dark the speaker is, and it is the cabinet
  setting Song Match fits from the reference.

On the Eco performance tier the whole of response B is switched off: a second convolution is what that tier
exists to avoid, and a slot's own level could otherwise quietly buy it back.

### Loading your own responses

**Browse** opens your impulse response folder as a list. Point it at wherever you keep your packs — nothing is
copied, the folder you already have *is* the library, so renaming a file there renames it here. Selecting a
response loads it straight into the slot, so you can listen through a pack without a dialog between each one.
Search matches names and folder names, the folder rail narrows to one pack, **Favourite** pins a response to
the top of the list, and **Recents** is the last dozen you tried. Favourites and recents are stored with your
preferences rather than with the project: they describe your shelf, not one rig.

**File** is the ordinary dialog, for a response that is not in that folder. You can also drop a `.wav`,
`.aiff` or `.flac` straight onto either slot.

While a file is loaded, three controls replace the cabinet chooser:

- **Response length** decides how much of it to use. Past the first few milliseconds an impulse response is
  mostly room, so a shorter one is drier *and* cheaper at the same time. Still capped by the performance tier.
- **Normalisation** — Peak matches the loudest sample, which makes a room-heavy response quieter than a
  close-mic one at the same blend setting; RMS matches what is heard, so the blend control means what it says;
  None leaves the file exactly as its author wrote it, which is what you want when comparing two vendors' packs.
- **Minimum phase** rebuilds the response with the same frequency content but all of its energy at the front.
  Two responses from different sources rarely start at the same place, and that difference combs when they are
  blended — this removes it without changing either one's tone. Off by default: for one response on its own,
  the phase it was measured with is part of what was measured.

**Export** writes what a slot is convolving to a 24-bit WAV. It is there mainly for Cabinet Match, whose result
is generated rather than loaded and would otherwise live only inside one project.

A project remembers the path to each loaded response *and* a fingerprint of the file's contents. If the file
at that path has changed since the project was saved — a pack updated in place, or a name reused — the slot
says so rather than letting a mix quietly re-voice itself. If it has gone entirely, the built-in cabinet takes
over and the path is kept, so reopening the project on a machine that has since synced the folder finds it.

A stereo file is used as it is. A **four-channel true-stereo** capture is used as one: those four responses are
what the left input does to each output and what the right input does to each, and all four are convolved, so
the cabinet has the stereo depth it was captured with rather than the two direct paths alone. The slot says
"true stereo" when it has one. Minimum-phase conversion deliberately does not apply to the cross terms — their
arrival time *is* the information they carry, and flattening it would remove the depth that made the file
worth loading.

**Sharing a rig with its cabinet.** On the Profile Library page, **Include my cabinet responses** copies the
responses your slots have loaded into an exported `.ntone`, so the rig arrives complete on a machine that has
never seen those files. It is off by default on purpose: most commercial impulse responses are licensed for
you to *use* rather than to pass on, and the plug-in cannot read a licence. Switch it on only for responses
you have the right to share.

## Workflows

- Neural Capture plays a capture from the library, or loads a model folder exported by the capture wizard;
  either way its hashes, schema, test vectors, and model must validate before it can reach the audio thread.
- Circuit Engineering lets you switch the preamp tube, power tube and topology, tone-stack network, and solver. Amp/Advanced knobs update the underlying component values; the response plot is calculated from the selected tone network. Structural edits compile off the audio thread and are restored with the project. The speaker is on the Cabinet page and is shared with every engine, so this page no longer carries one of its own.
- Tone Analyzer analyzes a local isolated clip and compares its embedding with the local tone database.
- Song Reconstruction uses local neural guitar/bass stems and lists timestamped clean, crunch, and distorted
  parts with dominant pitch, likely tuning family, cent offset, and confidence. Select a part and choose
  **Use selected part** to rebuild the playable candidates from that section; source audio remains excluded
  from exports.
- Tone Assistant explains bounded recommendations and requires Preview before Accept; Reject and Undo are exact.
- Profile Library searches by name/author/tag, filters instrument or favorites, imports `.ntone` directories,
  applies compatible rigs, and exports the current rig. Unsigned local packages show a warning.

### Auto Match

**Auto Match** is on the Song Match page, and it changes who sets the amplifier. Off — the default — applying
a candidate writes the rig once and then leaves it alone, exactly as it always has. On, the analysis takes
every control it fitted: Gain, Bass, Mid, Treble, Presence, Resonance and Master, the whole Tone Shaping page
including the noise gate, the voicing, and the cabinet section. A pill in the corner of the Amplifier, Pedals
and Tone Shaping pages says so while it is holding them.

With Auto Match on you do not press **Apply**: the moment a match finishes, its best candidate goes on the
amplifier. Choose a different part of the song and the search re-runs and re-applies itself, keeping any
control you had already taken over. **Bypass pedals and effects on apply** still decides whether the board and
the sends come with it.

Changing **Instrument** while a match is held is not treated as an edit — it re-runs the match. Guitar and bass
are searched over different amplifier voicings, so a guitar match carried onto a bass rig would be holding
settings the search would never have chosen for it. The rig waits, unheld, until the new match completes.

You can still change any of them. The first time you reach for a control the analysis has set, TubeForge asks
once:

- **Keep the matched value** puts the analysis's setting back and carries on holding it.
- **Change it anyway** keeps what you dialled and hands that one control back to you permanently. It turns
  green on the panel, and Auto Match stops writing it — everything else stays held.
- **Turn Auto Match off** stops holding everything. Nothing is reverted; the rig stays exactly as it sounds.

**Give them back** on the Song Match page restores the matched values on every control you have taken over.
Ticking **Don't warn me again** stops the question, not the change: controls you move are still handed back to
you, quietly. That answer is remembered between sessions, and a **Warnings: off** button appears next to Auto
Match so you can ask to be warned again.

**Follow the live signal** — off by default — lets a held rig track what you are actually playing. It moves
two controls and no others: the input trim, aimed to put your incoming peak where the preamp expects it, and
the gate threshold, sat just above the measured noise floor. Both stay within 6 dB of what the match set, and
both stop moving the moment you take that control over. Reach for it when you swap to a guitar with much
hotter or weaker pickups; if you find yourself wanting more than 6 dB of correction, re-run the match instead.

Auto Match survives saving: a `.tforge` project remembers the matched values, which controls you had taken
back, and reopens still holding them.

Two things Auto Match deliberately does not do. It does not fight your DAW — a control moved by host
automation, a preset or a foot controller is handed back without a dialog, and the page tells you afterwards
which one. And it does not re-fit the rig while you play: it sets the rig when a match completes and then
holds it, because the search that finds a rig renders dozens of candidates and takes seconds to minutes.

Applying a candidate now also normalises three controls that used to be left where you had them, because all
three silently changed the sound away from the candidate you auditioned: **Gain** goes to 5 (it is a macro
that adds up to ±15 dB across all four preamp stages), the **panel switch** is cleared, and **oversampling**
goes to Auto. This happens whether or not Auto Match is on.

Projects use `.tforge`; portable tone profiles use `.ntone`. Back up both. If audio glitches, disable 8x
oversampling, shorten convolution IRs, raise the host buffer, confirm the reported latency, and check the DSP,
callback, peak, and dropout readings. The installer includes a diagnostic collector that packages logs and
system metadata but no audio, project/profile files, or source file names.
