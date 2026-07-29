#include "ToneAssistantPage.h"
#include "../PluginProcessor.h"
#include "../TubeForgeTheme.h"

#include <algorithm>

namespace theme = tf::theme;

void AssistantView::setRecommendations(std::vector<nts::assistant::Recommendation> values,
                                       int selectedIndex, juce::String status, bool previewActive)
{
    recommendations = std::move(values); selected = std::max(0, selectedIndex);
    statusText = std::move(status); preview = previewActive; repaint();
}

void AssistantView::paint(juce::Graphics& graphics)
{
    auto area = getLocalBounds().toFloat().reduced(0.5f);
    theme::glass(graphics, area, 9.0f);
    if (preview)
    {
        graphics.setColour(theme::accent.withAlpha(0.75f));
        graphics.drawRoundedRectangle(area.reduced(0.5f), 9.0f, 1.6f);
    }
    area = area.reduced(14.0f, 12.0f);

    graphics.setColour(preview ? theme::accent.brighter(0.2f) : theme::accent);
    graphics.setFont(theme::font(13.0f, true));
    graphics.drawFittedText(statusText, area.removeFromTop(26.0f).toNearestInt(), juce::Justification::centredLeft, 1);
    area.removeFromTop(6.0f);

    if (recommendations.empty())
    {
        graphics.setColour(theme::textTertiary);
        graphics.setFont(theme::font(13.0f));
        graphics.drawFittedText("Play for a while, analyse a clip, or pick a goal above.\n\n"
                                "Suggestions arrive with the evidence behind them and are never applied "
                                "until you preview and accept them.",
                                area.reduced(40.0f).toNearestInt(), juce::Justification::centred, 5);
        return;
    }

    selected = std::clamp(selected, 0, static_cast<int>(recommendations.size()) - 1);
    auto list = area.removeFromLeft(area.getWidth() * 0.38f);
    area.removeFromLeft(16.0f);
    const auto rowHeight = std::min(60.0f, list.getHeight() / static_cast<float>(recommendations.size()));
    for (std::size_t index = 0; index < recommendations.size(); ++index)
    {
        const auto& recommendation = recommendations[index];
        auto row = list.removeFromTop(rowHeight).reduced(0.0f, 3.0f);
        const auto active = static_cast<int>(index) == selected;
        theme::glass(graphics, row, 6.0f, active);
        if (active)
        {
            graphics.setColour(theme::accent);
            graphics.fillRoundedRectangle(row.withWidth(3.0f).reduced(0.0f, 5.0f), 1.5f);
        }
        auto confidence = row.removeFromRight(52.0f);
        graphics.setColour(theme::good);
        graphics.setFont(theme::font(10.0f, true));
        graphics.drawText(juce::String(juce::roundToInt(recommendation.confidence * 100.0f)) + "%", confidence,
                          juce::Justification::centred);
        graphics.setColour(active ? theme::textPrimary : theme::textSecondary);
        graphics.setFont(theme::font(10.5f, active));
        graphics.drawFittedText(recommendation.diagnosis, row.reduced(11.0f, 3.0f).toNearestInt(),
                                juce::Justification::centredLeft, 2);
    }

    const auto& recommendation = recommendations[static_cast<std::size_t>(selected)];
    auto detail = area;
    graphics.setColour(theme::textPrimary);
    graphics.setFont(theme::font(15.0f, true));
    graphics.drawFittedText(recommendation.beginnerExplanation,
                            detail.removeFromTop(50.0f).toNearestInt(), juce::Justification::topLeft, 3);
    graphics.setColour(theme::textSecondary);
    graphics.setFont(theme::font(10.5f));
    graphics.drawFittedText(recommendation.advancedExplanation,
                            detail.removeFromTop(62.0f).toNearestInt(), juce::Justification::topLeft, 5);
    detail.removeFromTop(6.0f);
    theme::caption(graphics, detail.removeFromTop(16.0f).toNearestInt(), "Evidence", theme::accent);
    juce::StringArray evidence;
    for (const auto& item : recommendation.evidence) evidence.add(item);
    graphics.setColour(theme::textSecondary);
    graphics.setFont(theme::font(9.5f));
    graphics.drawFittedText(evidence.isEmpty() ? "Style preference, not a measured fault"
                                               : evidence.joinIntoString("   /   "),
                            detail.removeFromTop(42.0f).toNearestInt(), juce::Justification::topLeft, 4);
    theme::caption(graphics, detail.removeFromTop(16.0f).toNearestInt(), "What it would change", theme::accent);
    juce::StringArray changes;
    for (const auto& item : recommendation.changes)
        changes.add(juce::String(item.parameter) + "   " + juce::String(item.from, 2) + "  ->  "
                    + juce::String(item.to, 2));
    graphics.setColour(theme::textPrimary);
    graphics.setFont(theme::font(9.5f));
    graphics.drawFittedText(changes.isEmpty() ? "No automatic change; inspect the signal path yourself."
                                              : changes.joinIntoString("\n"),
                            detail.removeFromTop(std::min(96.0f, detail.getHeight() * 0.50f)).toNearestInt(),
                            juce::Justification::topLeft, 7);
    graphics.setColour(theme::good);
    graphics.setFont(theme::font(10.0f, true));
    graphics.drawFittedText("EXPECTED: " + recommendation.expectedEffect,
                            detail.toNearestInt(), juce::Justification::bottomLeft, 3);
}

