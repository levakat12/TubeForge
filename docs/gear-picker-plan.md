# Gear picker plan: choosing amps and pedals by their face

An implementation-ready plan for replacing the two choice combo boxes that pick an amplifier
voicing and a pedal kind with one shared component: a chip that shows what is currently loaded and
expands smoothly into a panel of gear faces, grouped by the character of the sound rather than by
the order of an enum.

Line references are against the working tree at the commit that added `Source/ui/FaceplateArt.cpp`;
re-check them before editing.

## Status

| Track | Items | Effort | State |
|---|---|---|---|
| A — The picker itself | A1–A5 | 2.5 d | ✅ Complete |
| B — Amp faces and the amp page | B1–B4 | 1.0 d | ✅ Complete |
| C — Pedal faces and the slots | C1–C5 | 2.5 d | ✅ Complete |
| D — Keyboard, screen readers, tests | D1–D3 | 1.0 d | ✅ Complete |
| E — Documentation | E1 | 0.25 d | ✅ Complete |

**Coverage: 100% by effort** (7.25 of 7.25 days). Both halves of the feature work end to end: the
amp chip on the Amplifier page, a chip per slot on the Pedals page, seven amplifiers shelved under
Clean / Crunch / Hi-gain and seven pedals under Empty / Clean & Dynamics / Overdrive / Distortion /
Fuzz / Captures. Choosing a capture chains straight into the existing capture library rather than
leaving a slot with no model in it.

### One qualification on D1

