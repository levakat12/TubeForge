/** Renders every drawn face to a PNG, so the art can be looked at without launching a DAW.

    Not a test: it asserts nothing and always succeeds. It exists because the amplifier and
    pedal faces are procedural, and the only way to know whether a change to a noise lattice or
    a palette actually improved anything is to look at the result. Three of the defects fixed
    while this artwork was written -- grain that tiled visibly, a panel that banded, a grille
    with its perforations lit instead of dark -- were invisible in code and obvious in a picture.

    Run it with an output directory, or with none to write into the working directory:

        nts_art_renderer [directory]

    One caveat, and it cost an hour the first time: the canvas must be created with
    `juce::SoftwareImageType`. A plain `juce::Image` in a console app on Windows yields a
    graphics context that silently draws nothing at all -- no error, no warning, just a
    transparent PNG.
*/

#include "ui/FaceplateArt.h"
#include "ui/PedalArt.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <iostream>

namespace
{
constexpr int faceplateWidth = 1400;
constexpr int faceplateHeight = 470;
constexpr int pedalCell = 190;
constexpr int pedalHeight = 300;

/// The page colour the art sits on, so edges and corners are judged against what surrounds them.
const juce::Colour backdrop { 0xff0b0a09 };

bool write(const juce::Image& canvas, const juce::File& file)
{
    file.deleteFile();
    auto stream = std::unique_ptr<juce::FileOutputStream>(file.createOutputStream());
    if (stream == nullptr) return false;
    return juce::PNGImageFormat().writeImageToStream(canvas, *stream);
}

int renderFaceplates(const juce::File& directory)
{
    auto written = 0;
    for (int instrument = 0; instrument < 2; ++instrument)
        for (std::size_t topology = 0; topology < tf::ui::faceplateStyleCount(); ++topology)
        {
            juce::Image canvas(juce::Image::ARGB, faceplateWidth, faceplateHeight, true,
                               juce::SoftwareImageType {});
            juce::Graphics graphics(canvas);
            graphics.fillAll(backdrop);

            tf::ui::FaceplateArt art;
            const auto style = tf::ui::faceplateStyle(static_cast<int>(topology), instrument);
            art.setStyle(style);
            art.paint(graphics, juce::Rectangle<int>(faceplateWidth, faceplateHeight)
                                    .toFloat().reduced(14.0f));

            const auto name = juce::String(static_cast<int>(topology)) + "-"
                            + style.badge.toLowerCase().replaceCharacter(' ', '-') + ".png";
            if (write(canvas, directory.getChildFile(name))) ++written;
        }
    return written;
}

int renderPedals(const juce::File& directory)
{
    // One strip holding every kind, because a pedal is judged against the others on the board
    // rather than on its own.
    const auto count = static_cast<int>(tf::ui::pedalFaceCount());
    juce::Image canvas(juce::Image::ARGB, pedalCell * count, pedalHeight, true,
                       juce::SoftwareImageType {});
    juce::Graphics graphics(canvas);
    graphics.fillAll(backdrop);

    for (int kind = 0; kind < count; ++kind)
    {
        tf::ui::PedalArt art;
        const auto& face = tf::ui::pedalFace(kind);
        art.setFace(face);
        art.paint(graphics, juce::Rectangle<int>(kind * pedalCell, 0, pedalCell, pedalHeight - 30)
                                .toFloat().reduced(10.0f));
        graphics.setColour(juce::Colours::white.withAlpha(0.8f));
        graphics.setFont(juce::Font { juce::FontOptions { 12.0f } });
        graphics.drawText(face.name,
                          juce::Rectangle<int>(kind * pedalCell, pedalHeight - 26, pedalCell, 20),
                          juce::Justification::centred, true);
    }
    return write(canvas, directory.getChildFile("pedal-faces.png")) ? 1 : 0;
}
} // namespace

int main(int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI juceInitialiser;

    const auto directory = argc > 1 ? juce::File::getCurrentWorkingDirectory().getChildFile(argv[1])
                                    : juce::File::getCurrentWorkingDirectory();
    if (! directory.createDirectory())
    {
        std::cerr << "could not create " << directory.getFullPathName() << std::endl;
        return 1;
    }

    const auto faceplates = renderFaceplates(directory);
    const auto pedals = renderPedals(directory);
    std::cout << "wrote " << faceplates << " faceplates and " << pedals
              << " pedal strip to " << directory.getFullPathName() << std::endl;
    return 0;
}