ToneAssistantPage::ToneAssistantPage(TubeForgeAudioProcessor& processorToUse)
    : ModulePage(processorToUse)
{
    goal.addItemList({ "Diagnose", "Tight rhythm", "Clean bass support", "Aggressive picked bass",
                       "Smooth lead", "Warm clean", "Less harsh", "Preserve low end",
                       "Match reference" }, 1);
    goal.setSelectedId(1, juce::dontSendNotification);
    suggestion.setTextWhenNoChoicesAvailable("Nothing to suggest yet");
    suggestion.setTextWhenNothingSelected("Nothing to suggest yet");
    personalize.setToggleState(processor.assistantPersonalizationEnabled(), juce::dontSendNotification);
    personalize.setTooltip("Remembers which suggestions you accept, on this machine only.");
    tf::ui::configureFieldCaption(captions[0], "Goal");
    tf::ui::configureFieldCaption(captions[1], "Suggestion");
    for (auto* component : std::initializer_list<juce::Component*> { &goal, &suggestion, &preview,
             &accept, &reject, &undo, &personalize, &clearPreferences, &captions[0], &captions[1],
             &view })
        addAndMakeVisible(*component);

    goal.onChange = [this]
    {
        constexpr std::array goals { nts::assistant::Goal::diagnose, nts::assistant::Goal::tightRhythm,
            nts::assistant::Goal::cleanBassSupport, nts::assistant::Goal::aggressivePickedBass,
            nts::assistant::Goal::smoothLead, nts::assistant::Goal::warmClean,
            nts::assistant::Goal::lessHarsh, nts::assistant::Goal::preserveLowEnd,
            nts::assistant::Goal::matchReference };
        const auto index = std::clamp(goal.getSelectedId() - 1, 0, static_cast<int>(goals.size()) - 1);
        processor.setAssistantGoal(goals[static_cast<std::size_t>(index)]);
    };
    preview.onClick = [this]
    { (void) processor.previewAssistantRecommendation(
          static_cast<std::size_t>(std::max(0, suggestion.getSelectedId() - 1))); };
    accept.onClick = [this] { (void) processor.acceptAssistantPreview(); };
    reject.onClick = [this] { (void) processor.rejectAssistantPreview(); };
    undo.onClick = [this] { (void) processor.undoAssistantChange(); };
    personalize.onClick = [this]
    { processor.setAssistantPersonalizationEnabled(personalize.getToggleState()); };
    clearPreferences.onClick = [this] { processor.clearAssistantPreferences(); };
}

void ToneAssistantPage::resized()
{
    auto area = getLocalBounds();
    auto top = area.removeFromTop(44);
    tf::ui::layOutField(top.removeFromLeft(180), captions[0], goal);
    top.removeFromLeft(12);
    tf::ui::layOutField(top.removeFromLeft(200), captions[1], suggestion);
    top.removeFromLeft(14);
    auto buttonRow = top.withTrimmedTop(13).withHeight(28);
    preview.setBounds(buttonRow.removeFromLeft(80).reduced(2, 0));
    accept.setBounds(buttonRow.removeFromLeft(76).reduced(2, 0));
    reject.setBounds(buttonRow.removeFromLeft(76).reduced(2, 0));
    undo.setBounds(buttonRow.removeFromLeft(70).reduced(2, 0));
    clearPreferences.setBounds(buttonRow.removeFromRight(70).reduced(2, 0));
    personalize.setBounds(buttonRow.removeFromRight(std::min(buttonRow.getWidth(), 130)).reduced(2, 0));
    area.removeFromTop(10);
    view.setBounds(area);
}

void ToneAssistantPage::refresh()
{
    const auto recommendations = processor.assistantRecommendations();
    if (suggestion.getNumItems() != static_cast<int>(recommendations.size()))
    {
        suggestion.clear(juce::dontSendNotification);
        for (std::size_t index = 0; index < recommendations.size(); ++index)
            suggestion.addItem(juce::String(index + 1) + ". " + recommendations[index].diagnosis,
                               static_cast<int>(index + 1));
        if (! recommendations.empty()) suggestion.setSelectedId(1, juce::dontSendNotification);
    }
    view.setRecommendations(recommendations, std::max(0, suggestion.getSelectedId() - 1),
                            processor.assistantStatusText(), processor.assistantPreviewActive());
    personalize.setToggleState(processor.assistantPersonalizationEnabled(), juce::dontSendNotification);
}
