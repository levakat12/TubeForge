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
const std::array<PedalFace, nts::pedals::kindCount>& faceTable()
{
    static const std::array<PedalFace, nts::pedals::kindCount> table { {
        PedalFace {
            "Empty", "No pedal in this slot. Not bypassed -- absent, and it costs nothing.",
            PedalFinish::matte,
            juce::Colour { 0xff1b1a18 }, juce::Colour { 0xff222120 },
            juce::Colour { 0xff1b1a18 }, juce::Colour { 0xff67625a },
            juce::Colour { 0xff141416 }, juce::Colour { 0xff67625a },
            0, false, true,
            PedalCharacter::dynamics, true
        },
        PedalFace {
            "Boost", "A clean lift. Drive is how hard the amplifier's own front end gets pushed, "
                     "not how dirty this is.",
            PedalFinish::brushed,
            juce::Colour { 0xff8d9298 }, juce::Colour { 0xffb3b8bd },
            juce::Colour { 0xff21242a }, juce::Colour { 0xffeef1f5 },
            juce::Colour { 0xff17181b }, juce::Colour { 0xffe8ecf2 },
            1, true, false,
            PedalCharacter::dynamics
        },
        PedalFace {
            "Overdrive", "Clips only what is above 720 Hz and sums it back with the untouched "
                         "signal. That mid hump is why it stays articulate.",
            PedalFinish::gloss,
            juce::Colour { 0xff3f6b32 }, juce::Colour { 0xff588c46 },
            juce::Colour { 0xffe4dcc0 }, juce::Colour { 0xff26301f },
            juce::Colour { 0xff141416 }, juce::Colour { 0xffff5545 },
            3, true, false,
            PedalCharacter::overdrive
        },
        PedalFace {
            "Distortion", "Everything through a soft clipper at high gain. Tone is the only "
                          "thing standing between it and a fizz.",
            PedalFinish::gloss,
            juce::Colour { 0xffab4a1f }, juce::Colour { 0xffd4632c },
            juce::Colour { 0xff17120e }, juce::Colour { 0xfff6e6cf },
            juce::Colour { 0xff141416 }, juce::Colour { 0xffffd23b },
            3, true, false,
            PedalCharacter::distortion
        },
        PedalFace {
            "Fuzz", "Highest gain, asymmetric, and high-passed hard at the input so low notes "
                    "stay notes.",
            PedalFinish::crackle,
            juce::Colour { 0xff53565a }, juce::Colour { 0xff70747a },
            juce::Colour { 0xff1a1b1d }, juce::Colour { 0xffe6e8ea },
            juce::Colour { 0xff0f0f10 }, juce::Colour { 0xffff3b30 },
            2, false, false,
            PedalCharacter::fuzz
        },
        PedalFace {
            "Compressor", "One knob: Drive moves threshold and ratio together. Make-up gain "
                          "follows automatically.",
            PedalFinish::hammertone,
            juce::Colour { 0xff3a5c78 }, juce::Colour { 0xff4e7897 },
            juce::Colour { 0xffdfe6ec }, juce::Colour { 0xff1d2a35 },
            juce::Colour { 0xff141416 }, juce::Colour { 0xff5ce08a },
            2, true, false,
            PedalCharacter::dynamics
        },
        PedalFace {
            "Neural capture", "Plays a converted .nam pedal capture. Drive sets the level going "
                              "into the model, which a capture is most sensitive to.",
            PedalFinish::matte,
            juce::Colour { 0xff232427 }, juce::Colour { 0xff313337 },
            juce::Colour { 0xff0b1712 }, juce::Colour { 0xff5ce08a },
            juce::Colour { 0xff17181b }, juce::Colour { 0xff5ce08a },
            3, true, false,
            PedalCharacter::capture
        }
    } };
    static_assert(table.size() == nts::pedals::kindCount,
                  "every pedal kind needs a face");
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

    // A stompbox seen from above is roughly 2:3. Fitting that inside whatever rectangle the
    // caller gives keeps a pedal looking like a pedal in a tile, on a chip, and on the card.
    const auto aspect = 0.66f;
    auto body = area.getWidth() / area.getHeight() > aspect
        ? area.withWidth(area.getHeight() * aspect).withCentre(area.getCentre())
        : area.withHeight(area.getWidth() / aspect).withCentre(area.getCentre());
    body = body.reduced(body.getWidth() * 0.04f);

    const auto radius = body.getWidth() * 0.09f;

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
        theme::tracked(graphics, "EMPTY", body.toNearestInt(), juce::Justification::centred, 1.5f);
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
    graphics.setColour(juce::Colours::black.withAlpha(0.55f));
    graphics.drawRoundedRectangle(body.reduced(0.5f), radius, 1.0f);
    graphics.setColour(juce::Colours::white.withAlpha(0.12f));
    graphics.drawRoundedRectangle(body.reduced(1.6f), radius * 0.85f, 1.0f);

    auto inner = body.reduced(body.getWidth() * 0.10f, body.getHeight() * 0.07f);

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
    graphics.drawFittedText(current.name, plate.toNearestInt().reduced(3, 1),
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
