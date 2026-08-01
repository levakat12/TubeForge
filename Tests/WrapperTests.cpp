#include "TestHarness.h"
#include "PluginProcessor.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace
{
void testProcessorStateAndAudio(TestHarness& tests)
{
    TubeForgeAudioProcessor processor;
    processor.setPlayConfigDetails(2, 2, 48000.0, 32);
    processor.prepareToPlay(48000.0, 32);

    {
        // Oversampling defaults to Auto, which follows total stage drive. The
        // factory guitar patch is a four-stage cascade well into saturation, so
        // the default must be oversampled: at 1x it aliases audibly.
        auto* gainParameter = processor.getParameters().getParameter("gain");
        tests.expect(gainParameter != nullptr, "gain parameter exists");
        tests.expect(processor.getLatencySamples() > 0,
                     "auto oversampling engages for the default high-gain patch");

        if (gainParameter != nullptr)
        {
            gainParameter->setValueNotifyingHost(gainParameter->convertTo0to1(0.0f));
            processor.prepareToPlay(48000.0, 32);
            tests.expectEqual(processor.getLatencySamples(), 0,
                              "auto oversampling drops to 1x when the amp is clean");

            gainParameter->setValueNotifyingHost(gainParameter->convertTo0to1(10.0f));
            processor.prepareToPlay(48000.0, 32);
            tests.expect(processor.getLatencySamples() > 0,
                         "auto oversampling re-engages at maximum gain");
            gainParameter->setValueNotifyingHost(gainParameter->convertTo0to1(5.0f));
            processor.prepareToPlay(48000.0, 32);
        }
    }

    juce::AudioBuffer<float> buffer(2, 32);
    juce::MidiBuffer midi;
    for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
    {
        buffer.setSample(0, sample, 0.1f);
        buffer.setSample(1, sample, -0.1f);
    }
    processor.processBlock(buffer, midi);

    auto finite = true;
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
            finite = finite && std::isfinite(buffer.getSample(channel, sample));
    tests.expect(finite, "JUCE wrapper processes a valid block");

    {
        // The amplifier writes the host buffer in place, so the input meter has to
        // come from a snapshot taken first. A quiet input driven into a loud amp
        // setting is the case where reading the processed buffer is visibly wrong.
        auto* gainParameter = processor.getParameters().getParameter("gain");
        if (gainParameter != nullptr)
            gainParameter->setValueNotifyingHost(gainParameter->convertTo0to1(9.5f));

        constexpr auto quietInput = 0.02f;
        juce::AudioBuffer<float> quiet(2, 32);
        for (int block = 0; block < 8; ++block)
        {
            for (int channel = 0; channel < quiet.getNumChannels(); ++channel)
                for (int sample = 0; sample < quiet.getNumSamples(); ++sample)
                    quiet.setSample(channel, sample, quietInput);
            processor.processBlock(quiet, midi);
        }

        const auto reportedInput = std::max(processor.meterState().inputPeak(0),
                                            processor.meterState().inputPeak(1));
        tests.expectNear(reportedInput, quietInput, 1.0e-4,
                         "input meter reports the pre-amplifier level");
    }

    tests.expect(static_cast<juce::AudioProcessor&>(processor).getParameters().size() >= 28,
                 "wrapper exposes simple and advanced traditional-amp controls");
    auto allAutomatable = true;
    for (auto* parameter : static_cast<juce::AudioProcessor&>(processor).getParameters())
        allAutomatable = allAutomatable && parameter != nullptr && parameter->isAutomatable();
    tests.expect(allAutomatable, "all traditional-amp controls are host-automatable");

    auto* gain = processor.getParameters().getParameter("gain");
    tests.expect(gain != nullptr, "amp gain parameter exists");
    if (gain != nullptr)
    {
        gain->setValueNotifyingHost(gain->convertTo0to1(8.25f));
        juce::MemoryBlock ampState;
        processor.getStateInformation(ampState);
        gain->setValueNotifyingHost(gain->convertTo0to1(1.0f));
        processor.setStateInformation(ampState.getData(), static_cast<int>(ampState.getSize()));
        tests.expectNear(gain->convertFrom0to1(gain->getValue()), 8.25, 0.01,
                         "project state restores amp controls");
    }

    juce::MemoryBlock before;
    processor.getStateInformation(before);
    constexpr char invalidState[] = "{broken-state";
    processor.setStateInformation(invalidState, static_cast<int>(sizeof(invalidState) - 1));
    juce::MemoryBlock after;
    processor.getStateInformation(after);
    tests.expectEqual(before, after, "invalid wrapper state leaves the last known good state active");
}

void testEditorLifecycle(TestHarness& tests)
{
    TubeForgeAudioProcessor processor;
    for (int iteration = 0; iteration < 250; ++iteration)
    {
        std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
        tests.expect(editor != nullptr, "editor can be created repeatedly");
        if (editor == nullptr)
            break;
        editor->setSize(620 + (iteration % 20), 440 + (iteration % 20));
    }
}

void testPhysicalCircuitEditing(TestHarness& tests)
{
    TubeForgeAudioProcessor processor;
    const auto select = [&processor](const char* id, float choice)
    {
        if (auto* parameter = processor.getParameters().getParameter(id))
            parameter->setValueNotifyingHost(parameter->convertTo0to1(choice));
    };
    select("circuitPreampTube", 0.0f);
    select("circuitPowerTube", 0.0f);
    select("circuitPowerTopology", 0.0f);
    select("circuitToneStack", 2.0f);
    select("circuitBackend", 1.0f);
    select("circuitCabinetStyle", 2.0f);

    const auto edited = processor.circuitGraphSnapshot();
    const auto triode = std::find_if(edited.nodes.begin(), edited.nodes.end(), [](const auto& node)
    { return node.type == nts::circuit::NodeType::triodeStage; });
    const auto power = std::find_if(edited.nodes.begin(), edited.nodes.end(), [](const auto& node)
    { return node.type == nts::circuit::NodeType::powerStage; });
    const auto tone = std::find_if(edited.nodes.begin(), edited.nodes.end(), [](const auto& node)
    { return node.type == nts::circuit::NodeType::toneStack; });
    const auto cabinet = std::find_if(edited.nodes.begin(), edited.nodes.end(), [](const auto& node)
    { return node.type == nts::circuit::NodeType::cabinet; });
    tests.expect(triode != edited.nodes.end() && triode->modelId == "tube.12au7.v1"
                 && triode->backend == nts::circuit::ModelBackend::numerical,
                 "preamp selector switches both tube definition and solver backend");
    tests.expect(power != edited.nodes.end() && power->modelId == "power.6v6.v1"
                 && nts::circuit::parameterValue(*power, "topology", -1.0f) == 0.0f,
                 "power selectors switch tube family and single-ended topology");
    tests.expect(tone != edited.nodes.end() && tone->modelId == "passive.fmv.bass.v1",
                 "tone-stack selector switches the electrical network");
    tests.expect(cabinet != edited.nodes.end() && cabinet->modelId == "cabinet.bass-sealed.v1",
                 "cabinet selector switches the active cabinet part");

    juce::MemoryBlock state;
    processor.getStateInformation(state);
    select("circuitPreampTube", 2.0f);
    processor.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    const auto restored = processor.circuitGraphSnapshot();
    const auto restoredTriode = std::find_if(restored.nodes.begin(), restored.nodes.end(), [](const auto& node)
    { return node.type == nts::circuit::NodeType::triodeStage; });
    tests.expect(restoredTriode != restored.nodes.end() && restoredTriode->modelId == "tube.12au7.v1",
                 "project state restores the complete physical-circuit selection");
}

/// Guards the cached parameter pointer table.
///
/// Enumerator-to-id alignment is structural -- both expand from TUBEFORGE_RUNTIME_PARAMETERS
/// -- so it needs no test. What is still possible is a name in that list with no matching
/// parameter, and a parameter added to the layout that nobody added to the list, leaving it
/// unreadable from the audio thread. Both are checked here.
void testParameterPointerTable(TestHarness& tests)
{
    using Param = TubeForgeAudioProcessor::Param;
    TubeForgeAudioProcessor processor;
    constexpr auto count = static_cast<std::size_t>(Param::count);

    auto resolved = true;
    auto tracks = true;
    std::vector<juce::String> tabled;
    for (std::size_t index = 0; index < count; ++index)
    {
        const auto id = static_cast<Param>(index);
        const auto* name = TubeForgeAudioProcessor::parameterId(id);
        tabled.emplace_back(name);

        auto* parameter = processor.getParameters().getParameter(name);
        if (parameter == nullptr) { resolved = false; continue; }

        // A distinct position in each parameter's own range, then read the expectation back
        // through the parameter itself: floats snap to their step and choices quantise, so
        // the requested value is not always the stored one.
        parameter->setValueNotifyingHost(0.2f + 0.6f * static_cast<float>(index)
                                                     / static_cast<float>(count));
        const auto expected = parameter->convertFrom0to1(parameter->getValue());
        tracks = tracks && std::abs(processor.parameterOf(id) - expected) <= 1.0e-4f;
    }
    tests.expect(resolved, "every Param enumerator names a parameter that exists in the layout");
    tests.expect(tracks, "every cached pointer tracks its parameter's current value");

    auto covered = true;
    for (const auto* parameter : processor.juce::AudioProcessor::getParameters())
        if (const auto* ranged = dynamic_cast<const juce::RangedAudioParameter*>(parameter))
            covered = covered
                   && std::find(tabled.begin(), tabled.end(), ranged->getParameterID()) != tabled.end();
    tests.expect(covered, "every layout parameter is reachable through the Param table");
}

/// The Input parameter has to move the signal in every engine mode, not just Traditional.
void testInputTrimAppliesInEveryEngineMode(TestHarness& tests)
{
    using Param = TubeForgeAudioProcessor::Param;

    // Small enough that every engine stays essentially linear, so a trim change shows up
    // as a clean level ratio rather than being swallowed by saturation.
    constexpr auto amplitude = 0.02f;
    constexpr int blockSize = 256;

    const auto rmsAtTrim = [&](int engineMode, float trimDb)
    {
        TubeForgeAudioProcessor processor;
        processor.setPlayConfigDetails(2, 2, 48000.0, blockSize);

        const auto set = [&](Param id, float plain)
        {
            auto* parameter = processor.getParameters().getParameter(
                TubeForgeAudioProcessor::parameterId(id));
            if (parameter != nullptr)
                parameter->setValueNotifyingHost(parameter->convertTo0to1(plain));
        };
        set(Param::engineMode, static_cast<float>(engineMode));
        set(Param::input, trimDb);
        // Its own bounded gain would confound a level measurement.
        set(Param::neuralCompensation, 0.0f);
        processor.prepareToPlay(48000.0, blockSize);

        juce::AudioBuffer<float> buffer(2, blockSize);
        juce::MidiBuffer midi;
        double sum {};
        // The physical circuit compiles on a worker thread, so measuring straight after
        // prepareToPlay races that compile and reads whatever is published at the time. Warm
        // up until the output settles rather than assuming the engine is ready.
        for (int warmUp = 0; warmUp < 40; ++warmUp)
        {
            buffer.clear();
            processor.processBlock(buffer, midi);
            juce::Thread::sleep(1);
        }
        // Several blocks: the trim smoother needs to settle before the level is measured.
        for (int block = 0; block < 8; ++block)
        {
            for (int channel = 0; channel < 2; ++channel)
                for (int sample = 0; sample < blockSize; ++sample)
                    buffer.setSample(channel, sample,
                                     amplitude * std::sin(6.2831853f * 220.0f
                                                          * static_cast<float>(block * blockSize + sample)
                                                          / 48000.0f));
            processor.processBlock(buffer, midi);
            if (block == 7)
                for (int sample = 0; sample < blockSize; ++sample)
                    sum += static_cast<double>(buffer.getSample(0, sample))
                         * static_cast<double>(buffer.getSample(0, sample));
        }
        return std::sqrt(sum / blockSize);
    };

    for (const auto mode : { 1, 2 })
    {
        const auto quiet = rmsAtTrim(mode, 0.0f);
        const auto loud = rmsAtTrim(mode, 12.0f);
        const auto ratioDb = 20.0 * std::log10(std::max(loud, 1.0e-12) / std::max(quiet, 1.0e-12));
        tests.expect(quiet > 1.0e-9, "engine mode " + std::to_string(mode) + " produces signal");
        // What "correct" looks like differs by engine, so the bound does too.
        //
        // Neural mode with no model loaded passes its input through, so a 12 dB trim should
        // arrive as very nearly 12 dB. The physical circuit is a saturating tube model that is
        // genuinely compressing at this level -- it turns 12 dB in into about 6 dB out -- so
        // demanding 12 dB there would be demanding the circuit not work. An earlier version of
        // this test did pass at 12 dB in both modes, but only because it measured before the
        // circuit had finished compiling on its worker thread and was reading a passthrough.
        const auto lowerBound = mode == 1 ? 10.5 : 3.0;
        const auto upperBound = mode == 1 ? 13.5 : 12.5;
        tests.expect(ratioDb > lowerBound && ratioDb < upperBound,
                     "input trim scales the signal in engine mode " + std::to_string(mode)
                         + " (measured " + std::to_string(ratioDb) + " dB)");
    }
}

/// Bypass and engine changes must not step the signal, and bypass must stay time-aligned.
void testBypassAndEngineSwitching(TestHarness& tests)
{
    using Param = TubeForgeAudioProcessor::Param;
    constexpr int blockSize = 128;
    constexpr float frequency = 220.0f;

    TubeForgeAudioProcessor processor;
    processor.setPlayConfigDetails(2, 2, 48000.0, blockSize);
    processor.prepareToPlay(48000.0, blockSize);

    auto* bypass = processor.getParameters().getParameter(
        TubeForgeAudioProcessor::parameterId(Param::bypass));
    auto* engineMode = processor.getParameters().getParameter(
        TubeForgeAudioProcessor::parameterId(Param::engineMode));
    tests.expect(bypass != nullptr && engineMode != nullptr, "bypass and engine mode parameters exist");
    if (bypass == nullptr || engineMode == nullptr) return;

    tests.expect(processor.getBypassParameter() != nullptr,
                 "processor publishes a bypass parameter to the host");
    tests.expectEqual(processor.getBypassParameter()->getName(64), juce::String("Bypass"),
                      "the published bypass parameter is the bypass control");

    juce::AudioBuffer<float> buffer(2, blockSize);
    juce::MidiBuffer midi;
    int phase {};
    std::vector<float> captured;

    // Drives a number of blocks, optionally flipping a parameter partway, and records the
    // output so the seam can be inspected sample by sample.
    const auto run = [&](int blocks, int flipAtBlock, juce::RangedAudioParameter* parameter, float plain)
    {
        for (int block = 0; block < blocks; ++block)
        {
            if (block == flipAtBlock && parameter != nullptr)
                parameter->setValueNotifyingHost(parameter->convertTo0to1(plain));
            for (int sample = 0; sample < blockSize; ++sample)
            {
                const auto value = 0.05f * std::sin(6.2831853f * frequency
                                                    * static_cast<float>(phase + sample) / 48000.0f);
                buffer.setSample(0, sample, value);
                buffer.setSample(1, sample, value);
            }
            phase += blockSize;
            processor.processBlock(buffer, midi);
            for (int sample = 0; sample < blockSize; ++sample)
                captured.push_back(buffer.getSample(0, sample));
        }
    };

    // The largest single-sample jump anywhere in a capture. A hard switch shows up as a
    // step of roughly the signal amplitude; a ramped one stays near the per-sample slope.
    const auto largestStep = [](const std::vector<float>& samples)
    {
        auto largest = 0.0f;
        for (std::size_t index = 1; index < samples.size(); ++index)
            largest = std::max(largest, std::abs(samples[index] - samples[index - 1]));
        return largest;
    };

    captured.clear();
    run(8, -1, nullptr, 0.0f);
    const auto steadyStep = largestStep(captured);

    captured.clear();
    run(24, 4, bypass, 1.0f);
    tests.expect(largestStep(captured) < steadyStep * 4.0f + 1.0e-4f,
                 "engaging bypass does not step the output");

    // Once the fade has settled, bypass has to be transparent and in time with the input.
    captured.clear();
    run(16, -1, nullptr, 0.0f);
    auto transparent = true;
    const auto latency = processor.getLatencySamples();
    for (int sample = 0; sample < blockSize; ++sample)
    {
        const auto expectedPhase = phase - blockSize + sample - latency;
        const auto expected = 0.05f * std::sin(6.2831853f * frequency
                                               * static_cast<float>(expectedPhase) / 48000.0f);
        const auto actual = captured[captured.size() - blockSize + static_cast<std::size_t>(sample)];
        transparent = transparent && std::abs(actual - expected) < 2.0e-3f;
    }
    tests.expect(transparent, "bypass passes the input through delayed by the reported latency");

    captured.clear();
    run(24, 4, bypass, 0.0f);
    tests.expect(largestStep(captured) < steadyStep * 4.0f + 1.0e-4f,
                 "leaving bypass does not step the output");

    // Engine changes mute and unmute rather than switching under the signal.
    for (const auto mode : { 1.0f, 2.0f, 0.0f })
    {
        captured.clear();
        run(32, 4, engineMode, mode);
        tests.expect(largestStep(captured) < steadyStep * 4.0f + 1.0e-4f,
                     "changing to engine mode " + std::to_string(static_cast<int>(mode))
                         + " does not step the output");

        auto audible = false;
        for (std::size_t index = captured.size() - blockSize; index < captured.size(); ++index)
            audible = audible || std::abs(captured[index]) > 1.0e-5f;
        tests.expect(audible, "engine mode " + std::to_string(static_cast<int>(mode))
                                  + " unmutes after the switch completes");
    }
}

/// End-to-end user cabinet IR loading: decode a real file, hear it, revert it.
void testCabinetIrLoading(TestHarness& tests)
{
    constexpr int blockSize = 128;
    constexpr double sampleRate = 48000.0;

    // A short, obviously-coloured response: a strong pre-delay tap then a decaying ring, so
    // its effect on the output is unmistakable against the built-in cabinet.
    const auto irFile = juce::File::createTempFile("tubeforge-test-ir.wav");
    {
        juce::AudioBuffer<float> impulse(1, 256);
        impulse.clear();
        for (int sample = 0; sample < impulse.getNumSamples(); ++sample)
            impulse.setSample(0, sample,
                              (sample == 0 ? 0.9f : 0.0f)
                                  + 0.35f * std::exp(-static_cast<float>(sample) / 40.0f)
                                        * std::sin(0.9f * static_cast<float>(sample)));
        juce::WavAudioFormat format;
        std::unique_ptr<juce::OutputStream> stream(irFile.createOutputStream());
        tests.expect(stream != nullptr, "temporary impulse file can be created");
        if (stream == nullptr) return;
        auto writer = format.createWriterFor(stream, juce::AudioFormatWriterOptions {}
                                                         .withSampleRate(sampleRate)
                                                         .withNumChannels(1)
                                                         .withBitsPerSample(24));
        tests.expect(writer != nullptr, "temporary impulse file can be written");
        if (writer == nullptr) return;
        writer->writeFromAudioSampleBuffer(impulse, 0, impulse.getNumSamples());
    }

    TubeForgeAudioProcessor processor;
    processor.setPlayConfigDetails(2, 2, sampleRate, blockSize);
    processor.prepareToPlay(sampleRate, blockSize);

    juce::AudioBuffer<float> buffer(2, blockSize);
    juce::MidiBuffer midi;
    const auto renderImpulse = [&]
    {
        std::vector<float> captured;
        for (int block = 0; block < 8; ++block)
        {
            buffer.clear();
            if (block == 0) { buffer.setSample(0, 0, 1.0f); buffer.setSample(1, 0, 1.0f); }
            processor.processBlock(buffer, midi);
            for (int sample = 0; sample < blockSize; ++sample) captured.push_back(buffer.getSample(0, sample));
        }
        return captured;
    };

    const auto builtIn = renderImpulse();

    processor.requestCabinetIrLoad(0, irFile);
    // The loader runs off the message thread; give it a bounded window rather than a fixed sleep.
    auto loaded = false;
    for (int attempt = 0; attempt < 200 && ! loaded; ++attempt)
    {
        juce::Thread::sleep(10);
        loaded = processor.cabinetIrFile(0) == irFile;
    }
    tests.expect(loaded, "cabinet impulse response loads from a real audio file: "
                             + processor.cabinetIrStatusText(0).toStdString());
    if (! loaded) { irFile.deleteFile(); return; }

    // Several renders: the response fades in, so the comparison must be made once it is fully
    // in rather than part-way through the crossfade.
    std::vector<float> withUserIr;
    for (int attempt = 0; attempt < 4; ++attempt) withUserIr = renderImpulse();

    auto changed = false;
    for (std::size_t index = 0; index < builtIn.size() && index < withUserIr.size(); ++index)
        changed = changed || std::abs(builtIn[index] - withUserIr[index]) > 1.0e-4f;
    tests.expect(changed, "a loaded cabinet response actually changes the output");

    processor.clearCabinetIr(0);
    tests.expect(processor.cabinetIrFile(0) == juce::File {},
                 "clearing a cabinet slot forgets the user response");

    // Rejecting bad input matters as much as accepting good input.
    const auto bogus = juce::File::createTempFile("tubeforge-test-not-audio.wav");
    bogus.replaceWithText("this is not an audio file");
    processor.requestCabinetIrLoad(1, bogus);
    auto reported = false;
    for (int attempt = 0; attempt < 200 && ! reported; ++attempt)
    {
        juce::Thread::sleep(10);
        reported = processor.cabinetIrStatusText(1).isNotEmpty()
                && ! processor.cabinetIrStatusText(1).startsWith("Loading");
    }
    tests.expect(reported && processor.cabinetIrFile(1) == juce::File {},
                 "an unreadable file is reported and leaves the slot unchanged");

    irFile.deleteFile();
    bogus.deleteFile();
}

/// The tuner has to track the string through the real audio path, not a synthetic harness.
void testTuner(TestHarness& tests)
{
    constexpr int blockSize = 256;
    constexpr double sampleRate = 48000.0;

    TubeForgeAudioProcessor processor;
    processor.setPlayConfigDetails(2, 2, sampleRate, blockSize);
    processor.prepareToPlay(sampleRate, blockSize);

    const auto readAfterPlaying = [&](double frequency, int blocks)
    {
        juce::AudioBuffer<float> buffer(2, blockSize);
        juce::MidiBuffer midi;
        int phase {};
        for (int block = 0; block < blocks; ++block)
        {
            for (int sample = 0; sample < blockSize; ++sample)
            {
                const auto t = static_cast<double>(phase + sample) / sampleRate;
                const auto angle = 2.0 * 3.14159265358979 * frequency * t;
                const auto value = static_cast<float>(0.30 * std::sin(angle)
                                                      + 0.14 * std::sin(2.0 * angle + 0.3)
                                                      + 0.06 * std::sin(3.0 * angle + 1.0));
                buffer.setSample(0, sample, value);
                buffer.setSample(1, sample, value);
            }
            phase += blockSize;
            processor.processBlock(buffer, midi);
            // The queue is drained from the shell tick, so the test has to tick it too.
            processor.updateTuner();
        }
        return processor.tunerReading();
    };

    // Low E on a guitar, then low B on a five-string bass: the two ends that decide whether
    // the analysis window is long enough and the decimation is not eating the fundamental.
    const auto lowE = readAfterPlaying(82.407, 60);
    tests.expect(lowE.voiced, "tuner detects a plucked low E through the audio path");
    tests.expectEqual(lowE.midiNote, 40, "tuner identifies low E as E2");
    tests.expect(std::abs(lowE.cents) < 5.0f, "tuner reads an in-tune low E as in tune");

    const auto lowB = readAfterPlaying(30.868, 90);
    tests.expect(lowB.voiced, "tuner detects a five-string bass low B");
    tests.expectEqual(lowB.midiNote, 23, "tuner identifies low B as B0");

    // Direction matters: a player needs to know which way to turn the peg.
    const auto flat = readAfterPlaying(82.407 * std::pow(2.0, -20.0 / 1200.0), 60);
    tests.expect(flat.voiced && flat.cents < -10.0f, "tuner reports a flat string as flat");

    const auto sharp = readAfterPlaying(110.0 * std::pow(2.0, 20.0 / 1200.0), 60);
    tests.expect(sharp.voiced && sharp.cents > 10.0f, "tuner reports a sharp string as sharp");

    // Silence must clear the reading rather than leaving the last note on screen.
    juce::AudioBuffer<float> quiet(2, blockSize);
    juce::MidiBuffer midi;
    for (int block = 0; block < 90; ++block)
    {
        quiet.clear();
        processor.processBlock(quiet, midi);
        processor.updateTuner();
    }
    tests.expect(! processor.tunerReading().voiced, "tuner reports nothing when nothing is played");
}

/// Programs and MIDI program change: how a rig gets switched from a foot controller.
void testProgramsAndMidi(TestHarness& tests)
{
    constexpr int blockSize = 128;
    TubeForgeAudioProcessor processor;
    processor.setPlayConfigDetails(2, 2, 48000.0, blockSize);
    processor.prepareToPlay(48000.0, blockSize);

    auto& base = static_cast<juce::AudioProcessor&>(processor);
    tests.expectEqual(base.getNumPrograms(), 4, "the four factory voices are exposed as programs");
    tests.expect(base.acceptsMidi(), "the plug-in accepts MIDI so program changes are delivered");

    auto named = true;
    for (int program = 0; program < base.getNumPrograms(); ++program)
        named = named && base.getProgramName(program).isNotEmpty();
    tests.expect(named, "every program has a name");
    tests.expect(base.getProgramName(-1).isEmpty() && base.getProgramName(99).isEmpty(),
                 "an out-of-range program has no name rather than reading past the list");

    // A program is the instrument and topology parameters, so selecting one has to move them --
    // otherwise a program change and a manual change to the same controls would disagree.
    auto roundTrips = true;
    for (int program = 0; program < base.getNumPrograms(); ++program)
    {
        base.setCurrentProgram(program);
        roundTrips = roundTrips && base.getCurrentProgram() == program;
    }
    tests.expect(roundTrips, "selecting a program is reflected by the current program");

    base.setCurrentProgram(0);
    const auto instrumentOf = [&processor]
    {
        auto* parameter = processor.getParameters().getParameter("instrument");
        return parameter == nullptr ? -1.0f : parameter->convertFrom0to1(parameter->getValue());
    };
    base.setCurrentProgram(3);
    tests.expectNear(instrumentOf(), 1.0, 0.01, "a bass program selects the bass instrument");

    // A MIDI program change is latched on the audio thread and applied on the message thread,
    // so it takes a dispatch to land rather than arriving inside processBlock.
    base.setCurrentProgram(0);
    juce::AudioBuffer<float> buffer(2, blockSize);
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::programChange(1, 2), 0);
    buffer.clear();
    processor.processBlock(buffer, midi);
    tests.expectEqual(base.getCurrentProgram(), 0,
                      "a program change is not applied on the audio thread");
    juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
    tests.expectEqual(base.getCurrentProgram(), 2,
                      "a MIDI program change selects the program once dispatched");

    // Out-of-range program numbers must be ignored, not clamped into an unrelated rig.
    const auto before = base.getCurrentProgram();
    juce::MidiBuffer outOfRange;
    outOfRange.addEvent(juce::MidiMessage::programChange(1, 90), 0);
    buffer.clear();
    processor.processBlock(buffer, outOfRange);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
    tests.expectEqual(base.getCurrentProgram(), before,
                      "a program change outside the list is ignored");

    // And the switch itself has to go through the amplifier's crossfade, not step.
    base.setCurrentProgram(0);
    int phase {};
    std::vector<float> captured;
    const auto play = [&](int blocks, int switchAt)
    {
        for (int block = 0; block < blocks; ++block)
        {
            if (block == switchAt)
            {
                juce::MidiBuffer change;
                change.addEvent(juce::MidiMessage::programChange(1, 1), 0);
                for (int sample = 0; sample < blockSize; ++sample)
                {
                    const auto value = 0.05f * std::sin(6.2831853f * 220.0f
                                                        * static_cast<float>(phase + sample) / 48000.0f);
                    buffer.setSample(0, sample, value); buffer.setSample(1, sample, value);
                }
                phase += blockSize;
                processor.processBlock(buffer, change);
                juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
            }
            else
            {
                for (int sample = 0; sample < blockSize; ++sample)
                {
                    const auto value = 0.05f * std::sin(6.2831853f * 220.0f
                                                        * static_cast<float>(phase + sample) / 48000.0f);
                    buffer.setSample(0, sample, value); buffer.setSample(1, sample, value);
                }
                phase += blockSize;
                juce::MidiBuffer empty;
                processor.processBlock(buffer, empty);
            }
            for (int sample = 0; sample < blockSize; ++sample)
                captured.push_back(buffer.getSample(0, sample));
        }
    };

    captured.clear(); play(10, -1);
    auto steady = 0.0f;
    for (std::size_t index = 1; index < captured.size(); ++index)
        steady = std::max(steady, std::abs(captured[index] - captured[index - 1]));

    captured.clear(); play(40, 6);
    auto largest = 0.0f;
    for (std::size_t index = 1; index < captured.size(); ++index)
        largest = std::max(largest, std::abs(captured[index] - captured[index - 1]));
    tests.expect(largest < steady * 4.0f + 1.0e-4f,
                 "a MIDI program change crossfades rather than stepping the output");
}

