#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace ParameterIds
{
constexpr auto input = "input";
constexpr auto drive = "drive";
constexpr auto bass = "bass";
constexpr auto mid = "mid";
constexpr auto treble = "treble";
constexpr auto presence = "presence";
constexpr auto mix = "mix";
constexpr auto output = "output";
constexpr auto bypass = "bypass";
} // namespace ParameterIds

TubeForgeAudioProcessor::TubeForgeAudioProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      parameters(*this, nullptr, "TubeForgeState", createParameterLayout())
{
}

void TubeForgeAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    ampEngine.prepare(sampleRate, samplesPerBlock, getTotalNumOutputChannels());
}

void TubeForgeAudioProcessor::releaseResources()
{
    ampEngine.reset();
}

bool TubeForgeAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto input = layouts.getMainInputChannelSet();
    const auto output = layouts.getMainOutputChannelSet();
    const auto supportedChannelCount = output == juce::AudioChannelSet::mono()
                                    || output == juce::AudioChannelSet::stereo();
    return supportedChannelCount && input == output;
}

void TubeForgeAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ignoreUnused(midi);
    juce::ScopedNoDenormals noDenormals;

    for (auto channel = getTotalNumInputChannels(); channel < getTotalNumOutputChannels(); ++channel)
        buffer.clear(channel, 0, buffer.getNumSamples());

    if (valueOf(parameters, ParameterIds::bypass) >= 0.5f)
        return;

    tubeforge::dsp::AmpParameters values;
    values.inputDb = valueOf(parameters, ParameterIds::input);
    values.driveDb = valueOf(parameters, ParameterIds::drive);
    values.bassDb = valueOf(parameters, ParameterIds::bass);
    values.midDb = valueOf(parameters, ParameterIds::mid);
    values.trebleDb = valueOf(parameters, ParameterIds::treble);
    values.presenceDb = valueOf(parameters, ParameterIds::presence);
    values.mix = valueOf(parameters, ParameterIds::mix) * 0.01f;
    values.outputDb = valueOf(parameters, ParameterIds::output);
    ampEngine.setParameters(values);
    ampEngine.process(buffer);
}

juce::AudioProcessorEditor* TubeForgeAudioProcessor::createEditor()
{
    return new TubeForgeAudioProcessorEditor(*this);
}

bool TubeForgeAudioProcessor::hasEditor() const { return true; }
const juce::String TubeForgeAudioProcessor::getName() const { return JucePlugin_Name; }
bool TubeForgeAudioProcessor::acceptsMidi() const { return false; }
bool TubeForgeAudioProcessor::producesMidi() const { return false; }
bool TubeForgeAudioProcessor::isMidiEffect() const { return false; }
double TubeForgeAudioProcessor::getTailLengthSeconds() const { return 0.0; }
int TubeForgeAudioProcessor::getNumPrograms() { return 1; }
int TubeForgeAudioProcessor::getCurrentProgram() { return 0; }
void TubeForgeAudioProcessor::setCurrentProgram(int index) { juce::ignoreUnused(index); }
const juce::String TubeForgeAudioProcessor::getProgramName(int index) { juce::ignoreUnused(index); return {}; }
void TubeForgeAudioProcessor::changeProgramName(int index, const juce::String& newName)
{
    juce::ignoreUnused(index, newName);
}

void TubeForgeAudioProcessor::getStateInformation(juce::MemoryBlock& destinationData)
{
    if (auto xml = parameters.copyState().createXml())
        copyXmlToBinary(*xml, destinationData);
}

void TubeForgeAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary(data, sizeInBytes))
        if (xml->hasTagName(parameters.state.getType()))
            parameters.replaceState(juce::ValueTree::fromXml(*xml));
}

juce::AudioProcessorValueTreeState::ParameterLayout TubeForgeAudioProcessor::createParameterLayout()
{
    using Float = juce::AudioParameterFloat;
    using Bool = juce::AudioParameterBool;
    using Range = juce::NormalisableRange<float>;
    using FloatAttributes = juce::AudioParameterFloatAttributes;

    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add(std::make_unique<Float>(juce::ParameterID { ParameterIds::input, 1 }, "Input", Range { -24.0f, 24.0f, 0.1f }, 0.0f, FloatAttributes {}.withLabel("dB")));
    layout.add(std::make_unique<Float>(juce::ParameterID { ParameterIds::drive, 1 }, "Drive", Range { 0.0f, 36.0f, 0.1f }, 12.0f, FloatAttributes {}.withLabel("dB")));
    layout.add(std::make_unique<Float>(juce::ParameterID { ParameterIds::bass, 1 }, "Bass", Range { -12.0f, 12.0f, 0.1f }, 0.0f, FloatAttributes {}.withLabel("dB")));
    layout.add(std::make_unique<Float>(juce::ParameterID { ParameterIds::mid, 1 }, "Mid", Range { -12.0f, 12.0f, 0.1f }, 0.0f, FloatAttributes {}.withLabel("dB")));
    layout.add(std::make_unique<Float>(juce::ParameterID { ParameterIds::treble, 1 }, "Treble", Range { -12.0f, 12.0f, 0.1f }, 0.0f, FloatAttributes {}.withLabel("dB")));
    layout.add(std::make_unique<Float>(juce::ParameterID { ParameterIds::presence, 1 }, "Presence", Range { -12.0f, 12.0f, 0.1f }, 0.0f, FloatAttributes {}.withLabel("dB")));
    layout.add(std::make_unique<Float>(juce::ParameterID { ParameterIds::mix, 1 }, "Mix", Range { 0.0f, 100.0f, 0.1f }, 100.0f, FloatAttributes {}.withLabel("%")));
    layout.add(std::make_unique<Float>(juce::ParameterID { ParameterIds::output, 1 }, "Output", Range { -30.0f, 12.0f, 0.1f }, -6.0f, FloatAttributes {}.withLabel("dB")));
    layout.add(std::make_unique<Bool>(juce::ParameterID { ParameterIds::bypass, 1 }, "Bypass", false));
    return layout;
}

float TubeForgeAudioProcessor::valueOf(const juce::AudioProcessorValueTreeState& state, const char* parameterId) noexcept
{
    if (const auto* value = state.getRawParameterValue(parameterId))
        return value->load();
    return 0.0f;
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new TubeForgeAudioProcessor();
}
