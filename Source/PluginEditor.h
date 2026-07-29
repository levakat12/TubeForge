#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include <nts/assistant/ToneAssistant.h>
#include <nts/circuit/CircuitTypes.h>
#include <nts/ecosystem/TonePackage.h>
#include <nts/reconstruction/SourceReconstruction.h>
#include <nts/tone/ToneAnalysis.h>

#include <memory>
#include <optional>
#include <array>
#include <vector>

class TubeForgeAudioProcessor;

class CircuitSchematicView final : public juce::Component
{
public:
    void setSnapshot(std::vector<nts::circuit::NodeTelemetry> telemetry, juce::String status,
                     nts::circuit::CircuitGraphDescription graph);
    void paint(juce::Graphics& graphics) override;
private:
    std::vector<nts::circuit::NodeTelemetry> stages;
    juce::String statusText;
    nts::circuit::CircuitGraphDescription circuit;
};

class StudioSignalChain final : public juce::Component
{
public:
    void setEngineMode(int mode);
    void paint(juce::Graphics& graphics) override;
private:
    int activeEngineMode {};
};

class StudioLevelMeter final : public juce::Component
{
public:
    void setLevels(float input, float output);
    void paint(juce::Graphics& graphics) override;
private:
    float inputLevel {};
    float outputLevel {};
};

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

class ReconstructionView final : public juce::Component
{
public:
    void setResult(std::optional<nts::reconstruction::ReconstructionResult> value,
                   juce::String status, float progress);
    void paint(juce::Graphics& graphics) override;
private:
    std::optional<nts::reconstruction::ReconstructionResult> result;
    juce::String statusText;
    float progressValue {};
};

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

class AmpLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    void drawRotarySlider(juce::Graphics& graphics, int x, int y, int width, int height,
                          float sliderPosition, float rotaryStartAngle, float rotaryEndAngle,
                          juce::Slider& slider) override;
    void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour&,
                              bool isMouseOver, bool isButtonDown) override;
    void drawToggleButton(juce::Graphics&, juce::ToggleButton&, bool isMouseOver,
                          bool isButtonDown) override;
    void drawComboBox(juce::Graphics&, int width, int height, bool isButtonDown,
                      int buttonX, int buttonY, int buttonWidth, int buttonHeight,
                      juce::ComboBox&) override;
};

class TubeForgeAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                            private juce::Timer
{
public:
    explicit TubeForgeAudioProcessorEditor(TubeForgeAudioProcessor& processor);
    ~TubeForgeAudioProcessorEditor() override;

    void paint(juce::Graphics& graphics) override;
    void resized() override;

private:
    void timerCallback() override;
    void chooseProjectToSave();
    void chooseProjectToOpen();
    void chooseNeuralModel();
    void chooseToneClip();
    void chooseSongForReconstruction();
    void chooseReconstructionExport();
    void chooseTonePackageImport();
    void chooseTonePackageExport();
    void refreshPackageBrowser();
    static void configureSlider(juce::Slider& slider);
    static void configureKnob(juce::Slider& slider);
    static void configureHeading(juce::Label& label, const juce::String& text);