The accessibility work is verified through **UI Automation**, not by listening to a screen reader.
Querying the running editor's automation tree shows the chip as a Button named `Voicing, Vintage
Bloom` carrying its keyboard instructions as help text, and the open panel as a Window named
`Choose an amplifier` carrying "Arrow keys move between entries, Return chooses, Escape closes
without changing anything." That is the layer a screen reader reads, so the exposure is real rather
than assumed — but nobody has yet sat with NVDA or Narrator and listened to it, and the highlight
announcements posted through `AccessibilityHandler::postAnnouncement` are the part that check
cannot see. **Worth one session with a screen reader before calling it shipped.**

### What changed against the plan

- **A2 gained a watchdog that was not planned.** `VBlankAttachment` is the right tool, but if the
  display link never delivers a frame the panel sits at zero expansion: invisible, full-size, and
  swallowing every click meant for the page underneath. A 30 Hz timer now checks that frames are
  arriving and drives the animation itself if they stop. It costs nothing while vsync is working.
  This was found in testing, not by reading the code.
- **A2 also stops repainting once open.** The original design animated continuously; with a full
  `FaceplateArt::paint` behind the panel that is the most expensive way available to draw nothing.
- **`GearChip::invalidate` was added.** The chip watches its own parameter, but the amp faces also
  depend on the *instrument*, and a bass rig wears a different cabinet. Without a way to be told,
  the chip kept showing the guitar face until the voicing happened to change.
- **Clean is no longer empty.** The seven topologies landed first, so A5's empty-category handling
  is implemented but no category is currently empty to exercise it.
- **Tile size became part of the catalogue.** The plan fixed a tile at 220×120, which is right for
  a faceplate and wrong for a stompbox: a 2:3 pedal in a 220-wide tile sits between two large empty
  margins and fits three per row where seven would go. `GearCatalogue` now carries `tileWidth` and
  `tileFaceHeight`; pedals use 132×168.
- **`GearChip::faceAspect` was added** for the same reason, so the swatch on a pedal chip is
  portrait rather than landscape.
- **`GearChip::onChosen` was added** so a page can act on a choice after the parameter is written.
  C4 needs it, and it has to fire *after* the write so the page sees the new kind.
- **The kind hints on the Pedals page now come from the face table.** They were a second copy of
  the same sentences the picker shows, which is two chances to describe one pedal two ways.
- **Arrow-key row movement was wrong and is fixed.** `columnsPerRow` inferred the grid width from
  the length of the first shelf, so a shelf of two above a shelf of three made Down move by two
  everywhere and left the last tile of every wider shelf unreachable from above. The grid now
  reports the width it was actually laid out for.
- **The picker opens on what is already loaded**, so the first arrow key steps away from the
  current choice rather than from the top of the list. Not in the plan, and obvious the moment it
  was used.
- **D3's harness is `nts_art_renderer`**, a console target linking only the two art files. It is
  deliberately outside the `check` target: it asserts nothing and always succeeds, because the
  only way to review procedural artwork is to look at it.

## What exists today

Two unrelated combo boxes, both plain `juce::ComboBox` filled from their own choice parameter:

| | Where | Populated by | Bound by |
|---|---|---|---|
| Amp voicing | [ToneShapingPage.h:25](../Source/ui/ToneShapingPage.h#L25), laid out at [ToneShapingPage.cpp:143](../Source/ui/ToneShapingPage.cpp#L143) | `populateFromParameter` ([:85](../Source/ui/ToneShapingPage.cpp#L85)) | `ComboBoxAttachment` ([:112](../Source/ui/ToneShapingPage.cpp#L112)) |
| Pedal kind, ×4 slots | [PedalboardPage.h:39](../Source/ui/PedalboardPage.h#L39) | `populateFromParameter` ([PedalboardPage.cpp:84](../Source/ui/PedalboardPage.cpp#L84)) | `ComboBoxAttachment` ([:116](../Source/ui/PedalboardPage.cpp#L116)) |

Three things already in the tree do most of the work and should be reused rather than reinvented:

- **`CapturePicker::openOver`** ([CapturePicker.h:43](../Source/ui/CapturePicker.h#L43)) is the
  house pattern for an in-place overlay: it fills its parent, owns itself, and deletes itself on
  dismissal. Its deferred-delete comment ([CapturePicker.cpp:76](../Source/ui/CapturePicker.cpp#L76))
  documents a real use-after-free that the new picker will hit identically, because it will also
  dismiss from a click callback.
- **`FaceplateArt`** ([FaceplateArt.h](../Source/ui/FaceplateArt.h)) already renders an amplifier
  into an arbitrary rectangle from a table row. A thumbnail is that same call into a small image.
- **`tf::ui::Glyph` / `IconButton`** ([ShellWidgets.h:11](../Source/ui/ShellWidgets.h#L11)) is the
  established way this codebase draws rather than ships assets, and the category rail should follow
  it.

## The design

**Collapsed.** A chip roughly 200×40: a small render of the gear's own face on the left, its name
and character beside it, a chevron on the right. On the Amplifier page this sits in the existing
chip row next to Instrument and Cabinet ([AmplifierPage.h:36](../Source/ui/AmplifierPage.h#L36)); on
the Pedals page it replaces the kind combo inside each slot card.

**Expanding.** On click the chip's bounds animate to a panel filling the page, over about 180 ms on
a cubic ease-out. The panel's contents fade in over the last 40% of that, so the user sees a shape
growing rather than a list appearing. Collapse is the same curve reversed, ending exactly on the
chip's bounds so the panel appears to fold back into the thing that opened it.

**Expanded.** A category rail down the left — for amps: Clean, Crunch, Hi-gain; for pedals: Clean &
Dynamics, Overdrive, Distortion, Fuzz, Captures — and a grid of face tiles to the right. A tile is
the gear's face at about 220×120 with its name under it. Hovering lifts a tile and lights its pilot
lamp; the current selection carries the accent hairline. Choosing collapses the panel and writes
the parameter.

**Categories are the point.** "Tight Modern" and "Vintage Bloom" tell a player nothing until they
have tried both. "Hi-gain" and "Crunch" tell them where to look first, which is the entire reason
for the change.

### Proposed groupings

Amps, against the seven voicings sketched in the topology discussion. Only the two in bold exist
today — see [Dependencies](#dependencies).

| Character | Voicings |
|---|---|
| Clean | American Clean, Studio Direct |
| Crunch | **Vintage Bloom**, British Crunch, Class-A Chime |
| Hi-gain | **Tight Modern**, Sagging Rectifier |

Pedals, against `PedalKind` ([PedalBoard.h:26](../engine/pedals/include/nts/pedals/PedalBoard.h#L26)):

| Category | Kinds |
|---|---|
| *(standing tile, no category)* | `none` — "Empty" |
| Clean & Dynamics | `boost`, `compressor` |
| Overdrive | `overdrive` |
| Distortion | `distortion` |
| Fuzz | `fuzz` |
| Captures | `neuralCapture` |

`none` gets a permanent tile pinned above the rail rather than a category of its own. It is the
default and the way a slot is emptied, and burying the most-used entry one category deep would be a
regression over the combo box. Its tile is an empty pedal-shaped outline, not a face.

## Decisions, and why

**One component for both, parameterised by a data source.** Two pickers that look alike will drift
in exactly the way `populateFromParameter` was written to prevent
([UiSupport.h:27](../Source/ui/UiSupport.h#L27)). `GearPicker` takes a `std::span<const GearTile>`
and a `juce::RangedAudioParameter&`; it knows nothing about amps or pedals.

**`ComboBoxAttachment` cannot be used and must not be faked.** The picker is not a combo box, and
driving a hidden one is the kind of shortcut that breaks host automation silently. The picker writes
its parameter explicitly:

```
parameter.beginChangeGesture();
parameter.setValueNotifyingHost(parameter.convertTo0to1(index));
parameter.endChangeGesture();
```

The gestures are not optional — a host recording automation needs the touch boundaries, and a
choice written without them shows up as an un-writable lane in several DAWs.

**The picker reads its parameter every tick, it does not cache a selection.** The topology can move
under it from a preset recall, from `applyRecoveredRig`
([PluginProcessorHost.cpp:146](../Source/PluginProcessorHost.cpp#L146)), or from host automation.
`AmplifierPage::refresh` already polls exactly this way for the faceplate
([AmplifierPage.cpp](../Source/ui/AmplifierPage.cpp)) and the picker should use the same route.

**Animate on the vsync, not on the shell timer.** The shell ticks at 20 Hz
([PluginEditor.cpp:232](../Source/PluginEditor.cpp#L232)), which is a visibly steppy expansion.
`juce::VBlankAttachment` is available in the vendored JUCE
(`modules/juce_gui_basics/windows/juce_VBlankAttachment.h`) and matches the display; it is the right
tool and costs nothing while the picker is closed because the picker only exists while open.

**Character lives on the existing style tables, not in a third table.** `FaceplateStyle` already has
one row per topology; add a `character` field there. A separate UI-side category table would be a
second thing to keep in step with the choice parameter, and there is already a `jassert` in
`AmplifierPage`'s constructor guarding exactly that kind of drift.

**Keep a combo box somewhere.** See [D1](#d1--keyboard-and-screen-reader-parity); this is the
mitigation for the accessibility a custom picker throws away, and it is nearly free.

## Track A — The picker itself

### A1 · `GearPicker` overlay shell

**Change.** New `Source/ui/GearPicker.{h,cpp}`. Model on `CapturePicker`: `openOver(parent, ...)`,
self-owning, deferred delete through `MessageManager::callAsync` behind a `dismissing` flag. Data in:

```cpp
struct GearTile
{
    int parameterIndex;      // index into the choice parameter
    juce::String name;
    juce::String blurb;      // one line, what it sounds like
    int category;            // index into the category list; -1 pins it above the rail
    std::function<void(juce::Graphics&, juce::Rectangle<float>)> paintFace;
};
```

`paintFace` rather than a `juce::Image` so the caller decides whether a face is drawn live or served
from a cache, and so `GearPicker` never learns what an amplifier is.

**Files.** New pair, plus three `target_sources` lists in `CMakeLists.txt` — the plugin, the
standalone and `TUBEFORGE_SHELL_SOURCES` (around lines 313, 425 and 677; there are exactly three).

**Risk.** Low. The lifetime hazard is already solved and documented in `CapturePicker`.

**Done.** A picker opens over a page with hard-coded tiles, dismisses on Escape and on background
click, and leaks nothing under the JUCE leak detector.

### A2 · Expand and collapse

**Change.** Store the launching chip's bounds in the picker's coordinate space, and interpolate
between it and the full panel rect on a `VBlankAttachment`. Cubic ease-out, 180 ms out, 140 ms back.
Contents live in a child component whose alpha ramps 0→1 across the last 40% of the expansion, so
text is never drawn at a scale it was not laid out for.

**Risk.** Medium, and it is a repaint-cost risk rather than a correctness one. The page behind the
overlay repaints every frame while the panel is translucent, and on the Amplifier page that page is
a full `FaceplateArt::paint` — tiled fills, two clip regions and several gradients. **Measure before
optimising.** If it does not hold frame rate, snapshot the page into an image once at open and blit
that during the animation; do not start there.

**Done.** Expansion holds 60 fps on the reference machine with the Amplifier page behind it, with
the frame time recorded in the plan's follow-up notes.

### A3 · Category rail and tile grid

**Change.** Rail on the left, tiles in a flow grid that reflows on resize. Categories filter rather
than page — selecting one scrolls the grid to it and highlights the rail entry, so a user who does
not think in categories can still scroll past everything.

**Risk.** Low.

**Done.** Every entry in the source parameter is reachable, including any whose category is empty.

### A4 · Selection, hover and commit

**Change.** Hover lifts a tile 2 px and lights its lamp; the selected tile carries the accent
hairline. A click commits, collapses, and writes the parameter with the gesture pair above.

**Risk.** Low.

**Done.** Choosing an entry moves the parameter, and moving the parameter from outside moves the
selection.

### A5 · Empty and sparse categories

**Change.** A category with no entries renders as a dimmed rail item with a one-line "nothing here
yet" panel, not as a missing row. This is not a hypothetical: **Clean is empty until the new
topologies land**, and a rail that silently omits it would make the plan's own dependency invisible.

**Risk.** Low.

**Done.** With today's two topologies, Clean shows and explains itself.

## Track B — Amp faces and the amp page

### B1 · Thumbnail cache

**Change.** A small cache keyed on (topology, instrument, tile size) rendering `FaceplateArt` into a
`juce::Image` once per key.

**Why it matters.** `FaceplateArt::setStyle` rebuilds three procedural tiles — a 96×96 grain field,
a weave cell and a 64×64 brush field. Doing that per tile per frame during an animation would be
indefensible; doing it once per style at open is nothing. Render on open, not in the constructor, so
a user who never opens the picker never pays.

**Risk.** Low. Note that thumbnails must be re-rendered on DPI change, or they will be soft on a
scaled display.

**Done.** Opening the amp picker costs one render per voicing and none thereafter.

### B2 · `character` on `FaceplateStyle`

**Change.** Add `GearCharacter character` to `FaceplateStyle`
([FaceplateArt.h](../Source/ui/FaceplateArt.h)) and fill it in `styleTable()`. Today: Tight Modern →
hi-gain, Vintage Bloom → crunch.

**Risk.** Low.

**Done.** `faceplateStyle(i, j).character` is the only source of an amp's category.

### B3 · Topology chip on the Amplifier page

**Change.** Add the chip to the existing chip row alongside Instrument and Cabinet. The row is
`chipRowHeight` = 41 px and currently uses 160 + 130 px of a full-width strip
([AmplifierPage.cpp](../Source/ui/AmplifierPage.cpp)), so there is room without touching the
faceplate area below it.

**Polish, optional.** Choosing a voicing re-skins the faceplate behind the collapsing panel, which
is the best moment in the whole feature. `FaceplateArt` has no cross-fade — it swaps styles
outright. Blending two styles means holding two sets of tiles and painting twice; worth doing, but
only after the rest works, and it is the first thing to drop if the track runs long.

**Risk.** Low.

**Done.** Topology is selectable from the Amplifier page and the faceplate follows.

### B4 · The Tone Shaping combo stays

**Change.** Leave [ToneShapingPage](../Source/ui/ToneShapingPage.cpp)'s topology combo exactly as it
is.

**Why.** It costs nothing, it is the keyboard and screen-reader path (see D1), and the Tone Shaping
page is where someone already deep in per-stage settings would expect to find it. Two controls on
one parameter stay in sync through the value tree without any extra work.

**Risk.** None.

**Done.** Both routes move the same parameter.

## Track C — Pedal faces and the slots

### C1 · `PedalArt`

**Change.** New `Source/ui/PedalArt.{h,cpp}`, built the same way as `FaceplateArt`: a `PedalFace`
table with one row per `PedalKind`, and a renderer that draws an enclosure, a knob cluster, a
footswitch, an LED and a nameplate.

**This is the single largest item in the plan** and the reason Track C is not Track B. Suggested
per-kind identities, to be argued with before anyone draws them:

| Kind | Enclosure | Cue |
|---|---|---|
| `boost` | brushed silver, one knob | clean, minimal, no graphics |
| `compressor` | pale blue hammertone, two knobs | a needle-style meter face |
| `overdrive` | green, three knobs | cream nameplate, the classic |
| `distortion` | orange-red, three knobs | bold slab lettering |
| `fuzz` | grey crackle, two knobs, large | round footswitch, no LED |
| `neuralCapture` | matte black, three knobs | small screen instead of a nameplate |
| `none` | outline only | dashed border, no hardware |

**Risk.** Medium — it is the most drawing work here, and the lesson from `FaceplateArt` applies
directly: procedural noise lattices must use cell counts that divide the tile edge exactly, or the
texture beats against the pixel grid and reads as mottled stone. That is commented at the fix site
in `FaceplateArt.cpp` and should be read before starting.

**Done.** Seven distinguishable pedals render at tile size and at chip size.

### C2 · Categories on `PedalFace`

**Change.** As B2, a `category` field on the table row.

**Done.** The grouping table above is expressed in one place.

### C3 · Slot chips replace the kind combos

**Change.** Each slot card's `kind` combo becomes a chip opening a `GearPicker` over the Pedals page,
scoped to that slot's kind parameter
([PluginProcessor.h:159](../Source/PluginProcessor.h#L159) for the id scheme).

**Risk.** Low on the audio side, and worth stating explicitly: `PedalSlot` already rides the wet/dry
blend down before swapping kind
([PedalBoard.h:75](../engine/pedals/include/nts/pedals/PedalBoard.h#L75)), so a kind chosen from a
picker is exactly as click-free as one chosen from a combo. No engine change is needed.

**Done.** All four slots pick through the picker and the four `ComboBoxAttachment`s are gone.

### C4 · Captures chain into the existing picker

**Change.** Choosing the Captures tile sets the kind and then immediately opens `CapturePicker` for
that slot, rather than leaving the user to find "Load capture" afterwards.

**Why.** Selecting `neuralCapture` without a model is a slot that does nothing, which today is
recoverable only by noticing the status line. Chaining is one call and removes the dead end.

**Risk.** Low, but the two overlays must not be up at once — collapse the gear picker fully before
opening the capture picker, and take the same deferred-delete care.

**Done.** Choosing Captures ends with a model loaded or with the slot back on its previous kind.

### C5 · Pedals page relayout

**Change.** The slot cards were laid out around a combo; a chip is a different shape. Re-flow the
card and keep the four-knob row where it is.

**Risk.** Low.

**Done.** Four cards fit at the minimum window size without clipping.

## Track D — Keyboard, screen readers, tests

### D1 · Keyboard and screen-reader parity

**This is the real cost of the feature and the item most likely to be skipped.** `juce::ComboBox`
brings focus handling, arrow-key traversal, type-to-select and an accessibility handler for free.
A custom picker starts with none of it.

**Change.** The chip is focusable and opens on Return or Space. Inside the panel: arrow keys move
between tiles, Home/End jump, Return commits, Escape cancels back to the previous value. Give
`GearPicker` an `AccessibilityHandler` with the list role, and give each tile an accessible name of
"*name*, *category*". Retain the Tone Shaping combo (B4) as the guaranteed-accessible route for
topology; the Pedals page has no such fallback, so **its picker must carry full keyboard support
before C3 ships**.

**Risk.** Medium — easy to declare done without testing. Verify with a screen reader, not by
reading the code.

**Done.** Every action is reachable from the keyboard alone, and a screen reader announces the
current selection.

### D2 · Tests

**Change.** Extend `Tests/WrapperTests.cpp`, which already drives parameters by id and constructs the
editor:

- choosing tile *n* sets the choice parameter to *n*, with a begin/end gesture pair around it;
- moving the parameter from outside moves the picker's selection;
- every entry of each choice parameter appears in exactly one tile — the drift guard, as a test
  rather than only a `jassert`, since `jassert` is compiled out of the shipping build.

**Done.** `nts_wrapper_tests` covers all three and passes.

### D3 · Visual check

**Change.** The throwaway harness used to iterate on `FaceplateArt` — a console app linking the art
files and writing PNGs — should be committed under `Tests/` this time rather than left in a
scratch directory, so pedal faces and amp thumbnails can be eyeballed without launching a DAW.

**Note.** It must create its canvas with `juce::SoftwareImageType`. A plain `juce::Image` in a
console app yields a context that silently draws nothing on Windows, which cost an hour the first
time.

**Done.** One command renders every amp and pedal face to a folder.

## Track E — Documentation

### E1 · User guide

**Change.** Update [user-guide.md](user-guide.md) where it describes choosing a voicing and a pedal
kind, and note in [architecture.md](architecture.md) that `GearPicker` is the shared selection
component.

## Dependencies

**Track B is worth doing at two voicings but is much better at seven.** With today's `Topology`
enum the amp picker shows one tile under Crunch, one under Hi-gain and an empty Clean. Nothing
breaks — A5 exists precisely for this — but the categories only start earning their place once the
topology work lands. Either order works; doing topologies first makes the picker's first
demonstration a much better one.

**Track C depends on nothing** and can run in parallel with B.

## Open questions

1. **Does the Pedals page keep a combo box?** B4 gives topology an accessible fallback for free
   because a second control already exists. The Pedals page has no equivalent, so either its picker
   carries full keyboard support (D1, planned) or a combo stays somewhere on the card. Full keyboard
   support is the better answer and the more expensive one.
2. **Should the amp chip show the instrument too?** A bass rig has its own face, so the chip could
   read "Vintage Bloom Bass". It duplicates the Instrument control sitting beside it.
3. **How wide is a tile?** 220×120 preserves the faceplate's proportions and fits three across at
   the minimum window width. Wider tiles read better and fit two.

## Out of scope, and why

- **Amp and pedal presets inside the picker.** Choosing a voicing and choosing a saved rig are
  different actions; the preset rail at the top of the window already owns the second one.
- **Drag-to-reorder pedal slots.** Real, wanted, and unrelated to how a kind is chosen.
- **Retiring the old faceplate PNG.** Tracked separately.
- **A picker for cabinets or captures.** `CapturePicker` works, and a capture is chosen by name and
  metadata rather than by appearance — a grid of identical black rectangles would be worse than the
  list it replaced.
