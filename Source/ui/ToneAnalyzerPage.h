#pragma once

#include "ModulePage.h"
#include "UiSupport.h"

#include <nts/tone/ToneAnalysis.h>

#include <memory>
#include <optional>

/// Draws a finished tone analysis: descriptors with their uncertainty, the multi-resolution
/// tone profile, and the per-axis confidence the result carries.
class ToneAnalysisView final : public juce::Component
{
public:
    void setResult(std::optional<nts::tone::ToneAnalysisResult> analysis,
                   juce::String status, juce::String nearest, std::size_t profileCount);
    void paint(juce::Graphics& graphics) override;
private:
    std::optional<nts::tone::ToneAnalysisResult> result;
    juce::String statusText;
    juce::String nearestText;
    std::size_t profiles {};
};

/// Offline analysis of an isolated guitar or bass clip.
class ToneAnalyzerPage final : public ModulePage
{
public:
    explicit ToneAnalyzerPage(TubeForgeAudioProcessor& processor);

    void resized() override;
    void refresh() override;

private:
    void chooseClip();

    juce::TextButton analyzeClip { "Analyze a clip" };
    ToneAnalysisView view;
    std::unique_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ToneAnalyzerPage)
};