/// The shared front end: trim and gate reaching the engines that had neither.
void testSharedFrontEnd(TestHarness& tests)
{
    constexpr int blockSize = 128;
    constexpr double sampleRate = 48000.0;

    // Level of the output after feeding a signal quiet enough to sit under the gate threshold.
    const auto quietOutputWithGate = [&](int engineMode, bool gateOn)
    {
        TubeForgeAudioProcessor processor;
        processor.setPlayConfigDetails(2, 2, sampleRate, blockSize);
        const auto set = [&](const char* id, float value)
        {
            if (auto* parameter = processor.getParameters().getParameter(id))
                parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
        };
        set("engineMode", static_cast<float>(engineMode));
        set("gateEnabled", gateOn ? 1.0f : 0.0f);
        set("gateThreshold", -30.0f);   // well above the test signal
        set("gateDepth", -90.0f);
        set("gateAttack", 1.0f);
        set("gateHold", 0.0f);
        set("gateRelease", 10.0f);
        set("input", 0.0f);
        processor.prepareToPlay(sampleRate, blockSize);

        juce::AudioBuffer<float> buffer(2, blockSize);
        juce::MidiBuffer midi;
        double peak {};
        int phase {};
        for (int block = 0; block < 80; ++block)
        {
            for (int sample = 0; sample < blockSize; ++sample)
            {
                // -46 dBFS: comfortably below a -30 dB threshold, so an active gate shuts it.
                const auto value = 0.005f * std::sin(6.2831853f * 220.0f
                                                     * static_cast<float>(phase + sample) / 48000.0f);
                buffer.setSample(0, sample, value); buffer.setSample(1, sample, value);
            }
            phase += blockSize;
            processor.processBlock(buffer, midi);
            if (block >= 60)
                for (int sample = 0; sample < blockSize; ++sample)
                    peak = std::max(peak, std::abs(static_cast<double>(buffer.getSample(0, sample))));
        }
        return peak;
    };

    for (const auto mode : { 1, 2 })
    {
        const auto open = quietOutputWithGate(mode, false);
        const auto gated = quietOutputWithGate(mode, true);
        tests.expect(open > 1.0e-6,
                     "engine mode " + std::to_string(mode) + " passes a quiet signal with the gate off");
        tests.expect(gated < open * 0.5,
                     "the gate attenuates a below-threshold signal in engine mode "
                         + std::to_string(mode));
    }

    // And the trim is now the shared one rather than the amplifier's, so it still has to
    // scale the traditional path by the amount asked for.
    const auto traditionalLevel = [&](float trimDb)
    {
        TubeForgeAudioProcessor processor;
        processor.setPlayConfigDetails(2, 2, sampleRate, blockSize);
        const auto set = [&](const char* id, float value)
        {
            if (auto* parameter = processor.getParameters().getParameter(id))
                parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
        };
        set("engineMode", 0.0f); set("input", trimDb); set("gain", 0.0f);
        set("gateEnabled", 0.0f); set("cabinet", 0.0f);
        processor.prepareToPlay(sampleRate, blockSize);

        juce::AudioBuffer<float> buffer(2, blockSize);
        juce::MidiBuffer midi;
        double energy {}; int phase {}; int counted {};
        for (int block = 0; block < 60; ++block)
        {
            for (int sample = 0; sample < blockSize; ++sample)
            {
                const auto value = 0.01f * std::sin(6.2831853f * 220.0f
                                                    * static_cast<float>(phase + sample) / 48000.0f);
                buffer.setSample(0, sample, value); buffer.setSample(1, sample, value);
            }
            phase += blockSize;
            processor.processBlock(buffer, midi);
            if (block >= 50)
                for (int sample = 0; sample < blockSize; ++sample)
                {
                    const auto out = static_cast<double>(buffer.getSample(0, sample));
                    energy += out * out; ++counted;
                }
        }
        return std::sqrt(energy / std::max(1, counted));
    };

    const auto quiet = traditionalLevel(0.0f);
    const auto loud = traditionalLevel(12.0f);
    tests.expect(quiet > 1.0e-9 && loud > quiet * 1.5,
                 "the shared trim still drives the traditional amplifier");
}

