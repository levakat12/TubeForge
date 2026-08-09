#include "PedalArt.h"
#include "../TubeForgeTheme.h"

// The only engine include, and only in the implementation: the interface stays in terms of
// parameter indices, while the table gets its size checked against the enum at compile time.
#include <nts/pedals/PedalBoard.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace theme = tf::theme;

namespace tf::ui
{
namespace
{
/** The mottle a hammertone or crackle finish has, as a tileable speckle.

    The lesson from `FaceplateArt` applies unchanged and is the reason the cell count divides
    the tile edge exactly: a lattice landing on fractional pixel boundaries beats against the
    pixel grid, and the interference reads as mottled stone rather than as paint.
*/
juce::Image makeSpeckleTile(PedalFinish finish)
{
    constexpr int size = 48;
    juce::Image tile(juce::Image::ARGB, size, size, true);

    // Fixed seed: the finish is artwork, not an effect. Re-rolling it per repaint would make
    // every enclosure crawl.
    juce::Random random { 0x51ce77e5 };
    // Hammertone is a coarse swirl; crackle is a fine broken shell. Both divide 48.
    const auto cells = finish == PedalFinish::crackle ? 24 : 12;
    const auto depth = finish == PedalFinish::crackle ? 0.22f : 0.16f;

    std::vector<float> lattice(static_cast<std::size_t>(cells * cells));
    for (auto& value : lattice) value = random.nextFloat() * 2.0f - 1.0f;
    const auto at = [&](int x, int y)
    {
        const auto wrappedX = ((x % cells) + cells) % cells;
        const auto wrappedY = ((y % cells) + cells) % cells;
        return lattice[static_cast<std::size_t>(wrappedY * cells + wrappedX)];
    };

    juce::Image::BitmapData pixels(tile, juce::Image::BitmapData::writeOnly);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
        {
            const auto fx = static_cast<float>(x) / static_cast<float>(size) * static_cast<float>(cells);
            const auto fy = static_cast<float>(y) / static_cast<float>(size) * static_cast<float>(cells);
            const auto x0 = static_cast<int>(std::floor(fx));
            const auto y0 = static_cast<int>(std::floor(fy));
            const auto tx = fx - static_cast<float>(x0);
            const auto ty = fy - static_cast<float>(y0);
            const auto smoothX = tx * tx * (3.0f - 2.0f * tx);
            const auto smoothY = ty * ty * (3.0f - 2.0f * ty);
            const auto top = juce::jmap(smoothX, at(x0, y0), at(x0 + 1, y0));
            const auto bottom = juce::jmap(smoothX, at(x0, y0 + 1), at(x0 + 1, y0 + 1));
            const auto value = juce::jmap(smoothY, top, bottom);
            const auto lit = value >= 0.0f;
            pixels.setPixelColour(x, y, (lit ? juce::Colours::white : juce::Colours::black)
                                            .withAlpha(juce::jlimit(0.0f, 1.0f,
                                                std::abs(value) * depth * (lit ? 0.6f : 1.0f))));
        }
    return tile;
}

/** The faces, in the order of a slot's `kind` choice parameter.

    Read down them and the differences are meant to be legible at tile size before any text is:
    an empty footprint, a bare silver box with one knob, a pale blue two-knob with a meter, the
    green three-knob everyone recognises, an orange-red slab, a big grey crackle fuzz with no
    LED, and a matte black box with a screen instead of a nameplate.
*/
const std::vector<PedalFace>& faceTable()
{
    static const std::vector<PedalFace> table {
        PedalFace {
            "EMPTY", PedalFinish::matte,
            juce::Colour { 0xff1b1a18 }, juce::Colour { 0xff222120 },
            juce::Colour { 0xff1b1a18 }, juce::Colour { 0xff67625a },
            juce::Colour { 0xff141416 }, juce::Colour { 0xff67625a },
            0, false, true,
            PedalCharacter::dynamics, true
        },
        PedalFace {
            "BOOST", PedalFinish::brushed,
            juce::Colour { 0xff8d9298 }, juce::Colour { 0xffb3b8bd },
            juce::Colour { 0xff21242a }, juce::Colour { 0xffeef1f5 },
            juce::Colour { 0xff17181b }, juce::Colour { 0xffe8ecf2 },
            1, true, false,
            PedalCharacter::dynamics
        },
        PedalFace {
            "DRIVE", PedalFinish::gloss,
            juce::Colour { 0xff3f6b32 }, juce::Colour { 0xff588c46 },
            juce::Colour { 0xffe4dcc0 }, juce::Colour { 0xff26301f },
            juce::Colour { 0xff141416 }, juce::Colour { 0xffff5545 },
            3, true, false,
            PedalCharacter::overdrive
        },
        PedalFace {
            "DIST", PedalFinish::gloss,
            juce::Colour { 0xffab4a1f }, juce::Colour { 0xffd4632c },
            juce::Colour { 0xff17120e }, juce::Colour { 0xfff6e6cf },
            juce::Colour { 0xff141416 }, juce::Colour { 0xffffd23b },
            3, true, false,
            PedalCharacter::distortion
        },
        PedalFace {
            "FUZZ", PedalFinish::crackle,
            juce::Colour { 0xff53565a }, juce::Colour { 0xff70747a },
            juce::Colour { 0xff1a1b1d }, juce::Colour { 0xffe6e8ea },
            juce::Colour { 0xff0f0f10 }, juce::Colour { 0xffff3b30 },
            2, false, false,
            PedalCharacter::fuzz, false, PedalShape::wide
        },
        PedalFace {
            "COMP", PedalFinish::hammertone,
            juce::Colour { 0xff3a5c78 }, juce::Colour { 0xff4e7897 },
            juce::Colour { 0xffdfe6ec }, juce::Colour { 0xff1d2a35 },
            juce::Colour { 0xff141416 }, juce::Colour { 0xff5ce08a },
            2, true, false,
            PedalCharacter::dynamics
        },
        PedalFace {
            "CAPTURE", PedalFinish::matte,
            juce::Colour { 0xff232427 }, juce::Colour { 0xff313337 },
            juce::Colour { 0xff0b1712 }, juce::Colour { 0xff5ce08a },
            juce::Colour { 0xff17181b }, juce::Colour { 0xff5ce08a },
            3, true, false,
            PedalCharacter::capture
        },

        /* The modelled units, in model-table order.

           Colour is doing the identifying here, not the plate text: at tile size a player
           picks a pedal out of a shelf by its enclosure long before reading anything. So the
           eleven overdrives are deliberately spread right across the spectrum rather than
           being eleven shades of green, even though the family they model mostly is. */

        // Overdrives.
        PedalFace { "SCREAM", PedalFinish::gloss,
            juce::Colour { 0xff3f6b32 }, juce::Colour { 0xff588c46 },
            juce::Colour { 0xffe4dcc0 }, juce::Colour { 0xff26301f },
            juce::Colour { 0xff141416 }, juce::Colour { 0xffff5545 }, 3, true, false,
            PedalCharacter::overdrive },
        PedalFace { "AMBER", PedalFinish::gloss,
            juce::Colour { 0xff8a6a1e }, juce::Colour { 0xffb08c2c },
            juce::Colour { 0xff231a08 }, juce::Colour { 0xfff4e6bd },
            juce::Colour { 0xff141416 }, juce::Colour { 0xffffb648 }, 3, true, false,
            PedalCharacter::overdrive },
        PedalFace { "GOLD", PedalFinish::hammertone,
            juce::Colour { 0xff9a7c3a }, juce::Colour { 0xffc0a052 },
            juce::Colour { 0xff2a1a12 }, juce::Colour { 0xfff2e2b8 },
            juce::Colour { 0xff4a1d1d }, juce::Colour { 0xffff9d30 }, 3, true, false,
            PedalCharacter::overdrive },
        PedalFace { "SUNBURST", PedalFinish::gloss,
            juce::Colour { 0xffb09420 }, juce::Colour { 0xffd8ba34 },
            juce::Colour { 0xff1c1808 }, juce::Colour { 0xfff6efc9 },
            juce::Colour { 0xff141416 }, juce::Colour { 0xffff5545 }, 3, true, false,
            PedalCharacter::overdrive },
        PedalFace { "BLUES", PedalFinish::matte,
            juce::Colour { 0xff1e1e20 }, juce::Colour { 0xff2c2c2f },
            juce::Colour { 0xff17130a }, juce::Colour { 0xffd8bb6e },
            juce::Colour { 0xff141416 }, juce::Colour { 0xffffb648 }, 3, true, false,
            PedalCharacter::overdrive },
        PedalFace { "RAIL", PedalFinish::gloss,
            juce::Colour { 0xffcdd0d4 }, juce::Colour { 0xffe8ebee },
            juce::Colour { 0xff17181b }, juce::Colour { 0xfff2f4f7 },
            juce::Colour { 0xff141416 }, juce::Colour { 0xffff3b30 }, 3, true, false,
            PedalCharacter::overdrive },
        PedalFace { "SHARP", PedalFinish::matte,
            juce::Colour { 0xff2b2f33 }, juce::Colour { 0xff3b4046 },
            juce::Colour { 0xff101214 }, juce::Colour { 0xffcfe4f2 },
            juce::Colour { 0xff141416 }, juce::Colour { 0xff9fd8ff }, 3, true, false,
            PedalCharacter::overdrive },
        PedalFace { "INDIGO", PedalFinish::gloss,
            juce::Colour { 0xff2b4a86 }, juce::Colour { 0xff3d63a8 },
            juce::Colour { 0xffe2e8f2 }, juce::Colour { 0xff16223a },
            juce::Colour { 0xff141416 }, juce::Colour { 0xff9fd8ff }, 3, true, false,
            PedalCharacter::overdrive },
        PedalFace { "MONARCH", PedalFinish::gloss,
            juce::Colour { 0xffc8b478 }, juce::Colour { 0xffe0cf98 },
            juce::Colour { 0xff211b10 }, juce::Colour { 0xfff6efdc },
            juce::Colour { 0xff141416 }, juce::Colour { 0xffffd23b }, 3, true, false,
            PedalCharacter::overdrive },
        PedalFace { "SPARKLE", PedalFinish::gloss,
            juce::Colour { 0xff6f9a72 }, juce::Colour { 0xff8fbc92 },
            juce::Colour { 0xff1a2418 }, juce::Colour { 0xffeff6ea },
            juce::Colour { 0xff141416 }, juce::Colour { 0xff5ce08a }, 3, true, false,
            PedalCharacter::overdrive },
        PedalFace { "BREW", PedalFinish::gloss,
            juce::Colour { 0xff6d2c28 }, juce::Colour { 0xff8e3c36 },
            juce::Colour { 0xfff0e2cc }, juce::Colour { 0xff2a1210 },
            juce::Colour { 0xff141416 }, juce::Colour { 0xffff9d30 }, 3, true, false,
            PedalCharacter::overdrive },

        // Distortions.
        PedalFace { "RODENT", PedalFinish::matte,
            juce::Colour { 0xff1a1a1c }, juce::Colour { 0xff262629 },
            juce::Colour { 0xff10120e }, juce::Colour { 0xffb9e6a0 },
            juce::Colour { 0xff0f0f10 }, juce::Colour { 0xffb9e6a0 }, 3, true, false,
            PedalCharacter::distortion, false, PedalShape::wedge },
        PedalFace { "RAZOR", PedalFinish::gloss,
            juce::Colour { 0xffab4a1f }, juce::Colour { 0xffd4632c },
            juce::Colour { 0xff17120e }, juce::Colour { 0xfff6e6cf },
            juce::Colour { 0xff141416 }, juce::Colour { 0xffffd23b }, 3, true, false,
            PedalCharacter::distortion, false, PedalShape::wedge },
        PedalFace { "CHAINSAW", PedalFinish::matte,
            juce::Colour { 0xff17171a }, juce::Colour { 0xff232327 },
            juce::Colour { 0xff1d0d05 }, juce::Colour { 0xffff8a3c },
            juce::Colour { 0xff0f0f10 }, juce::Colour { 0xffff5545 }, 3, true, false,
            PedalCharacter::distortion, false, PedalShape::wedge },
        PedalFace { "STACK", PedalFinish::gloss,
            juce::Colour { 0xff9c1f22 }, juce::Colour { 0xffc42d31 },
            juce::Colour { 0xff1a0c0c }, juce::Colour { 0xfff6dede },
            juce::Colour { 0xff141416 }, juce::Colour { 0xffff5545 }, 3, true, false,
            PedalCharacter::distortion },
        PedalFace { "DJENT", PedalFinish::brushed,
            juce::Colour { 0xff54327e }, juce::Colour { 0xff6f47a2 },
            juce::Colour { 0xff1a1024 }, juce::Colour { 0xffe6d8f6 },
            juce::Colour { 0xff141416 }, juce::Colour { 0xffc98cff }, 3, true, false,
            PedalCharacter::distortion },
        PedalFace { "SECTOR", PedalFinish::gloss,
            juce::Colour { 0xff33383d }, juce::Colour { 0xff474d54 },
            juce::Colour { 0xff1c0f06 }, juce::Colour { 0xffffb648 },
            juce::Colour { 0xff141416 }, juce::Colour { 0xffff9d30 }, 3, true, false,
            PedalCharacter::distortion, false, PedalShape::wedge },
        PedalFace { "ATTACK", PedalFinish::matte,
            juce::Colour { 0xff1c1c1e }, juce::Colour { 0xff2b2b2e },
            juce::Colour { 0xffe8eaec }, juce::Colour { 0xff141416 },
            juce::Colour { 0xff0f0f10 }, juce::Colour { 0xffff3b30 }, 3, true, false,
            PedalCharacter::distortion, false, PedalShape::wedge },
        PedalFace { "EXTRA", PedalFinish::matte,
            juce::Colour { 0xff191818 }, juce::Colour { 0xff262424 },
            juce::Colour { 0xff20190a }, juce::Colour { 0xffe0bd6a },
            juce::Colour { 0xff0f0f10 }, juce::Colour { 0xffffb648 }, 3, true, false,
            PedalCharacter::distortion },
        PedalFace { "VIER", PedalFinish::hammertone,
            juce::Colour { 0xff1f2a3a }, juce::Colour { 0xff2d3d55 },
            juce::Colour { 0xff0d1219 }, juce::Colour { 0xffd6e2f0 },
            juce::Colour { 0xff0f0f10 }, juce::Colour { 0xff9fd8ff }, 3, true, false,
            PedalCharacter::distortion },

        // Fuzzes.
        PedalFace { "SUSTAIN", PedalFinish::brushed,
            juce::Colour { 0xff1f1f22 }, juce::Colour { 0xff2f2f34 },
            juce::Colour { 0xff121214 }, juce::Colour { 0xffe4e6ea },
            juce::Colour { 0xff0f0f10 }, juce::Colour { 0xffff3b30 }, 3, true, false,
            PedalCharacter::fuzz, false, PedalShape::wide },
        PedalFace { "SMILE", PedalFinish::gloss,
            juce::Colour { 0xff21406b }, juce::Colour { 0xff2f588f },
            juce::Colour { 0xff0e1728 }, juce::Colour { 0xffdce6f4 },
            juce::Colour { 0xff141416 }, juce::Colour { 0xffff3b30 }, 2, false, false,
            PedalCharacter::fuzz, false, PedalShape::wide },
        PedalFace { "OCTAVE", PedalFinish::brushed,
            juce::Colour { 0xff85898e }, juce::Colour { 0xffa8adb3 },
            juce::Colour { 0xff17181b }, juce::Colour { 0xffeef1f5 },
            juce::Colour { 0xff141416 }, juce::Colour { 0xffff9d30 }, 2, true, false,
            PedalCharacter::fuzz, false, PedalShape::wide },
        PedalFace { "FOUNDRY", PedalFinish::brushed,
            juce::Colour { 0xff9aa0a6 }, juce::Colour { 0xffc0c6cc },
            juce::Colour { 0xff1c1d20 }, juce::Colour { 0xfff2f4f7 },
            juce::Colour { 0xff141416 }, juce::Colour { 0xffffd23b }, 3, true, false,
            PedalCharacter::fuzz, false, PedalShape::wide },
        PedalFace { "BRUISER", PedalFinish::crackle,
            juce::Colour { 0xff45464a }, juce::Colour { 0xff5e6065 },
            juce::Colour { 0xff17181a }, juce::Colour { 0xffe6e8ea },
            juce::Colour { 0xff0f0f10 }, juce::Colour { 0xffff9d30 }, 2, false, false,
            PedalCharacter::fuzz, false, PedalShape::wide },
        PedalFace { "RIOT", PedalFinish::matte,
            juce::Colour { 0xff151516 }, juce::Colour { 0xff212123 },
            juce::Colour { 0xff1e0a0a }, juce::Colour { 0xffff5545 },
            juce::Colour { 0xff0f0f10 }, juce::Colour { 0xffff3b30 }, 2, true, false,
            PedalCharacter::fuzz, false, PedalShape::wide },
        PedalFace { "CLOVEN", PedalFinish::crackle,
            juce::Colour { 0xff4a4256 }, juce::Colour { 0xff635a72 },
            juce::Colour { 0xff191524 }, juce::Colour { 0xffe8e0f4 },
            juce::Colour { 0xff0f0f10 }, juce::Colour { 0xffc98cff }, 3, true, false,
            PedalCharacter::fuzz, false, PedalShape::wide },
        PedalFace { "LOW", PedalFinish::gloss,
            juce::Colour { 0xff23503a }, juce::Colour { 0xff31704f },
            juce::Colour { 0xff0d1a13 }, juce::Colour { 0xffdff0e6 },
            juce::Colour { 0xff141416 }, juce::Colour { 0xff5ce08a }, 3, true, false,
            PedalCharacter::fuzz, false, PedalShape::wide },

        // Boost and dynamics.
        PedalFace { "LIFT", PedalFinish::brushed,
            juce::Colour { 0xff9aa0a6 }, juce::Colour { 0xffc2c8ce },
            juce::Colour { 0xff21242a }, juce::Colour { 0xffeef1f5 },
            juce::Colour { 0xff17181b }, juce::Colour { 0xffe8ecf2 }, 3, true, false,
            PedalCharacter::dynamics },
        PedalFace { "SQUEEZE", PedalFinish::hammertone,
            juce::Colour { 0xff3a5c78 }, juce::Colour { 0xff4e7897 },
            juce::Colour { 0xffdfe6ec }, juce::Colour { 0xff1d2a35 },
            juce::Colour { 0xff141416 }, juce::Colour { 0xff5ce08a }, 3, true, false,
            PedalCharacter::dynamics },

        // Modulation. Cool colours across the family, so a shelf of them reads as one kind of
        // thing next to the warm drives above.
        PedalFace { "ENSEMBLE", PedalFinish::gloss,
            juce::Colour { 0xff2f6f8f }, juce::Colour { 0xff3f8fb3 },
            juce::Colour { 0xffe2eef4 }, juce::Colour { 0xff14303e },
            juce::Colour { 0xff141416 }, juce::Colour { 0xff9fd8ff }, 2, true, false,
            PedalCharacter::modulation },
        PedalFace { "VIBRATO", PedalFinish::gloss,
            juce::Colour { 0xff3f7f6a }, juce::Colour { 0xff56a189 },
            juce::Colour { 0xffe4f2ec }, juce::Colour { 0xff163329 },
            juce::Colour { 0xff141416 }, juce::Colour { 0xff5ce08a }, 2, true, false,
            PedalCharacter::modulation },
        PedalFace { "JET", PedalFinish::brushed,
            juce::Colour { 0xff5c6470 }, juce::Colour { 0xff78828f },
            juce::Colour { 0xff14181f }, juce::Colour { 0xffe6ecf4 },
            juce::Colour { 0xff141416 }, juce::Colour { 0xff9fd8ff }, 3, true, false,
            PedalCharacter::modulation },
        PedalFace { "SWEEP", PedalFinish::hammertone,
            juce::Colour { 0xff45507a }, juce::Colour { 0xff5c6a9c },
            juce::Colour { 0xff141830 }, juce::Colour { 0xffdfe4f6 },
            juce::Colour { 0xff141416 }, juce::Colour { 0xffc98cff }, 3, true, false,
            PedalCharacter::modulation },
        PedalFace { "PHASE", PedalFinish::gloss,
            juce::Colour { 0xffc4641f }, juce::Colour { 0xffe6842f },
            juce::Colour { 0xff1c1008 }, juce::Colour { 0xfff6e4cf },
            juce::Colour { 0xff141416 }, juce::Colour { 0xffffd23b }, 1, true, false,
            PedalCharacter::modulation },

        // Bit and sample-rate reduction. Cold greys and greens -- these are the digital ones.
        PedalFace { "MAPPER", PedalFinish::matte,
            juce::Colour { 0xff26292e }, juce::Colour { 0xff363b42 },
            juce::Colour { 0xff0c1a12 }, juce::Colour { 0xff5ce08a },
            juce::Colour { 0xff141416 }, juce::Colour { 0xff5ce08a }, 3, true, false,
            PedalCharacter::crush, false, PedalShape::rack },
        PedalFace { "OCTO", PedalFinish::gloss,
            juce::Colour { 0xff33506b }, juce::Colour { 0xff456d90 },
            juce::Colour { 0xff0d1620 }, juce::Colour { 0xffd8e8f4 },
            juce::Colour { 0xff141416 }, juce::Colour { 0xff9fd8ff }, 3, true, false,
            PedalCharacter::crush, false, PedalShape::rack },
        PedalFace { "REDUCE", PedalFinish::brushed,
            juce::Colour { 0xff7d838c }, juce::Colour { 0xff9ba3ad },
            juce::Colour { 0xff17181b }, juce::Colour { 0xffeef1f5 },
            juce::Colour { 0xff141416 }, juce::Colour { 0xffffd23b }, 3, true, false,
            PedalCharacter::crush, false, PedalShape::rack },
        PedalFace { "RAD", PedalFinish::matte,
            juce::Colour { 0xff2c3a24 }, juce::Colour { 0xff3e5033 },
            juce::Colour { 0xff101a0c }, juce::Colour { 0xffb9e6a0 },
            juce::Colour { 0xff141416 }, juce::Colour { 0xffb9e6a0 }, 3, true, false,
            PedalCharacter::crush, false, PedalShape::rack },
        PedalFace { "SERGEANT", PedalFinish::hammertone,
            juce::Colour { 0xff4a4a3c }, juce::Colour { 0xff626250 },
            juce::Colour { 0xff17170f }, juce::Colour { 0xffe8e8d4 },
            juce::Colour { 0xff141416 }, juce::Colour { 0xffffb648 }, 3, true, false,
            PedalCharacter::crush, false, PedalShape::rack },

        // Delay and reverb. Deep, saturated colours -- these are the ones that live after the
        // amplifier in a player's head, even though this board is in front of it.
        PedalFace { "CARBON", PedalFinish::gloss,
            juce::Colour { 0xff2a3f5e }, juce::Colour { 0xff3a577f },
            juce::Colour { 0xffe0e8f2 }, juce::Colour { 0xff121b2a },
            juce::Colour { 0xff141416 }, juce::Colour { 0xff9fd8ff }, 3, true, false,
            PedalCharacter::timeBased },
        PedalFace { "REPEAT", PedalFinish::matte,
            juce::Colour { 0xff25282c }, juce::Colour { 0xff343940 },
            juce::Colour { 0xff0d1218 }, juce::Colour { 0xffdfe8f2 },
            juce::Colour { 0xff141416 }, juce::Colour { 0xff5ce08a }, 3, true, false,
            PedalCharacter::timeBased },
        PedalFace { "SPRING", PedalFinish::hammertone,
            juce::Colour { 0xff3d5a3a }, juce::Colour { 0xff527a4e },
            juce::Colour { 0xffe4efdf }, juce::Colour { 0xff16240f },
            juce::Colour { 0xff141416 }, juce::Colour { 0xffffb648 }, 2, true, false,
            PedalCharacter::timeBased },
        PedalFace { "BLOOM", PedalFinish::gloss,
            juce::Colour { 0xff6a4a86 }, juce::Colour { 0xff8a64ab },
            juce::Colour { 0xfff0e8f8 }, juce::Colour { 0xff21122e },
            juce::Colour { 0xff141416 }, juce::Colour { 0xffc98cff }, 3, true, false,
            PedalCharacter::timeBased },
        PedalFace { "CATHEDRAL", PedalFinish::matte,
            juce::Colour { 0xff1d2733 }, juce::Colour { 0xff2b3846 },
            juce::Colour { 0xff0b1018 }, juce::Colour { 0xffcfe0f2 },
            juce::Colour { 0xff141416 }, juce::Colour { 0xff9fd8ff }, 3, true, false,
            PedalCharacter::timeBased },
        PedalFace { "PLATE", PedalFinish::brushed,
            juce::Colour { 0xff8b9198 }, juce::Colour { 0xffaeb4bb },
            juce::Colour { 0xff191b1f }, juce::Colour { 0xfff0f3f6 },
            juce::Colour { 0xff141416 }, juce::Colour { 0xffe8ecf2 }, 3, true, false,
            PedalCharacter::timeBased },

        // Bass DI and preamps. Heavier, darker enclosures -- these are rack-adjacent boxes
        // rather than stompboxes, and they should read that way on the shelf.
        PedalFace { "DEEP SIX", PedalFinish::matte,
            juce::Colour { 0xff1c1e22 }, juce::Colour { 0xff2a2d33 },
            juce::Colour { 0xff0e1520 }, juce::Colour { 0xff6fb8ff },
            juce::Colour { 0xff141416 }, juce::Colour { 0xff6fb8ff }, 3, true, false,
            PedalCharacter::bass, false, PedalShape::rack },
        PedalFace { "SAND", PedalFinish::brushed,
            juce::Colour { 0xff6e6250 }, juce::Colour { 0xff8d7f68 },
            juce::Colour { 0xff191510 }, juce::Colour { 0xfff2ebdc },
            juce::Colour { 0xff141416 }, juce::Colour { 0xffffb648 }, 3, true, false,
            PedalCharacter::bass, false, PedalShape::rack },
        PedalFace { "HAMMER", PedalFinish::hammertone,
            juce::Colour { 0xff37474f }, juce::Colour { 0xff4a616b },
            juce::Colour { 0xffe2ecf0 }, juce::Colour { 0xff141e23 },
            juce::Colour { 0xff141416 }, juce::Colour { 0xff5ce08a }, 3, true, false,
            PedalCharacter::bass, false, PedalShape::rack },
        PedalFace { "OMEGA", PedalFinish::matte,
            juce::Colour { 0xff23201c }, juce::Colour { 0xff322e28 },
            juce::Colour { 0xff1a1408 }, juce::Colour { 0xffe0bd6a },
            juce::Colour { 0xff141416 }, juce::Colour { 0xffffb648 }, 3, true, false,
            PedalCharacter::bass, false, PedalShape::rack },
        PedalFace { "SCRAMBLE", PedalFinish::gloss,
            juce::Colour { 0xff4a2b2b }, juce::Colour { 0xff653b3b },
            juce::Colour { 0xfff0e2e2 }, juce::Colour { 0xff1e1010 },
            juce::Colour { 0xff141416 }, juce::Colour { 0xffff5545 }, 3, true, false,
            PedalCharacter::bass, false, PedalShape::rack },
        PedalFace { "VALVE", PedalFinish::brushed,
            juce::Colour { 0xff59504a }, juce::Colour { 0xff736860 },
            juce::Colour { 0xff17130f }, juce::Colour { 0xfff2ece4 },
            juce::Colour { 0xff141416 }, juce::Colour { 0xffff9d30 }, 3, true, false,
            PedalCharacter::bass, false, PedalShape::rack },

        // Pitch. Saturated, slightly unreal colours -- these are the ones that do something no
        // amplifier does, and the shelf should look like it.
        PedalFace { "POLY", PedalFinish::gloss,
            juce::Colour { 0xff8a2f6b }, juce::Colour { 0xffae4189 },
            juce::Colour { 0xfff4e2ee }, juce::Colour { 0xff2a0e21 },
            juce::Colour { 0xff141416 }, juce::Colour { 0xffff5ac8 }, 3, true, false,
            PedalCharacter::pitch },
        PedalFace { "DIVE", PedalFinish::gloss,
            juce::Colour { 0xffb3202a }, juce::Colour { 0xffd93340 },
            juce::Colour { 0xff1c0a0c }, juce::Colour { 0xfff6dede },
            juce::Colour { 0xff141416 }, juce::Colour { 0xffffd23b }, 2, true, false,
            PedalCharacter::pitch },
        PedalFace { "WARP", PedalFinish::hammertone,
            juce::Colour { 0xff34638a }, juce::Colour { 0xff4785b5 },
            juce::Colour { 0xff0d1a26 }, juce::Colour { 0xffd8ecf8 },
            juce::Colour { 0xff141416 }, juce::Colour { 0xff9fd8ff }, 3, true, false,
            PedalCharacter::pitch }
    };
    // A vector rather than a fixed array: the model table grows and this must grow with it.
    // The pairing is asserted at run time in `nts_wrapper_tests` -- every entry the choice
    // parameter offers must have a face -- because neither count is a compile-time constant.
    return table;
}
} // namespace

juce::String pedalCharacterName(PedalCharacter character)
{
    switch (character)
    {
        case PedalCharacter::dynamics:   return "Clean & Dynamics";
        case PedalCharacter::overdrive:  return "Overdrive";
        case PedalCharacter::distortion: return "Distortion";
        case PedalCharacter::fuzz:       return "Fuzz";
        case PedalCharacter::modulation: return "Modulation";
        case PedalCharacter::crush:      return "Bit & Sample";
        case PedalCharacter::timeBased:  return "Delay & Reverb";
        case PedalCharacter::pitch:      return "Pitch";
        case PedalCharacter::bass:       return "Bass DI";
        case PedalCharacter::capture:    return "Captures";
    }
    return {};
}

const PedalFace& pedalFace(int kindIndex)
{
    const auto& table = faceTable();
    const auto index = static_cast<std::size_t>(
        juce::jlimit(0, static_cast<int>(table.size()) - 1, kindIndex));
    return table[index];
}

std::size_t pedalFaceCount() noexcept { return faceTable().size(); }

void PedalArt::setFace(const PedalFace& face)
{
    current = face;
    speckleTile = (current.finish == PedalFinish::hammertone || current.finish == PedalFinish::crackle)
        ? makeSpeckleTile(current.finish) : juce::Image();
}

void PedalArt::paint(juce::Graphics& graphics, juce::Rectangle<float> area) const
{
    if (area.getWidth() < 8.0f || area.getHeight() < 8.0f) return;

    /* A stompbox seen from above is roughly 2:3, and the families that are not are the point of
       having shapes at all: a big-box fuzz really is squarer, and a DI box really is wider than
       it is tall. Fitting the right proportion inside whatever rectangle the caller gives keeps
       a pedal looking like its own kind of pedal in a tile, on a chip, and on the card. */
    const auto aspect = [this]
    {
        switch (current.shape)
        {
            case PedalShape::wide:  return 0.82f;
            case PedalShape::rack:  return 1.15f;
            case PedalShape::wedge: return 0.72f;
            case PedalShape::compact: break;
        }
        return 0.66f;
    }();

    auto body = area.getWidth() / area.getHeight() > aspect
        ? area.withWidth(area.getHeight() * aspect).withCentre(area.getCentre())
        : area.withHeight(area.getWidth() / aspect).withCentre(area.getCentre());
    body = body.reduced(body.getWidth() * 0.04f);

    // A rack box is a folded steel case with visible corners; a stompbox is a rounded casting.
    const auto radius = body.getWidth() * (current.shape == PedalShape::rack ? 0.035f : 0.09f);

    if (current.outline)
    {
        // Not a pedal: the footprint one would sit in. Dashed, so a board of empty slots reads
        // as gaps rather than as a row of identical black boxes.
        const float dashes[] { 5.0f, 4.0f };
        juce::Path shape;
        shape.addRoundedRectangle(body, radius);
        juce::Path dashed;
        juce::PathStrokeType(1.2f).createDashedStroke(dashed, shape, dashes, 2);
        graphics.setColour(current.lettering.withAlpha(0.55f));
        graphics.fillPath(dashed);
        graphics.setFont(theme::font(std::min(11.0f, body.getWidth() * 0.14f), true));
        theme::tracked(graphics, current.plateText, body.toNearestInt(), juce::Justification::centred, 1.5f);
        return;
    }

    graphics.setColour(juce::Colours::black.withAlpha(0.45f));
    graphics.fillRoundedRectangle(body.translated(0.0f, body.getHeight() * 0.012f), radius);

    graphics.setGradientFill(juce::ColourGradient(current.bodyLift, body.getCentreX(), body.getY(),
                                                  current.body, body.getCentreX(), body.getBottom(),
                                                  false));
    graphics.fillRoundedRectangle(body, radius);
    if (speckleTile.isValid())
    {
        juce::Path clip;
        clip.addRoundedRectangle(body, radius);
        graphics.saveState();
        graphics.reduceClipRegion(clip);
        graphics.setTiledImageFill(speckleTile, juce::roundToInt(body.getX()),
                                   juce::roundToInt(body.getY()), 1.0f);
        graphics.fillRect(body);
        graphics.restoreState();
    }
    if (current.finish == PedalFinish::brushed)
    {
        // Horizontal tooling marks, cheap enough to draw straight rather than tile.
        graphics.setColour(juce::Colours::white.withAlpha(0.05f));
        for (auto y = body.getY() + 2.0f; y < body.getBottom(); y += 3.0f)
            graphics.fillRect(body.getX(), y, body.getWidth(), 1.0f);
    }
    if (current.shape == PedalShape::wedge)
    {
        // The sloped face: a lit band across the upper third, and a shadow line where the two
        // planes meet. Two fills rather than a real bevel, which at tile size is all that reads.
        const auto fold = body.getY() + body.getHeight() * 0.30f;
        graphics.setColour(juce::Colours::white.withAlpha(0.07f));
        graphics.fillRect(body.withBottom(fold).reduced(1.5f, 1.5f));
        graphics.setColour(juce::Colours::black.withAlpha(0.35f));
        graphics.fillRect(body.getX() + 2.0f, fold, body.getWidth() - 4.0f, 1.0f);
    }
    if (current.shape == PedalShape::rack)
    {
        // Corner screws. A DI box is bolted together where a stompbox is cast in one piece.
        const auto inset = body.getWidth() * 0.045f;
        const auto screw = std::max(1.2f, body.getWidth() * 0.018f);
        for (const auto x : { body.getX() + inset, body.getRight() - inset })
            for (const auto y : { body.getY() + inset, body.getBottom() - inset })
            {
                graphics.setColour(juce::Colours::black.withAlpha(0.5f));
                graphics.fillEllipse(x - screw, y - screw, screw * 2.0f, screw * 2.0f);
                graphics.setColour(juce::Colours::white.withAlpha(0.22f));
                graphics.drawEllipse(x - screw, y - screw, screw * 2.0f, screw * 2.0f, 0.8f);
            }
    }

    graphics.setColour(juce::Colours::black.withAlpha(0.55f));
    graphics.drawRoundedRectangle(body.reduced(0.5f), radius, 1.0f);
    graphics.setColour(juce::Colours::white.withAlpha(0.12f));
    graphics.drawRoundedRectangle(body.reduced(1.6f), radius * 0.85f, 1.0f);

    // A rack box has more of its width to spare and less of its height, so its furniture is
    // inset differently -- otherwise the knobs crowd the edges and the plate runs off the sides.
    const auto insetX = current.shape == PedalShape::rack ? 0.07f : 0.10f;
    const auto insetY = current.shape == PedalShape::rack ? 0.10f : 0.07f;
    auto inner = body.reduced(body.getWidth() * insetX, body.getHeight() * insetY);

    // Knobs across the top third.
    auto knobRow = inner.removeFromTop(inner.getHeight() * 0.26f);
    if (current.knobs > 0)
    {
        const auto diameter = std::min(knobRow.getHeight(),
                                       knobRow.getWidth() / (static_cast<float>(current.knobs) + 0.4f));
        const auto spacing = knobRow.getWidth() / static_cast<float>(current.knobs);
        for (int knob = 0; knob < current.knobs; ++knob)
        {
            const auto centre = juce::Point<float>(
                knobRow.getX() + spacing * (static_cast<float>(knob) + 0.5f), knobRow.getCentreY());
            const auto cap = juce::Rectangle<float>(diameter, diameter).withCentre(centre);
            graphics.setColour(juce::Colours::black.withAlpha(0.4f));
            graphics.fillEllipse(cap.translated(0.0f, diameter * 0.07f));
            graphics.setGradientFill(juce::ColourGradient(
                current.knobCap.brighter(0.5f), cap.getX(), cap.getY(),
                current.knobCap.darker(0.3f), cap.getRight(), cap.getBottom(), false));
            graphics.fillEllipse(cap);
            graphics.setColour(current.lettering.withAlpha(0.85f));
            graphics.drawLine(centre.x, cap.getY() + diameter * 0.12f,
                              centre.x, centre.y, std::max(1.0f, diameter * 0.09f));
        }
    }

    inner.removeFromTop(inner.getHeight() * 0.06f);

    // The nameplate, which is where a pedal actually says what it is.
    auto plate = inner.removeFromTop(inner.getHeight() * 0.34f);
    graphics.setColour(current.plate);
    graphics.fillRoundedRectangle(plate, plate.getHeight() * 0.16f);
    graphics.setColour(juce::Colours::black.withAlpha(0.4f));
    graphics.drawRoundedRectangle(plate.reduced(0.5f), plate.getHeight() * 0.16f, 1.0f);
    graphics.setColour(current.lettering);
    graphics.setFont(theme::font(std::max(6.0f, std::min(11.0f, plate.getHeight() * 0.44f)), true));
    graphics.drawFittedText(current.plateText, plate.toNearestInt().reduced(3, 1),
                            juce::Justification::centred, 2);

    // The LED sits between the plate and the switch, as it does on the real thing.
    auto lower = inner;
    if (current.hasLed)
    {
        const auto radiusLed = std::max(1.5f, body.getWidth() * 0.035f);
        const auto centre = juce::Point<float>(body.getCentreX(),
                                               lower.getY() + lower.getHeight() * 0.16f);
        graphics.setGradientFill(juce::ColourGradient(
            current.led.withAlpha(0.5f), centre.x, centre.y,
            current.led.withAlpha(0.0f), centre.x + radiusLed * 3.4f, centre.y, true));
        graphics.fillEllipse(juce::Rectangle<float>(radiusLed * 6.8f, radiusLed * 6.8f).withCentre(centre));
        graphics.setColour(current.led);
        graphics.fillEllipse(juce::Rectangle<float>(radiusLed * 2.0f, radiusLed * 2.0f).withCentre(centre));
    }

    // The footswitch.
    const auto switchRadius = std::min(lower.getWidth(), lower.getHeight()) * 0.24f;
    const auto switchCentre = juce::Point<float>(body.getCentreX(),
                                                 lower.getBottom() - switchRadius * 1.25f);
    const auto ring = juce::Rectangle<float>(switchRadius * 2.4f, switchRadius * 2.4f)
                          .withCentre(switchCentre);
    graphics.setColour(juce::Colours::black.withAlpha(0.35f));
    graphics.fillEllipse(ring);
    graphics.setGradientFill(juce::ColourGradient(
        juce::Colour { 0xffb9bdc2 }, ring.getX(), ring.getY(),
        juce::Colour { 0xff4a4d51 }, ring.getRight(), ring.getBottom(), false));
    graphics.fillEllipse(ring.reduced(switchRadius * 0.35f));
    graphics.setColour(juce::Colours::black.withAlpha(0.5f));
    graphics.drawEllipse(ring.reduced(switchRadius * 0.35f), 1.0f);
}

void PedalThumbnails::paint(juce::Graphics& graphics, juce::Rectangle<float> area, int kindIndex)
{
    if (area.getWidth() < 4.0f || area.getHeight() < 4.0f) return;

    const auto scale = static_cast<float>(graphics.getInternalContext().getPhysicalPixelScaleFactor());
    const auto width = juce::roundToInt(area.getWidth());
    const auto height = juce::roundToInt(area.getHeight());

    const auto matches = [&](const Entry& entry)
    {
        return entry.kind == kindIndex && entry.width == width && entry.height == height
            && std::abs(entry.scale - scale) < 0.01f;
    };

    auto found = std::find_if(entries.begin(), entries.end(), matches);
    if (found == entries.end())
    {
        const auto pixelWidth = std::max(4, juce::roundToInt(static_cast<float>(width) * scale));
        const auto pixelHeight = std::max(4, juce::roundToInt(static_cast<float>(height) * scale));

        Entry entry { kindIndex, width, height, scale,
                      juce::Image(juce::Image::ARGB, pixelWidth, pixelHeight, true) };
        {
            juce::Graphics render(entry.image);
            PedalArt art;
            art.setFace(pedalFace(kindIndex));
            art.paint(render, juce::Rectangle<int>(pixelWidth, pixelHeight).toFloat());
        }
        entries.push_back(std::move(entry));
        found = std::prev(entries.end());
    }

    graphics.drawImage(found->image, area, juce::RectanglePlacement::stretchToFit);
}
} // namespace tf::ui
