#pragma once

#include <nts/audio/CoreEngine.h>
#include <nts/diagnostics/DiagnosticsCollector.h>
#include <nts/diagnostics/LatencyBudget.h>
#include <nts/diagnostics/StructuredLogger.h>
#include <nts/state/ProjectState.h>

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <cstdint>

class TubeForgeAudioProcessor final : public juce::AudioProcessor
{
public:
    TubeForgeAudioProcessor();
    ~TubeForgeAudioProcessor() override = default;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;
    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int index, const juce::String& newName) override;

    void getStateInformation(juce::MemoryBlock& destinationData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    [[nodiscard]] juce::Result saveProject(const juce::File& file) const;
    [[nodiscard]] juce::Result loadProject(const juce::File& file);
    [[nodiscard]] nts::diagnostics::DiagnosticsSnapshot diagnosticsSnapshot() const noexcept;
    [[nodiscard]] const nts::audio::MeterState& meterState() const noexcept { return meters; }
    [[nodiscard]] juce::String modeName() const;
    [[nodiscard]] juce::String deviceStatusText() const;
    void refreshNonRealtimeDiagnostics() noexcept;
    void setStandaloneApplicationMode(bool shouldBeStandalone) noexcept
    {
        standaloneApplicationMode = shouldBeStandalone;
    }

    juce::AudioProcessorValueTreeState& getParameters() noexcept { return parameterState; }
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

private:
    static float valueOf(const juce::AudioProcessorValueTreeState& state, const char* parameterId) noexcept;
    static void setParameterValue(juce::AudioProcessorValueTreeState& state,
                                  const char* parameterId,
                                  float plainValue);
    [[nodiscard]] nts::state::ProjectState makeProjectState() const;
    bool applyProjectState(const nts::state::ProjectState& state);
    void updateReportedLatency();

    juce::AudioProcessorValueTreeState parameterState;
    nts::audio::RuntimeParameters runtimeParameters;
    nts::audio::MeterState meters;
    nts::audio::CoreEngine engine;
    nts::diagnostics::DiagnosticsCollector diagnostics;
    nts::diagnostics::LatencyBudget latencyBudget;
    nts::diagnostics::StructuredLogger logger;
    std::uint64_t absoluteSamplePosition {};
    double currentSampleRate { 44100.0 };
    int currentBlockSize {};
    bool standaloneApplicationMode {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TubeForgeAudioProcessor)
};