    TubeForgeAudioProcessor& processor;
    AmpLookAndFeel ampLookAndFeel;
    juce::Label title;
    juce::Label productTagline;
    juce::Label presetCaption;
    juce::Label presetName;
    juce::Label mode;
    juce::Label deviceStatus;
    juce::Label diagnosticsText;
    juce::Label inputLabel;
    juce::Label outputLabel;
    juce::Slider inputGain;
    juce::Slider outputGain;
    juce::ToggleButton bypass { "Bypass" };
    juce::TabbedComponent ampTabs { juce::TabbedButtonBar::TabsAtTop };
    juce::Component browserPanel;
    juce::Label browserTitle;
    juce::Label browserSubtitle;
    std::array<juce::TextButton, 8> browserButtons { juce::TextButton { "AMPLIFIER" },
                                                    juce::TextButton { "TONE LAB" },
                                                    juce::TextButton { "NEURAL CAPTURE" },
                                                    juce::TextButton { "CIRCUIT" },
                                                    juce::TextButton { "TONE ANALYZER" },
                                                    juce::TextButton { "SONG MATCH" },
                                                    juce::TextButton { "ASSISTANT" },
                                                    juce::TextButton { "PROFILES" } };
    StudioSignalChain signalChain;
    StudioLevelMeter studioMeter;
    juce::Component simplePage;
    juce::Component advancedPage;
    juce::Component neuralPage;
    juce::Component circuitPage;
    juce::Component analyzerPage;
    juce::Component reconstructionPage;
    juce::Component assistantPage;
    juce::Component profilesPage;
    CircuitSchematicView circuitSchematic;
    ToneAnalysisView toneAnalysisView;
    ReconstructionView reconstructionView;
    AssistantView assistantView;
    juce::TextButton analyzeToneClip { "Analyze audio clip" };
    juce::Label analyzerHelp;
    juce::TextButton importSong { "Import song" };
    juce::TextButton cancelReconstruction { "Cancel" };
    juce::TextButton applyCandidate { "Apply candidate" };
    juce::TextButton exportCandidate { "Export safe profile" };
    juce::ComboBox reconstructionTarget;
    juce::ComboBox reconstructionStereoMode;
    juce::ComboBox reconstructionRegion;
    juce::TextButton useReconstructionRegion { "Use selected part" };
    juce::ComboBox reconstructionCandidate;
    juce::Label reconstructionHelp;
    juce::ComboBox assistantGoal;
    juce::ComboBox assistantSuggestion;
    juce::TextButton assistantPreview { "Preview" };
    juce::TextButton assistantAccept { "Accept" };
    juce::TextButton assistantReject { "Reject" };
    juce::TextButton assistantUndo { "Undo" };
    juce::ToggleButton assistantPersonalization { "Local personalization" };
    juce::TextButton clearAssistantPreferences { "Clear preferences" };
    juce::Label assistantHelp;
    juce::TextEditor profileSearch;
    juce::TextEditor profileName;
    juce::TextEditor profileAuthor;
    juce::ComboBox profileInstrument;
    juce::ComboBox profileList;
    juce::ToggleButton profileFavoritesOnly { "Favorites only" };
    juce::ToggleButton profileFavorite { "Favorite" };
    juce::TextButton importProfile { "Import .ntone" };
    juce::TextButton exportProfile { "Export current rig" };
    juce::TextButton applyProfile { "Apply profile" };
    juce::TextButton refreshProfiles { "Refresh" };
    juce::Label profileDetails;
    std::vector<nts::ecosystem::ProfileRecord> visibleProfiles;
    juce::String reconstructionRegionSignature;
    juce::Label circuitHelp;
    juce::ComboBox circuitPreampTube;
    juce::ComboBox circuitPowerTube;
    juce::ComboBox circuitPowerTopology;
    juce::ComboBox circuitToneStack;
    juce::ComboBox circuitBackend;
    juce::ComboBox circuitCabinetStyle;
    juce::ImageComponent ampArtwork;
    std::array<juce::Label, 7> simpleLabels;
    std::array<juce::Slider, 7> simpleSliders;
    std::array<juce::Label, 19> advancedLabels;
    std::array<juce::Slider, 19> advancedSliders;
    juce::ComboBox instrumentSelector;
    juce::ComboBox topologySelector;
    juce::ComboBox oversamplingSelector;
    juce::ToggleButton cabinetEnabled { "Cabinet enabled" };
    juce::ToggleButton gateEnabled { "Noise gate" };
    juce::ComboBox engineModeSelector;
    juce::ComboBox neuralMonitorSelector;
    juce::ToggleButton neuralCompensation { "Apply bounded input compensation" };
    juce::TextButton loadNeuralModel { "Load neural model" };
    juce::Label neuralStatus;
    juce::Label neuralCalibration;
    juce::Label captureWizardHelp;
    juce::TextButton audioSettings { "Audio settings" };
    juce::TextButton openProject { "Open project" };
    juce::TextButton saveProject { "Save project" };
    double inputMeterValue {};
    double outputMeterValue {};
    juce::ProgressBar inputMeter { inputMeterValue };
    juce::ProgressBar outputMeter { outputMeterValue };
    std::unique_ptr<juce::FileChooser> fileChooser;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> inputAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> outputAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> bypassAttachment;
    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>> ampSliderAttachments;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> instrumentAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> topologyAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> oversamplingAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> cabinetAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> gateAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> engineModeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> neuralMonitorAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> neuralCompensationAttachment;
    std::array<std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment>, 6> circuitAttachments;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TubeForgeAudioProcessorEditor)
};
