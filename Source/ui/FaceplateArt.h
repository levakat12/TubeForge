#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstddef>
#include <vector>

namespace tf::ui
{
/** The cloth stretched across the speaker opening.

    Cosmetic, but it is the strongest single cue for which amplifier you are looking at: a
    player reads a grille from across a room, long before they read a badge. More patterns are
    one `switch` case in `makeClothTile` -- the enum is deliberately short rather than
    speculative, so every value here is one some style actually asks for.
*/
enum class GrilleWeave { tight, basket, perforated };

/// How the control strip catches light. Changes the panel fill only; the layout is identical.
enum class PanelFinish { brushed, painted };

/** What a voicing sounds like, in the only three words a player needs to start looking.

    A browsing concept rather than an engine one: nothing in the DSP reads it. It lives on the
    style row because that table already has exactly one entry per topology, and a second table
    keyed the same way is a second thing to keep in step.

    The order is the order the categories appear, and it is the order a player works through a
    shop: clean first, then crunch, then the high-gain end.
*/
enum class AmpCharacter { clean, crunch, hiGain };

inline constexpr std::size_t ampCharacterCount = 3;

/// The rail label for a character. Indexed by `AmpCharacter`.
[[nodiscard]] juce::String ampCharacterName(AmpCharacter character);

/** One amplifier's appearance.

    Every field is a colour, a small enum or a scalar, so adding a voicing is a table row in
    `faceplateStyle` rather than new drawing code. That is the point: `nts::amp::Topology` will
    grow, and the art has to grow with it without anyone reopening the renderer.

    One rule constrains the palette, and it is worth stating because it looks like timidity
    otherwise: **the panel stays dark**. It is the surface the knob captions and value read-outs
    sit on, and a cream or chrome control strip inside an otherwise dark editor forces every
    piece of text on it to switch colour to stay legible -- which is how the old artwork ended
    up under a flat 32% black scrim that dimmed the whole faceplate to protect a strip across
    the bottom third of it. Identity is carried by the covering, the grille, the piping, the
    brackets and the lamp instead, which is how an amplifier is actually recognised anyway.
*/
struct FaceplateStyle
{
    juce::String badge { "TubeForge" };

    /// Cabinet covering. `grain` runs 0 (smooth vinyl) to 1 (coarse pebbled leather).
    juce::Colour tolex { 0xff1c1a19 };
    juce::Colour tolexLift { 0xff2b2826 };
    float grain { 0.7f };
    /// Corner hardware, and the hairline around the panel. Steel, brass, nickel.
    juce::Colour bracket { 0xff8f9296 };

    GrilleWeave weave { GrilleWeave::tight };
    juce::Colour clothDark { 0xff0a0a0b };
    juce::Colour clothLight { 0xff24262b };
    /// The trim line around the grille. Thin and bright is modern; thick and gold is not.
    juce::Colour piping { 0xffb9bcc1 };

    PanelFinish finish { PanelFinish::brushed };
    juce::Colour panelLow { 0xff191a1d };
    juce::Colour panelHigh { 0xff2f3237 };
    /// Drives the knob captions and value read-outs as well as the badge, so a style can never
    /// pick a panel its own lettering disappears into.
    juce::Colour panelText { 0xffe8ebf0 };

    juce::Colour lamp { 0xff9fd8ff };
    /// Square corners read modern; a big radius reads like a sixties combo.
    float cornerRadius { 6.0f };

    /// Which shelf this voicing sits on when it is being browsed rather than played.
    AmpCharacter character { AmpCharacter::hiGain };
    /// One line saying what it sounds like. Shown under the name in the picker, where a player
    /// is choosing between seven names they have never heard before.
    juce::String blurb;
};

/** The style for a voicing, indexed by the `topology` and `instrument` choice parameters.

    Indices rather than `nts::amp::Topology` so a page never has to know what a topology is,
    exactly as the topology combo box on the tone page does. The implementation sizes its table
    against `nts::amp::topologyCount`, so a voicing appended to the enum without a livery here
    is a compile error rather than something to notice by eye. `faceplateStyleCount` is still
    published so a page can check itself against the choice parameter, and out-of-range indices
    fall back to the first style rather than reading past the table.
*/
[[nodiscard]] FaceplateStyle faceplateStyle(int topologyIndex, int instrumentIndex);

/// How many topologies the art covers. Compare against the choice parameter's option count.
[[nodiscard]] std::size_t faceplateStyleCount() noexcept;

/** Draws an amplifier faceplate: covering, grille, badge, pilot lamp, control panel, corners.

    Drawn rather than photographed. The page it fills is not a fixed aspect ratio, and the
    artwork this replaced was a 2.5:1 image cropped by `fillDestination` to cover it -- so the
    corner brackets ran off the sides at most window widths and the knob row was anchored to
    proportions reverse-engineered from the picture. Here the geometry is the source of truth:
    `deckTop` and `deckBottom` say where the knobs go, the panel is painted to fit them, and
    both stay right at any size or display scale.

    Textures are the only raster work, and they are small tiles built once per style change,
    not per repaint -- a fixed seed, so the covering does not crawl every time the editor ticks.
*/
class FaceplateArt
{
public:
    /// Where the knob row sits, as fractions of the faceplate's height. The panel is painted
    /// to enclose this band, so the layout and the art cannot drift apart.
    static constexpr float deckTop = 0.618f;
    static constexpr float deckBottom = 0.930f;

    void setStyle(const FaceplateStyle& style);
    [[nodiscard]] const FaceplateStyle& style() const noexcept { return current; }

    void paint(juce::Graphics& graphics, juce::Rectangle<float> area) const;

private:
    FaceplateStyle current;
    juce::Image grainTile, clothTile, brushTile;
};

/** Rendered faceplates at thumbnail size, kept so a grid of them costs one render each.

    `FaceplateArt::setStyle` rebuilds three procedural tiles -- a 96x96 grain field, a weave
    cell and a 64x64 brush field. That is nothing once per voicing and indefensible once per
    tile per frame, which is what a picker showing seven faces during a 180 ms animation would
    otherwise do.

    The physical pixel scale is part of the key, so a window dragged to a display with a
    different scaling re-renders instead of showing a soft image at the new size.
*/
class FaceplateThumbnails
{
public:
    /// Draws the face for a voicing into `area`, rendering it first if this is the first ask.
    void paint(juce::Graphics& graphics, juce::Rectangle<float> area,
               int topologyIndex, int instrumentIndex);

    /// Drops every cached image. Worth calling when a picker closes if memory matters more
    /// than the next open being instant; nothing calls it yet.
    void clear() { entries.clear(); }

private:
    struct Entry
    {
        int topology {}, instrument {}, width {}, height {};
        float scale {};
        juce::Image image;
    };

    std::vector<Entry> entries;
};
} // namespace tf::ui
