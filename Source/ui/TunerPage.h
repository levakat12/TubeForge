#pragma once

#include "ModulePage.h"
#include "UiSupport.h"

#include <memory>

/// Live tuning display.
///
/// Reads a value the processor has already computed off the audio thread; this page only
/// draws it. The needle is deliberately the largest thing here -- when tuning, the only
/// question is "which way", and a number answers that more slowly than a position does.
class TunerPage final : public ModulePage
{
public:
    explicit TunerPage(TubeForgeAudioProcessor& processor);

    void resized() override;
    void paint(juce::Graphics& graphics) override;
    void refresh() override;

private:
    /// How far off pitch the needle can show before it pins, in cents.
    static constexpr float needleRangeCents = 50.0f;
    /// Within this, the note reads as in tune and the needle turns green.
    static constexpr float inTuneCents = 5.0f;

    [[nodiscard]] juce::Rectangle<int> needleArea() const;

    tf::ui::SectionPanel meterPanel { "Tuner" };
    juce::Label noteLabel, centsLabel, frequencyLabel, hint;
    juce::ToggleButton muteWhileTuning { "Mute the output while tuning" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> muteAttachment;

    /// Latest reading, cached so paint and refresh agree within a frame.
    int midiNote { -1 };
    float cents {};
    bool voiced {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TunerPage)
};