/// The effects send, and the guarantee that adding it changed nothing for existing projects.
void testTimeBasedEffects(TestHarness& tests)
{
    constexpr int blockSize = 128;
    constexpr double sampleRate = 48000.0;

    const auto render = [&](float delayMix, float reverbMix)
    {
        TubeForgeAudioProcessor processor;
        processor.setPlayConfigDetails(2, 2, sampleRate, blockSize);
        const auto set = [&](const char* id, float value)
        {
            if (auto* parameter = processor.getParameters().getParameter(id))
                parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
        };
        set("delayMix", delayMix); set("reverbMix", reverbMix);
        set("delayTime", 120.0f); set("delayFeedback", 40.0f);
        processor.prepareToPlay(sampleRate, blockSize);

        juce::AudioBuffer<float> buffer(2, blockSize);
        juce::MidiBuffer midi;
        std::vector<float> captured;
        for (int block = 0; block < 80; ++block)
        {
            buffer.clear();
            // One short burst, then silence: anything after it is the effect, not the source.
            if (block < 2)
                for (int sample = 0; sample < blockSize; ++sample)
                {
                    const auto value = 0.2f * std::sin(6.2831853f * 220.0f
                                                       * static_cast<float>(block * blockSize + sample) / 48000.0f);
                    buffer.setSample(0, sample, value); buffer.setSample(1, sample, value);
                }
            processor.processBlock(buffer, midi);
            for (int sample = 0; sample < blockSize; ++sample)
                captured.push_back(buffer.getSample(0, sample));
        }
        return captured;
    };

    const auto dry = render(0.0f, 0.0f);
    // Energy long after the burst has stopped: with no effects there should be essentially none.
    const auto lateEnergy = [&](const std::vector<float>& samples)
    {
        double energy {};
        for (std::size_t index = samples.size() / 2; index < samples.size(); ++index)
            energy += static_cast<double>(samples[index]) * samples[index];
        return energy;
    };

    const auto withDelay = render(60.0f, 0.0f);
    const auto withReverb = render(0.0f, 60.0f);

    tests.expect(lateEnergy(withDelay) > lateEnergy(dry) * 10.0 + 1.0e-9,
                 "the delay leaves repeats after the source has stopped");
    tests.expect(lateEnergy(withReverb) > lateEnergy(dry) * 10.0 + 1.0e-9,
                 "the reverb leaves a tail after the source has stopped");

    auto finite = true;
    for (const auto value : withDelay) finite = finite && std::isfinite(value) && std::abs(value) < 8.0f;
    for (const auto value : withReverb) finite = finite && std::isfinite(value) && std::abs(value) < 8.0f;
    tests.expect(finite, "the effects stay finite and bounded through the plug-in");

    // The default is silent, so a project made before the effects existed sounds identical.
    TubeForgeAudioProcessor fresh;
    auto* delayMix = fresh.getParameters().getParameter("delayMix");
    auto* reverbMix = fresh.getParameters().getParameter("reverbMix");
    tests.expect(delayMix != nullptr && reverbMix != nullptr, "effect mix parameters exist");
    if (delayMix != nullptr && reverbMix != nullptr)
        tests.expect(delayMix->convertFrom0to1(delayMix->getValue()) == 0.0f
                     && reverbMix->convertFrom0to1(reverbMix->getValue()) == 0.0f,
                     "the effects default to silent, so existing projects are unaffected");

    // A reverb tail is seconds where a cabinet is milliseconds, so the host has to be told.
    TubeForgeAudioProcessor tailed;
    tailed.setPlayConfigDetails(2, 2, sampleRate, blockSize);
    if (auto* parameter = tailed.getParameters().getParameter("reverbMix"))
        parameter->setValueNotifyingHost(parameter->convertTo0to1(80.0f));
    tailed.prepareToPlay(sampleRate, blockSize);
    juce::AudioBuffer<float> warm(2, blockSize);
    juce::MidiBuffer midi;
    warm.clear();
    tailed.processBlock(warm, midi);
    tests.expect(tailed.getTailLengthSeconds() > 0.2,
                 "an engaged reverb lengthens the tail reported to the host");
}

