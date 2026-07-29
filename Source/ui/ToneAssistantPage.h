#pragma once

#include "ModulePage.h"
#include "UiSupport.h"

#include <nts/assistant/ToneAssistant.h>

#include <array>
#include <vector>

/// Draws the assistant's suggestions: the shortlist, the plain-language reading, the evidence
/// behind it, and the exact parameter changes it would make.
class AssistantView final : public juce::Component
{
public:
    void setRecommendations(std::vector<nts::assistant::Recommendation> values,
                            int selectedIndex, juce::String status, bool previewActive);
    void paint(juce::Graphics& graphics) override;
private:
    std::vector<nts::assistant::Recommendation> recommendations;
    int selected {};
    juce::String statusText;
    bool preview {};
};

/// Evidence-based local suggestions, previewed before they are applied.
class ToneAssistantPage final : public ModulePage
{
public:
    explicit ToneAssistantPage(TubeForgeAudioProcessor& processor);

    void resized() override;
    void refresh() override;

private:
    std::array<juce::Label, 2> captions;
    juce::ComboBox goal;
    juce::ComboBox suggestion;
    juce::TextButton preview { "Preview" };
    juce::TextButton accept { "Accept" };
    juce::TextButton reject { "Reject" };
    juce::TextButton undo { "Undo" };
    juce::ToggleButton personalize { "Personalize" };
    juce::TextButton clearPreferences { "Clear" };
    AssistantView view;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ToneAssistantPage)
};
