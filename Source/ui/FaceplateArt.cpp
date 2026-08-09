#include "FaceplateArt.h"
#include "../TubeForgeTheme.h"

// The only engine include here, and only in the implementation: the interface stays in terms
// of parameter indices so a page never has to know what a Topology is, while the style table
// gets its size checked against the enum at compile time rather than by a runtime assert.
#include <nts/amp/TraditionalAmp.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace theme = tf::theme;

namespace tf::ui
{
namespace
{
/** Tileable value noise on a wrapping lattice.

    Wrapping is the whole reason this is here rather than a call to `Random` per pixel: the
    grain is painted with `setTiledImageFill`, so a tile whose edges do not meet its opposite
    edges draws a visible grid across the cabinet.
*/
class Lattice
{
public:
    Lattice(int cellsPerEdge, juce::Random& random)
        : size(cellsPerEdge), values(static_cast<std::size_t>(cellsPerEdge * cellsPerEdge))
    {
        for (auto& value : values) value = random.nextFloat() * 2.0f - 1.0f;
    }

    /// `u` and `v` are positions in 0..1 across one tile.
    [[nodiscard]] float sample(float u, float v) const noexcept
    {
        const auto fx = u * static_cast<float>(size);
        const auto fy = v * static_cast<float>(size);
        const auto x0 = static_cast<int>(std::floor(fx));
        const auto y0 = static_cast<int>(std::floor(fy));
        const auto tx = smoothstep(fx - static_cast<float>(x0));
        const auto ty = smoothstep(fy - static_cast<float>(y0));
        const auto top = juce::jmap(tx, at(x0, y0), at(x0 + 1, y0));
        const auto bottom = juce::jmap(tx, at(x0, y0 + 1), at(x0 + 1, y0 + 1));
        return juce::jmap(ty, top, bottom);
    }

private:
    static float smoothstep(float t) noexcept { return t * t * (3.0f - 2.0f * t); }

    [[nodiscard]] float at(int x, int y) const noexcept
    {
        const auto wrappedX = ((x % size) + size) % size;
        const auto wrappedY = ((y % size) + size) % size;
        return values[static_cast<std::size_t>(wrappedY * size + wrappedX)];
    }