/// The last three additions: cabinet blend, tuning mute, and the partitioned-block guard.
void testFinalAdditions(TestHarness& tests)
{
    constexpr int blockSize = 128;
    constexpr double sampleRate = 48000.0;

    // Cabinet blend has to reach the amplifier: the two responses differ, so moving between
    // them must change the sound.
    const auto renderAtBlend = [&](float blendPercent)
    {
        TubeForgeAudioProcessor processor;
        processor.setPlayConfigDetails(2, 2, sampleRate, blockSize);
        const auto set = [&](const char* id, float value)
        {
            if (auto* parameter = processor.getParameters().getParameter(id))
                parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
        };
        set("engineMode", 0.0f); set("cabinet", 1.0f); set("cabinetBlend", blendPercent);
        processor.prepareToPlay(sampleRate, blockSize);

        juce::AudioBuffer<float> buffer(2, blockSize);
        juce::MidiBuffer midi;
        std::vector<float> captured;
        for (int block = 0; block < 24; ++block)
        {
            buffer.clear();
            if (block == 0) { buffer.setSample(0, 0, 0.5f); buffer.setSample(1, 0, 0.5f); }
            processor.processBlock(buffer, midi);
            for (int sample = 0; sample < blockSize; ++sample)
                captured.push_back(buffer.getSample(0, sample));
        }
        return captured;
    };

    const auto allA = renderAtBlend(0.0f);
    const auto allB = renderAtBlend(100.0f);
    auto different = false;
    for (std::size_t index = 0; index < allA.size() && index < allB.size(); ++index)
        different = different || std::abs(allA[index] - allB[index]) > 1.0e-4f;
    tests.expect(different, "the cabinet blend control selects between the two responses");

    // Mute while tuning silences the output without stopping the tuner.
    TubeForgeAudioProcessor processor;
    processor.setPlayConfigDetails(2, 2, sampleRate, blockSize);
    processor.prepareToPlay(sampleRate, blockSize);

    juce::AudioBuffer<float> buffer(2, blockSize);
    juce::MidiBuffer midi;
    int phase {};
    const auto play = [&](int blocks)
    {
        double peak {};
        for (int block = 0; block < blocks; ++block)
        {
            for (int sample = 0; sample < blockSize; ++sample)
            {
                const auto value = 0.3f * std::sin(6.2831853f * 110.0f
                                                   * static_cast<float>(phase + sample) / 48000.0f);
                buffer.setSample(0, sample, value); buffer.setSample(1, sample, value);
            }
            phase += blockSize;
            processor.processBlock(buffer, midi);
            processor.updateTuner();
            if (block >= blocks - 4)
                for (int sample = 0; sample < blockSize; ++sample)
                    peak = std::max(peak, std::abs(static_cast<double>(buffer.getSample(0, sample))));
        }
        return peak;
    };

    // The detector needs a quarter-second frame at its 8 kHz analysis rate, which is about
    // 94 blocks of 128 samples. Feeding less and then asserting on the reading would be
    // testing the test's patience rather than the plug-in.
    constexpr int blocksForAFullTunerFrame = 150;

    const auto loud = play(blocksForAFullTunerFrame);
    tests.expect(loud > 1.0e-4, "the amplifier is audible before the tuning mute is engaged");
    tests.expect(processor.tunerReading().voiced, "the tuner has a reading before muting");

    if (auto* mute = processor.getParameters().getParameter("tunerMute"))
        mute->setValueNotifyingHost(1.0f);
    const auto muted = play(blocksForAFullTunerFrame);
    tests.expect(muted < loud * 0.02, "engaging the tuning mute silences the output");
    tests.expect(processor.tunerReading().voiced,
                 "the tuner keeps tracking while the output is muted");

    if (auto* mute = processor.getParameters().getParameter("tunerMute"))
        mute->setValueNotifyingHost(0.0f);
    tests.expect(play(blocksForAFullTunerFrame) > 1.0e-4,
                 "releasing the tuning mute restores the output");
}

/// Contract the host relies on: how long to keep pulling blocks after transport stops.
void testHostTailReporting(TestHarness& tests)
{
    TubeForgeAudioProcessor processor;
    processor.setPlayConfigDetails(2, 2, 48000.0, 32);
    processor.prepareToPlay(48000.0, 32);

    // The cabinet convolves a real impulse, so a zero tail would truncate the decay of
    // every offline render -- silent data loss that never shows up while monitoring live.
    const auto tail = processor.getTailLengthSeconds();
    tests.expect(tail > 0.0, "processor reports a non-zero tail while the cabinet is active");
    tests.expect(tail < 1.0, "reported tail stays within a sane bound");
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI initialiseJuce;
    TestHarness tests;
    testProcessorStateAndAudio(tests);
    testPhysicalCircuitEditing(tests);
    testParameterPointerTable(tests);
    testInputTrimAppliesInEveryEngineMode(tests);
    testBypassAndEngineSwitching(tests);
    testCabinetIrLoading(tests);
    testTuner(tests);
    testProgramsAndMidi(tests);
    testSharedFrontEnd(tests);
    testTimeBasedEffects(tests);
    testFinalAdditions(tests);
    testHostTailReporting(tests);
    testEditorLifecycle(tests);
    return tests.result();
}
