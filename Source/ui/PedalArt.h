#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstddef>
#include <vector>

namespace tf::ui
{
/** What a pedal is *for*, in the words a player uses when reaching for one.

    The browsing axis, exactly as `AmpCharacter` is for amplifiers. `boost` and `compressor`
    share a shelf because neither is there to distort -- they are the two boxes you put in front
    of an amplifier to change how hard it is hit rather than what it sounds like.

    `none` deliberately has no character. It is pinned above the shelves instead: it is the
    default and the way a slot is emptied, and burying the most-used entry one category deep
    would be a regression over the combo box it replaces.
*/
enum class PedalCharacter { dynamics, overdrive, distortion, fuzz, capture };

inline constexpr std::size_t pedalCharacterCount = 5;

/// The rail label for a character. Indexed by `PedalCharacter`.
[[nodiscard]] juce::String pedalCharacterName(PedalCharacter character);

/// How the enclosure is finished. Changes the body fill only; the hardware layout is shared.
enum class PedalFinish { gloss, hammertone, crackle, matte, brushed };

/** One pedal's appearance.

    Same shape of table as `FaceplateStyle`: colours and small enums, so adding a kind is a row
    rather than new drawing code. `knobs` and `hasLed` are the two things that actually change
    the drawn hardware -- everything else is finish and colour.

    The `outline` flag is the empty slot, which is not a pedal at all: a dashed footprint with
    no hardware on it, so a board with three empty slots reads as three gaps rather than three
    identical black boxes.
*/
struct PedalFace
{
    juce::String name { "Pedal" };
    /// One line saying what it does. Shown under the name in the picker.
    juce::String blurb;

    PedalFinish finish { PedalFinish::gloss };
    juce::Colour body { 0xff2a2c30 };
    juce::Colour bodyLift { 0xff3a3d43 };
    /// The printed nameplate and any lettering on the enclosure.
    juce::Colour plate { 0xff11120f };
    juce::Colour lettering { 0xffe8e4d8 };
    juce::Colour knobCap { 0xff141416 };
    juce::Colour led { 0xffff3b30 };

    /// One to three. Drawn evenly across the upper third of the enclosure.
    int knobs { 3 };
    bool hasLed { true };
    /// Draws a footprint rather than a pedal. Only the empty slot uses it.
    bool outline {};

    PedalCharacter character { PedalCharacter::overdrive };
    /// -1 pins the tile above the rail. Only the empty slot uses it.
    bool pinned {};
};

/** The face for a pedal kind, indexed by the slot's `kind` choice parameter.

    Indices rather than `nts::pedals::PedalKind` so a page never has to know what a pedal kind
    is; the implementation sizes its table against `kindCount`, so a kind appended to the enum
    without a face here is a compile error rather than something to notice by eye.
*/
[[nodiscard]] const PedalFace& pedalFace(int kindIndex);

/// How many kinds the art covers. Compare against the choice parameter's option count.
[[nodiscard]] std::size_t pedalFaceCount() noexcept;

/** Draws a pedal: enclosure, knobs, footswitch, LED and nameplate.

    Drawn rather than photographed, for the same reasons the faceplates are: it scales to any
    rectangle and any display, it costs no binary, and a kind added to the engine gets a face
    without anyone opening an image editor.

    The tall, narrow proportion is deliberate and is the aspect the tiles reserve. A pedal seen
    from above is roughly 2:3, and squashing one into a landscape tile makes it read as a
    generic box rather than as a stompbox.
*/
class PedalArt
{
public:
    void setFace(const PedalFace& face);
    [[nodiscard]] const PedalFace& face() const noexcept { return current; }

    void paint(juce::Graphics& graphics, juce::Rectangle<float> area) const;

private:
    PedalFace current;
    juce::Image speckleTile;
};

/** Rendered pedals at thumbnail size, so a grid of them costs one render each.

    The same bargain `FaceplateThumbnails` makes, and for the same reason: the speckle tile a
    hammertone or crackle finish needs is procedural, and rebuilding it per tile per frame
    during an animation would be indefensible.
*/
class PedalThumbnails
{
public:
    void paint(juce::Graphics& graphics, juce::Rectangle<float> area, int kindIndex);
    void clear() { entries.clear(); }

private:
    struct Entry
    {
        int kind {}, width {}, height {};
        float scale {};
        juce::Image image;
    };

    std::vector<Entry> entries;
};
} // namespace tf::ui