    int size;
    std::vector<float> values;
};

/** The pebbled grain of a vinyl covering: two octaves, drawn over the tolex colour.

    Signed, so the tile darkens and lifts rather than only darkening -- a covering that is
    only ever shaded reads as dirt on flat paint instead of as texture catching light.

    Both octaves are deliberately fine. An octave with cells large enough to pick out is an
    octave whose tile repeat is also large enough to pick out, and a covering that visibly
    repeats every 96 pixels reads as camouflage rather than as grain -- which is exactly what
    a first attempt with a six-cell base octave produced.
*/
juce::Image makeGrainTile(float coarseness)
{
    constexpr int size = 96;
    juce::Image tile(juce::Image::ARGB, size, size, true);

    // Fixed seed: the grain is artwork, not an effect. Re-rolling it per repaint would make
    // the covering crawl every time the shell's timer ticked.
    // Cell counts must divide the tile edge exactly. A lattice whose cells land on fractional
    // pixel boundaries beats against the pixel grid, and the interference reads as mid-scale
    // mottling -- stone, not vinyl -- however fine the octave nominally is.
    juce::Random random { 0x7ef01ce5 };
    const Lattice coarse { 24, random };
    const Lattice fine { 48, random };

    const auto depth = 0.09f + 0.10f * juce::jlimit(0.0f, 1.0f, coarseness);
    juce::Image::BitmapData pixels(tile, juce::Image::BitmapData::writeOnly);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
        {
            const auto u = static_cast<float>(x) / static_cast<float>(size);
            const auto v = static_cast<float>(y) / static_cast<float>(size);
            const auto value = 0.60f * coarse.sample(u, v) + 0.40f * fine.sample(u, v);
            const auto lit = value >= 0.0f;
            // The lifted side is held well below the shaded one. On a near-black covering an
            // even split puts white speckle on top of the darkest surface in the window, which
            // reads as dust rather than as grain catching the light.
            const auto alpha = juce::jlimit(0.0f, 1.0f, std::abs(value) * depth * (lit ? 0.5f : 1.0f));
            pixels.setPixelColour(x, y, (lit ? juce::Colours::white : juce::Colours::black)
                                            .withAlpha(alpha));
        }
    return tile;
}

/// One cell of grille cloth. The cell size is the pattern's scale, so it is part of the look.
juce::Image makeClothTile(GrilleWeave weave, juce::Colour dark, juce::Colour light)
{
    const auto cell = weave == GrilleWeave::basket ? 16 : weave == GrilleWeave::perforated ? 12 : 8;
    juce::Image tile(juce::Image::ARGB, cell, cell, true);
    juce::Graphics graphics(tile);
    graphics.fillAll(dark);

    switch (weave)
    {
        case GrilleWeave::tight:
            // A fine even mesh: threads one pixel wide, crossing on a short pitch.
            graphics.setColour(light.withAlpha(0.85f));
            graphics.fillRect(1, 0, 1, cell);
            graphics.fillRect(5, 0, 1, cell);
            graphics.setColour(light.withAlpha(0.55f));
            graphics.fillRect(0, 1, cell, 1);
            graphics.fillRect(0, 5, cell, 1);
            break;

        case GrilleWeave::basket:
        {
            // Four quadrants, threads running horizontally in two of them and vertically in
            // the other two -- the over-under of a basketweave. The alternation is what makes
            // it read as woven at a distance rather than as a grid.
            const auto half = cell / 2;
            graphics.setColour(light);
            for (int quadrantY = 0; quadrantY < 2; ++quadrantY)
                for (int quadrantX = 0; quadrantX < 2; ++quadrantX)
                {
                    const auto x = quadrantX * half;
                    const auto y = quadrantY * half;
                    const auto horizontal = quadrantX == quadrantY;
                    for (int line = 1; line < half; line += 2)
                    {
                        if (horizontal) graphics.fillRect(x, y + line, half, 1);
                        else            graphics.fillRect(x + line, y, 1, half);
                    }
                }
            break;
        }

        case GrilleWeave::perforated:
        {
            // A punched metal grille rather than cloth: what a bass cabinet wears. The sheet
            // is the light colour and the holes are the dark one -- a hole is a view of the
            // unlit inside of the box, so lighting them instead turns the grille into polka
            // dots. Powder-coated black, so "light" here is only slightly the lighter of the
            // two: the contrast is a rim catching the light, not a change of tone.
            // Anchored to the dark colour rather than derived from the light one, so the sheet
            // stays the darkest region of the faceplate whatever palette a style hands it. A
            // grille is a hole in a box; it cannot be the brightest thing in the window.
            graphics.fillAll(dark.interpolatedWith(light, 0.32f));
            const std::array<juce::Rectangle<float>, 2> holes {
                juce::Rectangle<float>(1.2f, 1.2f, 5.6f, 5.6f),
                juce::Rectangle<float>(7.2f, 7.2f, 5.6f, 5.6f)
            };
            for (const auto& hole : holes)
            {
                graphics.setColour(dark.darker(0.3f));
                graphics.fillEllipse(hole);
                // Only the upper rim, so the sheet reads as having thickness.
                graphics.setColour(light.brighter(0.25f).withAlpha(0.45f));
                graphics.drawEllipse(hole.reduced(0.4f).translated(0.0f, -0.4f), 0.7f);
            }
            break;
        }
    }
    return tile;
}

/// Brushed metal: fine horizontal streaks, so the tile varies down its height and barely across.
juce::Image makeBrushTile()
{
    constexpr int size = 64;
    juce::Image tile(juce::Image::ARGB, size, size, true);

    juce::Random random { 0x2b8a11f3 };
    // Two widths of streak, both on cell counts that divide the tile -- see makeGrainTile.
    // Brushed metal is not one grit, and a single octave reads as banding.
    const Lattice broad { 16, random };
    const Lattice fine { 32, random };

    juce::Image::BitmapData pixels(tile, juce::Image::BitmapData::writeOnly);
    for (int y = 0; y < size; ++y)
    {
        // Sampled down a single column, so the value depends on y alone and the streak runs
        // the full width of whatever it fills.
        const auto v = static_cast<float>(y) / static_cast<float>(size);
        const auto streak = 0.5f * broad.sample(0.5f, v) + 0.5f * fine.sample(0.5f, v);
        for (int x = 0; x < size; ++x)
        {
            const auto jitter = random.nextFloat() * 0.10f - 0.05f;
            const auto value = streak * 0.75f + jitter;
            pixels.setPixelColour(x, y, (value < 0.0f ? juce::Colours::black : juce::Colours::white)
                                            .withAlpha(juce::jlimit(0.0f, 1.0f, std::abs(value) * 0.11f)));
        }
    }
    return tile;
}

/** A corner bracket: two arms and the screw holding them on.

    Drawn per corner rather than mirrored from one image because the light comes from the top
    left of the window, so the four are not the same picture rotated -- the bottom right one
    has to be the dull one or the cabinet looks flat.
*/
void drawCornerBracket(juce::Graphics& graphics, juce::Rectangle<float> area, bool right, bool bottom,
                       juce::Colour metal, float arm, float thickness)
{
    const auto x = right ? area.getRight() - arm : area.getX();
    const auto y = bottom ? area.getBottom() - arm : area.getY();

    juce::Path piece;
    piece.addRoundedRectangle(x, bottom ? area.getBottom() - thickness : area.getY(),
                              arm, thickness, 2.0f);
    piece.addRoundedRectangle(right ? area.getRight() - thickness : area.getX(), y,
                              thickness, arm, 2.0f);

    const auto lit = ! right && ! bottom;
    graphics.setGradientFill(juce::ColourGradient(metal.brighter(lit ? 0.45f : 0.18f), x, y,
                                                  metal.darker(bottom && right ? 0.55f : 0.35f),
                                                  x + arm, y + arm, false));
    graphics.fillPath(piece);
    graphics.setColour(juce::Colours::black.withAlpha(0.45f));
    graphics.strokePath(piece, juce::PathStrokeType(1.0f));

    const auto screwX = right ? area.getRight() - thickness * 0.5f : area.getX() + thickness * 0.5f;
    const auto screwY = bottom ? area.getBottom() - thickness * 0.5f : area.getY() + thickness * 0.5f;
    const auto radius = thickness * 0.17f;
    const auto screw = juce::Rectangle<float>(radius * 2.0f, radius * 2.0f).withCentre({ screwX, screwY });
    graphics.setColour(metal.darker(0.65f));
    graphics.fillEllipse(screw);
    graphics.setColour(metal.brighter(0.3f).withAlpha(0.7f));
    graphics.drawEllipse(screw.reduced(0.4f), 0.8f);
}

/// The pilot lamp, with the halo it throws onto the covering around it.
void drawPilotLamp(juce::Graphics& graphics, juce::Point<float> centre, float radius, juce::Colour lamp)
{
    const auto glow = radius * 3.6f;
    graphics.setGradientFill(juce::ColourGradient(lamp.withAlpha(0.42f), centre.x, centre.y,
                                                  lamp.withAlpha(0.0f), centre.x + glow, centre.y, true));
    graphics.fillEllipse(juce::Rectangle<float>(glow * 2.0f, glow * 2.0f).withCentre(centre));

    const auto jewel = juce::Rectangle<float>(radius * 2.0f, radius * 2.0f).withCentre(centre);
    graphics.setColour(lamp);
    graphics.fillEllipse(jewel);
    graphics.setColour(juce::Colours::white.withAlpha(0.65f));
    graphics.fillEllipse(jewel.reduced(radius * 0.55f).translated(-radius * 0.22f, -radius * 0.22f));
    graphics.setColour(juce::Colours::black.withAlpha(0.55f));
    graphics.drawEllipse(jewel.reduced(0.5f), 1.0f);
}

/** Turns a guitar rig into the bass version of itself.

    A bass cabinet is the same amplifier in a bigger, plainer box: punched metal instead of
    cloth, a darker covering, and hardware that has stopped being decorative. Applied as a
    transform rather than as its own table rows so that adding a topology stays one row.
*/
void applyBassCabinet(FaceplateStyle& style)
{
    style.badge += " Bass";
    style.tolex = style.tolex.darker(0.30f);
    style.tolexLift = style.tolexLift.darker(0.30f);
    style.weave = GrilleWeave::perforated;
    style.clothDark = style.clothDark.darker(0.25f);
    style.clothLight = style.clothLight.withSaturation(style.clothLight.getSaturation() * 0.45f);
    style.piping = style.piping.withSaturation(style.piping.getSaturation() * 0.5f).darker(0.15f);
    style.bracket = style.bracket.withSaturation(style.bracket.getSaturation() * 0.4f);
    style.cornerRadius = std::max(4.0f, style.cornerRadius - 3.0f);
}

/** The styles, in the order of the `topology` choice parameter.

    Read down them and the differences are the point. Tight Modern is cold, square and steel;
    Vintage Bloom is oxblood and brass; American Clean is blonde with an oxblood grille;
    British Crunch is black levant under a gold panel; Class-A Chime is fawn and cream;
    Sagging Rectifier is matte black with red bleeding out of it; Studio Direct is not a
    cabinet at all. Then the bass-native four, which are drawn as bass gear from the start
    rather than adapted into it. They are the voicings, drawn.

    Two constraints hold across every row, and both were learned by getting them wrong. The
    panel stays dark (see `FaceplateStyle`). And the grille stays the darkest large region:
    it is a hole in a box, so it cannot be the brightest thing in the window however light the
    real cloth is -- the wheat basketweave had to come down twice before it stopped dominating
    the page at full window width.
*/
const std::array<FaceplateStyle, nts::amp::topologyCount>& styleTable()
{
    static const std::array<FaceplateStyle, nts::amp::topologyCount> table {
        FaceplateStyle {
            "Tight Modern",
            juce::Colour { 0xff1c1a19 }, juce::Colour { 0xff2b2826 }, 0.45f,
            juce::Colour { 0xff8f9296 },
            GrilleWeave::tight,
            juce::Colour { 0xff141518 }, juce::Colour { 0xff3c414a },
            juce::Colour { 0xffb9bcc1 },
            PanelFinish::brushed,
            juce::Colour { 0xff191a1d }, juce::Colour { 0xff2f3237 },
            juce::Colour { 0xffe8ebf0 },
            juce::Colour { 0xff9fd8ff },
            6.0f,
            AmpCharacter::hiGain,
            "Four gain stages and a supply that recovers fast. Controlled and articulate at high "
            "gain; the tight end of modern."
        },
        FaceplateStyle {
            "Vintage Bloom",
            juce::Colour { 0xff3a231a }, juce::Colour { 0xff523327 }, 1.0f,
            juce::Colour { 0xffb08a52 },
            GrilleWeave::basket,
            juce::Colour { 0xff231b11 }, juce::Colour { 0xff5c4c2c },
            juce::Colour { 0xffd8b877 },
            PanelFinish::painted,
            juce::Colour { 0xff231710 }, juce::Colour { 0xff3d2818 },
            juce::Colour { 0xfff2dcae },
            juce::Colour { 0xffff9d30 },
            13.0f,
            AmpCharacter::vintage,
            "Two stages into a soft, sagging power section. Blooms on sustained notes and gets "
            "looser the harder you hit it."
        },
        FaceplateStyle {
            // Blonde tolex over an oxblood grille, polished chrome, red jewel. The lightest
            // covering of the seven, which is as far as the palette can go while the panel
            // stays dark enough to letter.
            "American Clean",
            juce::Colour { 0xff6d6152 }, juce::Colour { 0xff877a68 }, 0.6f,
            juce::Colour { 0xffc9ccd0 },
            GrilleWeave::tight,
            juce::Colour { 0xff1a1012 }, juce::Colour { 0xff5e2f35 },
            juce::Colour { 0xffd8c9a4 },
            PanelFinish::brushed,
            juce::Colour { 0xff1d1e21 }, juce::Colour { 0xff3a3d42 },
            juce::Colour { 0xfff2f4f7 },
            juce::Colour { 0xffff5545 },
            9.0f,
            AmpCharacter::clean,
            "The most headroom of the valve voicings. A stiff, heavily damped power section and "
            "a scooped mid -- it stays clean where the others give up."
        },
        FaceplateStyle {
            // Black levant, salt-and-pepper cloth, gold-anodised panel. The one everybody
            // pictures when the word crunch is used.
            "British Crunch",
            juce::Colour { 0xff191818 }, juce::Colour { 0xff272524 }, 0.85f,
            juce::Colour { 0xffc8a24e },
            GrilleWeave::basket,
            juce::Colour { 0xff17171a }, juce::Colour { 0xff54565c },
            juce::Colour { 0xffe0bd6a },
            PanelFinish::brushed,
            juce::Colour { 0xff2a2113 }, juce::Colour { 0xff4a3a1c },
            juce::Colour { 0xfff6e3ae },
            juce::Colour { 0xffffb648 },
            7.0f,
            AmpCharacter::crunch,
            "Breaks up in the power section rather than the preamp, with almost no negative "
            "feedback holding it down. Mid-forward, and it cuts."
        },
        FaceplateStyle {
            // Fawn and cream, brown cloth, chrome trim, and a lamp that barely registers --
            // the pale, chiming one. Deliberately the quietest faceplate here.
            "Class-A Chime",
            juce::Colour { 0xff5a4a37 }, juce::Colour { 0xff756148 }, 0.7f,
            juce::Colour { 0xffcfd2d6 },
            GrilleWeave::basket,
            juce::Colour { 0xff241b14 }, juce::Colour { 0xff6b5335 },
            juce::Colour { 0xffe6d9bc },
            PanelFinish::painted,
            juce::Colour { 0xff2b241a }, juce::Colour { 0xff473b2b },
            juce::Colour { 0xfff6efe0 },
            juce::Colour { 0xffffd9a0 },
            11.0f,
            AmpCharacter::vintage,
            "Cathode-biased and running no global feedback worth the name. Bright, chiming, and "
            "it starts compressing early -- the most touch-sensitive of the seven."
        },
        FaceplateStyle {
            // Matte black, coarse dark cloth, black anodised panel, and red bleeding out of
            // it. The heaviest-looking of the seven and the only one with a cold covering and
            // a hot lamp.
            "Sagging Rectifier",
            juce::Colour { 0xff151415 }, juce::Colour { 0xff211f21 }, 0.55f,
            juce::Colour { 0xff6f7175 },
            GrilleWeave::tight,
            juce::Colour { 0xff121113 }, juce::Colour { 0xff33262a },
            juce::Colour { 0xff8f2f30 },
            PanelFinish::brushed,
            juce::Colour { 0xff17161a }, juce::Colour { 0xff2c262c },
            juce::Colour { 0xffe9dfe1 },
            juce::Colour { 0xffff3b30 },
            5.0f,
            AmpCharacter::hiGain,
            "Tight Modern's four stages over a supply that is still recovering when the next "
            "chord lands. Heavy, loose, and it breathes."
        },
        FaceplateStyle {
            // Not a cabinet: a rack face. No tolex grain worth the name, a punched vent where
            // the grille would be, machined titanium, and a green LED. The DI voicing should
            // not look like something with a speaker in it.
            "Studio Direct",
            juce::Colour { 0xff232528 }, juce::Colour { 0xff32353a }, 0.15f,
            juce::Colour { 0xffa8adb4 },
            GrilleWeave::perforated,
            juce::Colour { 0xff141619 }, juce::Colour { 0xff474c54 },
            juce::Colour { 0xff7d838c },
            PanelFinish::brushed,
            juce::Colour { 0xff1b1e22 }, juce::Colour { 0xff33383f },
            juce::Colour { 0xffeef1f5 },
            juce::Colour { 0xff5ce08a },
            4.0f,
            AmpCharacter::clean,
            "An active front end rather than a valve one, with a flat three-band EQ and almost "
            "no sag. The DI voicing -- start here for bass."
        },
        FaceplateStyle {
            // The bass-native four begin here, and they are drawn to read as bass gear at a
            // glance: bigger radii, heavier brackets, and grilles coarser than anything above.
            // Black over silver-blue with a chrome bezel and a blue jewel -- the fridge.
            "Valve Flagship",
            juce::Colour { 0xff17171a }, juce::Colour { 0xff232327 }, 0.75f,
            juce::Colour { 0xffb4b8bd },
            GrilleWeave::tight,
            juce::Colour { 0xff101318 }, juce::Colour { 0xff2f3a4a },
            juce::Colour { 0xffc2c6cc },
            PanelFinish::brushed,
            juce::Colour { 0xff1a1c20 }, juce::Colour { 0xff31353c },
            juce::Colour { 0xffeef0f4 },
            juce::Colour { 0xff4aa8ff },
            8.0f,
            AmpCharacter::crunch,
            "Three valve stages that keep their bottom octave, into a supply that blooms under a "
            "chord. The reference big-amp sound."
        },
        FaceplateStyle {
            // Dark blue-grey with an aged cream weave and brass corners. The cloth is lighter
            // than any other bass face and still darker than its own tolex, which is the rule
            // the wheat basketweave broke twice before it was written down.
            "Cathode Vintage",
            juce::Colour { 0xff2f3540 }, juce::Colour { 0xff414954 }, 0.90f,
            juce::Colour { 0xffb9a97e },
            GrilleWeave::basket,
            juce::Colour { 0xff231f18 }, juce::Colour { 0xff5a5141 },
            juce::Colour { 0xffcbbb90 },
            PanelFinish::painted,
            juce::Colour { 0xff242118 }, juce::Colour { 0xff3b3527 },
            juce::Colour { 0xfff4ecd8 },
            juce::Colour { 0xffffc266 },
            14.0f,
            AmpCharacter::vintage,
            "Twenty-five watts, cathode-biased, no top end and no headroom. Compresses the moment "
            "you dig in -- the sound of a hundred records."
        },
        FaceplateStyle {
            // Milled aluminium on dark steel, punched vent, and a lamp with no colour in it.
            // The only bass face that is not a cabinet, and the squarest corners in the table.
            "Hybrid MOSFET",
            juce::Colour { 0xff26292d }, juce::Colour { 0xff353940 }, 0.20f,
            juce::Colour { 0xffb0b6bd },
            GrilleWeave::perforated,
            juce::Colour { 0xff131518 }, juce::Colour { 0xff3f454d },
            juce::Colour { 0xff868d96 },
            PanelFinish::brushed,
            juce::Colour { 0xff1c1f23 }, juce::Colour { 0xff343a41 },
            juce::Colour { 0xfff0f3f7 },
            juce::Colour { 0xffd8e4f0 },
            3.0f,
            AmpCharacter::clean,
            "Three valve stages of warmth in front of a power section that will not distort. "
            "Hi-fi, fast, and enormous underneath."
        },
        FaceplateStyle {
            // Burnt orange under a pale picture-frame edge. The brightest covering here by hue
            // and still below American Clean by luminance, so the panel rule holds without
            // dimming the whole face to protect it.
            "Short-Path Grind",
            juce::Colour { 0xff8a3d10 }, juce::Colour { 0xffa8501a }, 0.80f,
            juce::Colour { 0xffd9d2c4 },
            GrilleWeave::basket,
            juce::Colour { 0xff1a1512 }, juce::Colour { 0xff4a3a2c },
            juce::Colour { 0xffe4dccb },
            PanelFinish::painted,
            juce::Colour { 0xff241a12 }, juce::Colour { 0xff3d2c1c },
            juce::Colour { 0xfff6ece0 },
            juce::Colour { 0xffff7a2a },
            10.0f,
            AmpCharacter::hiGain,
            "The shortest signal path here. No bottom octave and no restraint -- it fuzzes out "
            "early and cuts through anything."
        },
        FaceplateStyle {
            // A rack face, not a cabinet: brushed steel, a punched vent where the grille would
            // be, silver-capped knobs and a red power lamp. The second face in the table with no
            // speaker behind it, and the only one whose lamp is a warning colour -- this is the
            // amplifier that hits a wall rather than easing into one.
            "Solid-State Bi-Amp",
            juce::Colour { 0xff2a2d31 }, juce::Colour { 0xff3a3e44 }, 0.15f,
            juce::Colour { 0xffbcc2c9 },
            GrilleWeave::perforated,
            juce::Colour { 0xff121417 }, juce::Colour { 0xff444a52 },
            juce::Colour { 0xff8b929b },
            PanelFinish::brushed,
            juce::Colour { 0xff1a1d21 }, juce::Colour { 0xff313740 },
            juce::Colour { 0xffeff2f6 },
            juce::Colour { 0xffff4136 },
            3.0f,
            AmpCharacter::crunch,
            "Two power sections either side of a high crossover. The lows stay clean while the "
            "highs hit the rails and clank."
        },
        FaceplateStyle {
            // Matte anodised aluminium with laser-etched graphics and a cold white indicator.
            // No grille cloth and no weave to speak of: the flattest, coldest face in the table,
            // which is the point -- it is the only one that is not pretending to be furniture.
            "CMOS Modern",
            juce::Colour { 0xff232529 }, juce::Colour { 0xff2e3136 }, 0.08f,
            juce::Colour { 0xff9aa0a8 },
            GrilleWeave::perforated,
            juce::Colour { 0xff0f1113 }, juce::Colour { 0xff383d44 },
            juce::Colour { 0xff6f757d },
            PanelFinish::brushed,
            juce::Colour { 0xff17191c }, juce::Colour { 0xff2b3037 },
            juce::Colour { 0xfff4f6f9 },
            juce::Colour { 0xffe8f4ff },
            2.0f,
            AmpCharacter::hiGain,
            "A high-gain silicon engine on the upper band only, over a fundamental it never "
            "touches. Tight, metallic, and unforgiving."
        }
    };
    return table;
}
} // namespace

juce::String ampCharacterName(AmpCharacter character)
{
    switch (character)
    {
        case AmpCharacter::clean:   return "Clean";
        case AmpCharacter::vintage: return "Vintage";
        case AmpCharacter::crunch:  return "Crunch";
        case AmpCharacter::hiGain:  return "Hi-gain";
    }
    return {};
}

FaceplateStyle faceplateStyle(int topologyIndex, int instrumentIndex)
{
    const auto& table = styleTable();
    const auto index = static_cast<std::size_t>(
        juce::jlimit(0, static_cast<int>(table.size()) - 1, topologyIndex));
    auto style = table[index];
    /* Only the guitar-derived voicings get the bass treatment.

       `applyBassCabinet` restates a guitar livery as bass gear -- darker covering, perforated
       grille, desaturated piping, and " Bass" on the badge. That is exactly right for a voicing
       the *engine* also merely retunes, and exactly wrong for one that was drawn as a bass
       amplifier already: it would flatten the four bass-native weaves to one, undo their colour,
       and badge a bass head "Valve Flagship Bass". The affinity the engine publishes is the same
       distinction, so the art asks it rather than keeping a second list in step.
    */
    if (instrumentIndex == 1
        && nts::amp::topologyAffinity(static_cast<nts::amp::Topology>(index))
               == nts::amp::TopologyAffinity::either)
        applyBassCabinet(style);
    return style;
}

std::size_t faceplateStyleCount() noexcept { return styleTable().size(); }

void FaceplateArt::setStyle(const FaceplateStyle& style)
{
    current = style;
    grainTile = makeGrainTile(current.grain);
    clothTile = makeClothTile(current.weave, current.clothDark, current.clothLight);
    brushTile = current.finish == PanelFinish::brushed ? makeBrushTile() : juce::Image();
}

void FaceplateArt::paint(juce::Graphics& graphics, juce::Rectangle<float> area) const
{
    if (area.getWidth() < 8.0f || area.getHeight() < 8.0f) return;

    const auto width = area.getWidth();
    const auto height = area.getHeight();
    const auto insetX = width * 0.035f;

    const auto grille = juce::Rectangle<float>(area.getX() + insetX, area.getY() + height * 0.070f,
                                               width - insetX * 2.0f, height * 0.470f);
    // Painted to enclose the knob row with a little air around it, so the deck constants stay
    // the single description of where the controls live.
    const auto panel = juce::Rectangle<float>(area.getX() + insetX,
                                              area.getY() + height * (deckTop - 0.028f),
                                              width - insetX * 2.0f,
                                              height * (deckBottom - deckTop + 0.056f));
    const auto band = juce::Rectangle<float>(grille.getX(), grille.getBottom(),
                                             grille.getWidth(), panel.getY() - grille.getBottom());

    juce::Path shell;
    shell.addRoundedRectangle(area, current.cornerRadius);
    graphics.saveState();
    graphics.reduceClipRegion(shell);

    // The covering. Lit from the top, because everything else on the faceplate is.
    graphics.setGradientFill(juce::ColourGradient(current.tolexLift, area.getX(), area.getY(),
                                                  current.tolex, area.getX(), area.getBottom(), false));
    graphics.fillRect(area);
    if (grainTile.isValid())
    {
        graphics.setTiledImageFill(grainTile, juce::roundToInt(area.getX()),
                                   juce::roundToInt(area.getY()), 1.0f);
        graphics.fillRect(area);
    }

    // The grille, sunk into the baffle.
    graphics.setColour(juce::Colours::black.withAlpha(0.55f));
    graphics.fillRoundedRectangle(grille.expanded(2.5f), 5.0f);
    {
        juce::Path clothClip;
        clothClip.addRoundedRectangle(grille, 3.0f);
        graphics.saveState();
        graphics.reduceClipRegion(clothClip);

        graphics.setColour(current.clothDark);
        graphics.fillRect(grille);
        if (clothTile.isValid())
        {
            graphics.setTiledImageFill(clothTile, juce::roundToInt(grille.getX()),
                                       juce::roundToInt(grille.getY()), 1.0f);
            graphics.fillRect(grille);
        }
        // Darker towards the edges: cloth stretched over a hollow box is not evenly lit, and
        // without this the weave reads as wallpaper.
        graphics.setGradientFill(juce::ColourGradient(juce::Colours::transparentBlack,
                                                      grille.getCentreX(), grille.getCentreY(),
                                                      juce::Colours::black.withAlpha(0.32f),
                                                      grille.getX(), grille.getY(), true));
        graphics.fillRect(grille);
        graphics.restoreState();
    }
    graphics.setColour(current.piping.withAlpha(0.85f));
    graphics.drawRoundedRectangle(grille.expanded(1.5f), 4.0f, 1.6f);

    // Badge on the left of the strip below the grille, pilot lamp on the right.
    if (band.getHeight() > 9.0f)
    {
        const auto text = band.reduced(insetX * 0.4f, 0.0f).toNearestInt();
        graphics.setColour(current.panelText.withAlpha(0.85f));
        graphics.setFont(theme::font(std::min(11.0f, band.getHeight() * 0.62f), true));
        theme::tracked(graphics, current.badge.toUpperCase(), text,
                       juce::Justification::centredLeft, 2.0f);

        const auto radius = std::min(band.getHeight() * 0.26f, 5.5f);
        drawPilotLamp(graphics, { band.getRight() - radius * 2.0f, band.getCentreY() },
                      radius, current.lamp);
    }

    // The control panel.
    {
        juce::Path panelClip;
        panelClip.addRoundedRectangle(panel, 4.0f);
        graphics.saveState();
        graphics.reduceClipRegion(panelClip);

        graphics.setGradientFill(juce::ColourGradient(current.panelHigh, panel.getX(), panel.getY(),
                                                      current.panelLow, panel.getX(), panel.getBottom(),
                                                      false));
        graphics.fillRect(panel);
        if (current.finish == PanelFinish::brushed && brushTile.isValid())
        {
            graphics.setTiledImageFill(brushTile, juce::roundToInt(panel.getX()),
                                       juce::roundToInt(panel.getY()), 1.0f);
            graphics.fillRect(panel);
        }
        else if (grainTile.isValid())
        {
            // Painted panels take the covering's grain at a fraction of its strength: the
            // paint is on the same box, so a perfectly smooth strip looks pasted on.
            graphics.setTiledImageFill(grainTile, juce::roundToInt(panel.getX()),
                                       juce::roundToInt(panel.getY()), 0.22f);
            graphics.fillRect(panel);
        }
        graphics.restoreState();
    }
    // A lit top edge and a shadowed bottom one: the whole of the panel's depth.
    graphics.setColour(juce::Colours::white.withAlpha(0.14f));
    graphics.drawLine(panel.getX() + 3.0f, panel.getY() + 0.5f, panel.getRight() - 3.0f, panel.getY() + 0.5f);
    graphics.setColour(juce::Colours::black.withAlpha(0.45f));
    graphics.drawLine(panel.getX() + 3.0f, panel.getBottom() - 0.5f,
                      panel.getRight() - 3.0f, panel.getBottom() - 0.5f);
    graphics.setColour(current.bracket.withAlpha(0.35f));
    graphics.drawRoundedRectangle(panel.reduced(0.5f), 4.0f, 1.0f);

    const auto arm = std::min(width, height) * 0.095f;
    drawCornerBracket(graphics, area, false, false, current.bracket, arm, arm * 0.34f);
    drawCornerBracket(graphics, area, true, false, current.bracket, arm, arm * 0.34f);
    drawCornerBracket(graphics, area, false, true, current.bracket, arm, arm * 0.34f);
    drawCornerBracket(graphics, area, true, true, current.bracket, arm, arm * 0.34f);

    graphics.restoreState();

    graphics.setColour(juce::Colours::black.withAlpha(0.55f));
    graphics.drawRoundedRectangle(area.reduced(0.5f), current.cornerRadius, 1.0f);
}

void FaceplateThumbnails::paint(juce::Graphics& graphics, juce::Rectangle<float> area,
                                int topologyIndex, int instrumentIndex)
{
    if (area.getWidth() < 4.0f || area.getHeight() < 4.0f) return;

    // Rendered at physical resolution rather than logical, so a 2x display gets a 2x image
    // rather than a soft one. The scale is part of the key for the same reason.
    const auto scale = static_cast<float>(graphics.getInternalContext().getPhysicalPixelScaleFactor());
    const auto width = juce::roundToInt(area.getWidth());
    const auto height = juce::roundToInt(area.getHeight());

    const auto matches = [&](const Entry& entry)
    {
        return entry.topology == topologyIndex && entry.instrument == instrumentIndex
            && entry.width == width && entry.height == height
            && std::abs(entry.scale - scale) < 0.01f;
    };

    auto found = std::find_if(entries.begin(), entries.end(), matches);
    if (found == entries.end())
    {
        const auto pixelWidth = std::max(4, juce::roundToInt(static_cast<float>(width) * scale));
        const auto pixelHeight = std::max(4, juce::roundToInt(static_cast<float>(height) * scale));

        Entry entry { topologyIndex, instrumentIndex, width, height, scale,
                      juce::Image(juce::Image::ARGB, pixelWidth, pixelHeight, true) };
        {
            juce::Graphics render(entry.image);
            FaceplateArt art;
            art.setStyle(faceplateStyle(topologyIndex, instrumentIndex));
            art.paint(render, juce::Rectangle<int>(pixelWidth, pixelHeight).toFloat());
        }
        entries.push_back(std::move(entry));
        found = std::prev(entries.end());
    }

    graphics.drawImage(found->image, area, juce::RectanglePlacement::stretchToFit);
}
} // namespace tf::ui
