#include "PackedFixtures.h"
#include "TestHarness.h"

#include <nts/ir/CabinetModel.h>
#include "PluginProcessor.h"
#include "ui/FaceplateArt.h"
#include "ui/PedalArt.h"

#include <nts/ml/PackedWaveNetModel.h>
#include <nts/nam/CaptureLibrary.h>

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_cryptography/juce_cryptography.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>
#include <algorithm>
#include <chrono>
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

void testPedalboard(TestHarness& tests)
{
    using PedalControl = TubeForgeAudioProcessor::PedalControl;

    TubeForgeAudioProcessor processor;
    processor.setPlayConfigDetails(2, 2, 48000.0, 64);
    processor.prepareToPlay(48000.0, 64);

    const auto set = [&processor](std::size_t slot, PedalControl control, float value)
    {
        auto* parameter = processor.getParameters().getParameter(
            TubeForgeAudioProcessor::pedalParameterId(slot, control));
        if (parameter != nullptr)
            parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
        return parameter != nullptr;
    };

    auto everyParameterExists = true;
    for (std::size_t slot = 0; slot < nts::pedals::slotCount; ++slot)
        for (const auto control : { PedalControl::kind, PedalControl::bypass, PedalControl::drive,
                                    PedalControl::tone, PedalControl::level, PedalControl::mix,
                                    PedalControl::auxA, PedalControl::auxB })
            everyParameterExists = everyParameterExists
                && processor.getParameters().getParameter(
                       TubeForgeAudioProcessor::pedalParameterId(slot, control)) != nullptr;
    tests.expect(everyParameterExists, "every pedal slot's eight controls are registered parameters");

    // Every model is reachable from the host. A choice list shorter than the model table would
    // leave the last pedals selectable only from a saved project.
    if (auto* kind = processor.getParameters().getParameter(
            TubeForgeAudioProcessor::pedalParameterId(0, PedalControl::kind)))
        tests.expectEqual(static_cast<std::size_t>(kind->getNumSteps()), nts::pedals::modelCount(),
                          "the pedal kind parameter offers exactly the models the engine has");

    // The archetypes keep the indices every saved project already refers to. Appending to the
    // table must never renumber them.
    auto archetypesInPlace = true;
    const std::array<std::pair<nts::pedals::PedalKind, std::string_view>, 7> archetypes { {
        { nts::pedals::PedalKind::none, "Empty" },
        { nts::pedals::PedalKind::boost, "Boost" },
        { nts::pedals::PedalKind::overdrive, "Overdrive" },
        { nts::pedals::PedalKind::distortion, "Distortion" },
        { nts::pedals::PedalKind::fuzz, "Fuzz" },
        { nts::pedals::PedalKind::compressor, "Compressor" },
        { nts::pedals::PedalKind::neuralCapture, "Neural capture" } } };
    for (const auto& [kind, name] : archetypes)
        archetypesInPlace = archetypesInPlace
            && nts::pedals::pedalModel(nts::pedals::modelIndexOf(kind)).name == name;
    tests.expect(archetypesInPlace,
                 "the built-in archetypes keep the model indices saved projects refer to");

    juce::MidiBuffer midi;
    const auto renderPeak = [&processor, &midi]
    {
        juce::AudioBuffer<float> buffer(2, 64);
        auto peak = 0.0f;
        // Long enough for the slot's blend ramp, which is 30 ms, to have settled.
        for (int block = 0; block < 64; ++block)
        {
            for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
                for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
                    buffer.setSample(channel, sample,
                                     0.05f * std::sin(2.0f * 3.14159265f * 220.0f
                                                      * static_cast<float>(block * 64 + sample) / 48000.0f));
            processor.processBlock(buffer, midi);
            if (block >= 48) peak = std::max(peak, buffer.getMagnitude(0, 0, buffer.getNumSamples()));
        }
        return peak;
    };

    const auto setEngineMode = [&processor](float mode)
    {
        if (auto* parameter = processor.getParameters().getParameter("engineMode"))
            parameter->setValueNotifyingHost(parameter->convertTo0to1(mode));
    };

    // Measured through the neural engine with no capture loaded, which passes its buffer
    // through untouched. That makes the board the only thing in the chain doing anything, so
    // a level change here is the board's and nothing else's. Measuring through the amplifier
    // instead would be measuring its loudness match, which exists precisely to absorb a level
    // change at its input.
    setEngineMode(1.0f);
    const auto withoutPedals = renderPeak();
    tests.expect(std::isfinite(withoutPedals) && withoutPedals > 0.0f,
                 "the default rig -- every slot on None -- passes audio");

    tests.expect(set(0, PedalControl::kind, 1.0f), "slot 1 kind is settable");
    set(0, PedalControl::drive, 10.0f);
    set(0, PedalControl::mix, 100.0f);
    const auto withBoost = renderPeak();
    tests.expect(withBoost > withoutPedals * 1.5f, "a boost in slot 1 reaches the output");

    // And taking it out again has to put the rig back where it was, or "no pedal at all" is
    // not really available once a pedal has been used.
    set(0, PedalControl::kind, 0.0f);
    const auto afterRemoval = renderPeak();
    tests.expectNear(afterRemoval, withoutPedals, 1.0e-4,
                     "setting the slot back to None restores the rig it started as");

    // Now the position claim, through the amplifier: a boost ahead of a saturating front end
    // changes what comes out even though the amplifier's loudness match is working against
    // any change in level. If the board were downstream, or absent from this engine's path,
    // the two renders would agree.
    setEngineMode(0.0f);
    const auto amplifiedClean = renderPeak();
    set(0, PedalControl::kind, 1.0f);
    const auto amplifiedBoosted = renderPeak();
    tests.expect(std::abs(amplifiedBoosted - amplifiedClean) > amplifiedClean * 0.01f,
                 "a boost ahead of the amplifier changes what the amplifier produces");
    set(0, PedalControl::kind, 0.0f);

    // Round-trip through the host's own state, which is where a page of controls that is not
    // in the project format quietly loses everything the user set.
    set(1, PedalControl::kind, 6.0f);
    set(1, PedalControl::drive, 7.5f);
    set(1, PedalControl::mix, 42.0f);
    set(1, PedalControl::level, -6.0f);
    juce::MemoryBlock state;
    processor.getStateInformation(state);
    set(1, PedalControl::kind, 0.0f);
    set(1, PedalControl::drive, 1.0f);
    set(1, PedalControl::mix, 100.0f);
    set(1, PedalControl::level, 0.0f);
    processor.setStateInformation(state.getData(), static_cast<int>(state.getSize()));

    const auto read = [&processor](std::size_t slot, PedalControl control)
    {
        const auto* value = processor.getParameters().getRawParameterValue(
            TubeForgeAudioProcessor::pedalParameterId(slot, control));
        return value == nullptr ? 0.0f : value->load();
    };
    tests.expectNear(read(1, PedalControl::kind), 6.0, 1.0e-4, "project state restores a slot's kind");
    tests.expectNear(read(1, PedalControl::drive), 7.5, 0.01, "project state restores a slot's drive");
    tests.expectNear(read(1, PedalControl::mix), 42.0, 0.05, "project state restores a slot's mix");
    tests.expectNear(read(1, PedalControl::level), -6.0, 0.05, "project state restores a slot's level");

    // A slot set to a neural capture with nothing loaded must not go silent, and must say so
    // rather than reporting a model it does not have.
    tests.expect(processor.pedalModelStatusText(1).isNotEmpty(),
                 "a pedal slot reports a model status");
    tests.expect(processor.pedalModelFile(1) == juce::File {},
                 "a slot with no capture loaded reports no capture file");
    const auto withEmptyCapture = renderPeak();
    tests.expect(std::isfinite(withEmptyCapture) && withEmptyCapture > 0.0f,
                 "a neural slot with no capture loaded still passes audio");

    // Out-of-range slot indices are a real possibility from a corrupt project, and must not
    // reach past the end of the board.
    processor.requestPedalModelLoad(-1, juce::File {});
    processor.requestPedalModelLoad(static_cast<int>(nts::pedals::slotCount), juce::File {});
    tests.expect(processor.pedalModelStatusText(-1).isEmpty(),
                 "an out-of-range pedal slot reports nothing rather than reading past the board");
}

void testCaptureConvertsAndPlays(TestHarness& tests)
{
    // The whole path in one test: a `.nam` on disk, converted in-process the way the plug-in
    // converts one, staged into a pedal slot through the same loader the editor uses, and heard.
    // Each half of this is covered elsewhere; what is only covered here is that they join up.
    const auto unique = juce::String(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto workspace = juce::File::getSpecialLocation(juce::File::tempDirectory)
                               .getChildFile("tubeforge-capture-" + unique);
    workspace.createDirectory();

    // A capture small enough to convert in milliseconds. The geometry is checked against the
    // layout formula by nts_nam_tests; here it only has to be a capture the reader accepts.
    const auto capture = workspace.getChildFile("Tiny Drive.nam");
    {
        juce::String weights("[");
        // channels 2, layers (kernel 2, dilation 1) and (kernel 3, dilation 2), head kernel 3.
        constexpr int count = 50;
        for (int index = 0; index < count; ++index)
            weights += (index > 0 ? "," : "")
                     + juce::String(index + 1 == count ? 0.75 : 0.01 * (index + 1) - 0.2, 8);
        weights += "]";
        capture.replaceWithText(
            R"({"version":"0.7.0","architecture":"WaveNet","sample_rate":48000.0,)"
            R"("config":{"head":null,"head_scale":0.75,"layers":[{"channels":2,"input_size":1,)"
            R"("condition_size":1,"bottleneck":2,"kernel_sizes":[2,3],"dilations":[1,2],)"
            R"("activation":[{"type":"LeakyReLU","negative_slope":0.01},)"
            R"({"type":"LeakyReLU","negative_slope":0.01}],"layer1x1":{"active":true,"groups":1},)"
            R"("head":{"out_channels":1,"kernel_size":3,"bias":true}}]},"weights":)" + weights
            + R"(,"metadata":{"name":"Tiny Drive","modeled_by":"the test suite",)"
              R"("gear_make":"Acme","gear_model":"Tiny","gear_type":"pedal","tone_type":"crunch",)"
              R"("loudness":-23.0,"training":{"validation_esr":0.01}}})");
    }

    nts::nam::CaptureLibrary library(workspace.getChildFile("library"));
    std::stop_source stop;
    const auto report = library.import(capture, nts::nam::Tier::standard, stop.get_token(), {});
    tests.expectEqual(report.converted, 1, "the plug-in's own converter reads a .nam from disk");
    if (library.entries().empty())
    {
        tests.expect(false, "the converted capture entered the library");
        workspace.deleteRecursively();
        return;
    }
    const auto entry = library.entries().front();
    tests.expect(entry.gear == nts::nam::GearKind::pedal, "gear_type routes the capture to pedals");

    TubeForgeAudioProcessor processor;
    processor.setPlayConfigDetails(2, 2, 48000.0, 64);
    processor.prepareToPlay(48000.0, 64);
    processor.requestPedalModelLoad(0, entry.artifact);

    // The load runs on a worker and validates the model against the artifact's own vectors
    // before it will activate, so this waits rather than assuming.
    const auto deadline = juce::Time::getMillisecondCounter() + 8000;
    while (! processor.pedalModelStatusText(0).startsWith("Ready")
           && juce::Time::getMillisecondCounter() < deadline)
        juce::Thread::sleep(20);
    tests.expect(processor.pedalModelStatusText(0).startsWith("Ready"),
                 "a converted capture stages into a pedal slot: "
                     + processor.pedalModelStatusText(0).toStdString());
    tests.expect(processor.pedalModelFile(0) == entry.artifact,
                 "the slot records which capture it loaded");

    // Now hear it. The engine is put in neural mode with no amp model loaded, so the amplifier
    // passes its buffer through untouched and the only thing in the chain is the pedal.
    const auto set = [&processor](const char* id, float value)
    {
        if (auto* parameter = processor.getParameters().getParameter(id))
            parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
    };
    set("engineMode", 1.0f);
    using PedalControl = TubeForgeAudioProcessor::PedalControl;
    set(TubeForgeAudioProcessor::pedalParameterId(0, PedalControl::mix), 100.0f);

    juce::MidiBuffer midi;
    const auto render = [&processor, &midi]
    {
        juce::AudioBuffer<float> buffer(2, 64);
        auto peak = 0.0f;
        for (int block = 0; block < 64; ++block)
        {
            for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
                for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
                    buffer.setSample(channel, sample,
                                     0.1f * std::sin(2.0f * 3.14159265f * 196.0f
                                                     * static_cast<float>(block * 64 + sample) / 48000.0f));
            processor.processBlock(buffer, midi);
            if (block >= 48) peak = std::max(peak, buffer.getMagnitude(0, 0, buffer.getNumSamples()));
        }
        return peak;
    };

    set(TubeForgeAudioProcessor::pedalParameterId(0, PedalControl::kind), 0.0f);
    const auto withoutCapture = render();
    set(TubeForgeAudioProcessor::pedalParameterId(0, PedalControl::kind), 6.0f);
    const auto withCapture = render();

    tests.expect(std::isfinite(withCapture) && withCapture > 0.0f,
                 "a loaded capture produces audio rather than silence");
    tests.expect(std::abs(withCapture - withoutCapture) > withoutCapture * 0.01f,
                 "a loaded capture changes the signal, so the model is actually in the chain");

    workspace.deleteRecursively();
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

    /* An editor built while Auto Match is holding a rig, then ticked and drawn.

       Not an assertion about how any of it looks -- that is a listening-and-looking question a
       test cannot answer. What it does cover is that the badge, the released-control marking and
       the warning poll all run against real held state, on a real timer, and render: three code
       paths that only execute when a rig is held and that no other test reaches. Before this,
       the whole Auto Match interface was exercised only through the processor. */
    const auto rig = nts::amp::makeOriginalPreset(nts::amp::Topology::vintageBloom,
                                                  nts::amp::Instrument::guitar).parameters;
    processor.setAutoMatchEnabled(true);
    processor.applyRecoveredRig(rig, true);
    processor.releaseAutoMatchParameter("presence");
    tests.expect(processor.autoMatchState() == tf::automatch::State::overridden,
                 "the editor is opened onto a partly overridden rig");

    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    tests.expect(editor != nullptr, "the editor opens while a rig is held");
    if (editor != nullptr)
    {
        editor->setSize(1240, 820);
        // Two ticks at 20 Hz, so the badge and the marking are pushed and the poll runs.
        juce::MessageManager::getInstance()->runDispatchLoopUntil(120);
        juce::Image rendered(juce::Image::ARGB, editor->getWidth(), editor->getHeight(), true);
        juce::Graphics graphics(rendered);
        editor->paintEntireComponent(graphics, true);
        tests.expect(processor.autoMatchState() == tf::automatch::State::overridden,
                     "ticking and drawing the editor does not disturb what is held");
        tests.expect(processor.autoMatchReleased("presence"),
                     "the released control is still released after a full tick and paint");
    }
    processor.setAutoMatchEnabled(false);
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
    /* The circuit engine no longer carries a speaker of its own.

       It used to synthesise a cabinet node with its own resonance/brightness pair, while the
       impulse-response cabinet lived inside the traditional amplifier -- so the two engines had
       different speakers and a loaded response did nothing on this one. The cabinet is now a
       stage after all three engines, and a node here as well would put two cabinets in series.

       Asserted rather than merely dropped, because "the graph has no cabinet in it" is the whole
       claim that makes the shared stage correct. `circuitCabinetStyle` is set above and is
       deliberately still settable: removing the parameter would shift every id after it in
       `ampControlIds` and corrupt every saved project. */
    tests.expect(cabinet == edited.nodes.end(),
                 "the circuit graph carries no cabinet: the shared stage is the only speaker");

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
    /* Sixty-four blocks, and the number is not arbitrary.

       `bypassMix` restarts its ramp every block -- `setTarget` is called unconditionally in
       processBlock and resets `remaining` each time -- so it approaches unity *geometrically*
       rather than arriving at it: each 128-sample block closes about 13% of what is left of the
       20 ms ramp. This settle used to be sixteen blocks, which leaves roughly a tenth of the wet
       signal still mixed in, and the test passed anyway because the residual happened to be small
       enough against the old synthetic cabinet.

       That is a test measuring the wrong thing and getting away with it. The moment the cabinet
       became a real model -- with a real group delay, so its residual is *phase-shifted* against
       the dry path rather than roughly on top of it -- the same 10% leak read as a three-sample
       misalignment. Sixty-four blocks puts the residual near 1e-4, which is what "bypass is
       transparent" has to mean if the assertion is to be worth making. */
    captured.clear();
    run(64, -1, nullptr, 0.0f);
    auto largestError = 0.0f;
    const auto latency = processor.getLatencySamples();
    for (int sample = 0; sample < blockSize; ++sample)
    {
        const auto expectedPhase = phase - blockSize + sample - latency;
        const auto expected = 0.05f * std::sin(6.2831853f * frequency
                                               * static_cast<float>(expectedPhase) / 48000.0f);
        const auto actual = captured[captured.size() - blockSize + static_cast<std::size_t>(sample)];
        largestError = std::max(largestError, std::abs(actual - expected));
    }
    /* The error is reported alongside the offset that would remove it, not merely compared.

       When this fails there are only two candidates -- the dry path is delayed by a different
       number of samples than the plug-in reported, or the bypassed output is not the dry path at
       all -- and they need different fixes. A best-fit offset of zero with a large error means the
       second; a non-zero offset names the first and says by how much. */
    auto bestOffset = 0;
    auto bestOffsetError = largestError;
    for (int offset = -8; offset <= 8; ++offset)
    {
        auto error = 0.0f;
        for (int sample = 0; sample < blockSize; ++sample)
        {
            const auto expectedPhase = phase - blockSize + sample - latency - offset;
            const auto expected = 0.05f * std::sin(6.2831853f * frequency
                                                   * static_cast<float>(expectedPhase) / 48000.0f);
            const auto actual = captured[captured.size() - blockSize + static_cast<std::size_t>(sample)];
            error = std::max(error, std::abs(actual - expected));
        }
        if (error < bestOffsetError) { bestOffsetError = error; bestOffset = offset; }
    }
    tests.expect(largestError < 2.0e-3f,
                 "bypass passes the input through delayed by the reported latency of "
                     + std::to_string(latency) + " samples: largest error "
                     + std::to_string(largestError) + ", best at offset "
                     + std::to_string(bestOffset) + " with error " + std::to_string(bestOffsetError));

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
// Writes an artifact directory in the layout `nts_ml.export` produces, with a packed WaveNet of the
// kind `nts-nam-import` emits from a Neural Amp Modeler capture. The expected output is rendered by
// the runtime itself: numerical parity with the Python renderer is the job of nts_ml_runtime_parity,
// whereas what this exercises is the loader -- schema version, digest, file presence, staging.
bool writeNeuralArtifact(const juce::File& directory, int manifestVersion = 3,
                         bool corruptDigest = false)
{
    const auto vectors = directory.getChildFile("test-vectors");
    if (! vectors.createDirectory()) return false;
    const auto packed = nts::test::wavenetFixture(3);
    nts::ml::PackedWaveNetModel model;
    std::string error;
    if (! model.load(packed, error)) return false;

    std::vector<float> input(2048), expected(2048);
    for (std::size_t index = 0; index < input.size(); ++index)
        input[index] = 0.08f * std::sin(static_cast<float>(index) * 0.13f);
    model.reset();
    if (! model.process(input, expected)) return false;

    const auto writeFloats = [](const juce::File& file, const std::vector<float>& values)
    {
        return file.replaceWithData(values.data(), values.size() * sizeof(float));
    };
    if (! directory.getChildFile("model.bin").replaceWithData(packed.data(), packed.size())) return false;
    if (! writeFloats(vectors.getChildFile("input.f32"), input)) return false;
    if (! writeFloats(vectors.getChildFile("output.f32"), expected)) return false;
    vectors.getChildFile("metadata.json").replaceWithText(
        R"({"maximumAbsoluteErrorTolerance": 1.0e-4, "samples": 2048})");
    directory.getChildFile("normalization.json").replaceWithText(
        R"({"inputRmsDb": -23.4, "inputScale": 1.0, "outputScale": 1.0, "dcOffset": 0.0})");
    auto digest = juce::SHA256(packed.data(), packed.size()).toHexString();
    if (corruptDigest) digest = digest.replaceSection(0, 1, digest.startsWith("a") ? "b" : "a");
    directory.getChildFile("manifest.json").replaceWithText(
        "{\"modelFormatVersion\": " + juce::String(manifestVersion)
        + ", \"architecture\": \"wavenet\", \"sampleRate\": 48000, \"inputChannels\": 1,"
          " \"outputChannels\": 1, \"stateSize\": 5, \"latencySamples\": 0,"
          " \"expectedInputRmsDb\": -23.4, \"parameterSchema\": [], \"sha256\": \""
        + digest + "\"}");
    return true;
}

void testNeuralArtifactLoading(TestHarness& tests)
{
    constexpr int blockSize = 128;
    constexpr double sampleRate = 48000.0;
    const auto root = juce::File::createTempFile("tubeforge-neural-artifacts");
    root.deleteFile();
    tests.expect(root.createDirectory(), "temporary artifact root can be created");

    const auto await = [](TubeForgeAudioProcessor& processor, const juce::String& expected)
    {
        for (int attempt = 0; attempt < 200; ++attempt)
        {
            if (processor.neuralModelStatusText().contains(expected)) return true;
            juce::Thread::sleep(10);
        }
        return false;
    };

    {
        const auto artifact = root.getChildFile("wavenet");
        tests.expect(writeNeuralArtifact(artifact), "a packed WaveNet artifact can be written");
        TubeForgeAudioProcessor processor;
        processor.setPlayConfigDetails(2, 2, sampleRate, blockSize);
        processor.prepareToPlay(sampleRate, blockSize);
        processor.requestNeuralModelLoad(artifact);
        tests.expect(await(processor, "Ready"),
                     "a converted NAM capture loads through the artifact path: "
                         + processor.neuralModelStatusText().toStdString());
        juce::AudioBuffer<float> buffer(2, blockSize);
        juce::MidiBuffer midi;
        buffer.clear();
        for (int sample = 0; sample < blockSize; ++sample)
            buffer.setSample(0, sample, 0.1f * std::sin(0.21f * static_cast<float>(sample)));
        processor.processBlock(buffer, midi);
        auto finite = true;
        for (int sample = 0; sample < blockSize; ++sample)
            finite = finite && std::isfinite(buffer.getSample(0, sample));
        tests.expect(finite, "audio through a loaded WaveNet stays finite");
    }
    {
        // The guard that matters: a schema this build does not know must be refused, not guessed at.
        const auto artifact = root.getChildFile("future");
        tests.expect(writeNeuralArtifact(artifact, 4), "a future-schema artifact can be written");
        TubeForgeAudioProcessor processor;
        processor.prepareToPlay(sampleRate, blockSize);
        processor.requestNeuralModelLoad(artifact);
        tests.expect(await(processor, "unsupported"), "an unknown manifest schema is refused: "
                                                          + processor.neuralModelStatusText().toStdString());
    }
    {
        const auto artifact = root.getChildFile("tampered");
        tests.expect(writeNeuralArtifact(artifact, 3, true), "a tampered artifact can be written");
        TubeForgeAudioProcessor processor;
        processor.prepareToPlay(sampleRate, blockSize);
        processor.requestNeuralModelLoad(artifact);
        tests.expect(await(processor, "SHA-256"), "a model that does not match its digest is refused: "
                                                      + processor.neuralModelStatusText().toStdString());
    }
    root.deleteRecursively();
}

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
    // The tuner and assistant feeds are only filled while an editor exists to drain them, so a
    // test that ticks the shell has to declare the shell open as well.
    processor.setEditorActive(true);

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

    // The display feeds are gated on an editor existing, which saves the tuner decimation and
    // two full-buffer measurements per block whenever the window is closed -- most of a mixing
    // session. That gate is only legitimate if it is genuinely display-only, so this renders the
    // same signal both ways and requires the audio to be bit-identical.
    const auto renderWithEditor = [&](bool editorOpen)
    {
        TubeForgeAudioProcessor rendered;
        rendered.setPlayConfigDetails(2, 2, sampleRate, blockSize);
        rendered.prepareToPlay(sampleRate, blockSize);
        rendered.setEditorActive(editorOpen);
        juce::AudioBuffer<float> renderBuffer(2, blockSize);
        juce::MidiBuffer renderMidi;
        std::vector<float> captured;
        int renderPhase {};
        for (int block = 0; block < 24; ++block)
        {
            for (int sample = 0; sample < blockSize; ++sample)
            {
                const auto value = 0.3f * std::sin(6.2831853f * 110.0f
                    * static_cast<float>(renderPhase + sample) / static_cast<float>(sampleRate));
                renderBuffer.setSample(0, sample, value);
                renderBuffer.setSample(1, sample, value);
            }
            renderPhase += blockSize;
            rendered.processBlock(renderBuffer, renderMidi);
            for (int sample = 0; sample < blockSize; ++sample)
                captured.push_back(renderBuffer.getSample(0, sample));
        }
        return captured;
    };
    const auto withEditor = renderWithEditor(true);
    const auto withoutEditor = renderWithEditor(false);
    auto identical = withEditor.size() == withoutEditor.size();
    for (std::size_t index = 0; index < withEditor.size() && identical; ++index)
        identical = withEditor[index] == withoutEditor[index];
    tests.expect(identical, "closing the editor changes no audio, only what the display feeds cost");
}

/// The performance tier is the only lever left once micro-optimisation runs out, so what it
/// actually enforces has to be pinned down rather than assumed from its documentation.
void testPerformanceTier(TestHarness& tests)
{
    constexpr int blockSize = 128;
    constexpr double sampleRate = 48000.0;

    TubeForgeAudioProcessor processor;
    processor.setPlayConfigDetails(2, 2, sampleRate, blockSize);
    processor.prepareToPlay(sampleRate, blockSize);

    const auto selectTier = [&](int tier)
    {
        if (auto* parameter = processor.getParameters().getParameter("performanceTier"))
            parameter->setValueNotifyingHost(parameter->convertTo0to1(static_cast<float>(tier)));
    };
    // Ask for the most expensive settings the plug-in offers, so every cap has something to bite.
    const auto request = [&](const char* id, float value)
    {
        if (auto* parameter = processor.getParameters().getParameter(id))
            parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
    };
    request("oversampling", 3.0f);   // 8x
    request("cabinetBlend", 50.0f);  // both cabinets contributing

    selectTier(2);
    tests.expect(processor.performanceTier() == TubeForgeAudioProcessor::PerformanceTier::studio,
                 "the tier parameter reads back as the selected tier");
    const auto studio = processor.tierLimits();
    tests.expectEqual(studio.maximumOversamplingFactor, 8, "Studio lifts the oversampling ceiling");
    tests.expect(! studio.singleCabinet && ! studio.approximateNonlinearities,
                 "Studio takes neither the single-cabinet nor the approximation shortcut");

    selectTier(1);
    const auto standard = processor.tierLimits();
    tests.expectEqual(standard.maximumOversamplingFactor, 2, "Standard caps oversampling at 2x");
    tests.expect(standard.assistantMetering, "Standard keeps the assistant fed");

    selectTier(0);
    const auto eco = processor.tierLimits();
    tests.expectEqual(eco.maximumOversamplingFactor, 1, "Eco pins oversampling to 1x");
    tests.expect(eco.singleCabinet && eco.forceNeuralMonoCollapse && ! eco.assistantMetering,
                 "Eco runs one cabinet, collapses neural stereo, and drops assistant metering");
    tests.expect(eco.maximumImpulseTaps < studio.maximumImpulseTaps,
                 "Eco truncates impulse responses well below Studio");

    // The caps have to reach the audio path, not merely be reported. Oversampling latency is the
    // observable proof: 1x reports none, anything above reports the anti-alias group delay.
    juce::AudioBuffer<float> buffer(2, blockSize);
    juce::MidiBuffer midi;
    const auto latencyAfterSettling = [&]
    {
        for (int block = 0; block < 64; ++block)
        {
            for (int sample = 0; sample < blockSize; ++sample)
            {
                const auto value = 0.3f * std::sin(0.06f * static_cast<float>(sample));
                buffer.setSample(0, sample, value); buffer.setSample(1, sample, value);
            }
            processor.processBlock(buffer, midi);
        }
        // Latency is republished through triggerAsyncUpdate, so it only reaches the host-facing
        // value once the message queue runs -- the same reason the program-change tests pump it.
        juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
        return processor.getLatencySamples();
    };
    const auto ecoLatency = latencyAfterSettling();
    selectTier(2);
    const auto studioLatency = latencyAfterSettling();
    tests.expect(ecoLatency < studioLatency,
                 "the Eco oversampling cap reaches the audio path and removes its latency");

    // And the tier must not wipe the user's control: raising it restores what they asked for.
    tests.expectEqual(static_cast<int>(std::lround(
        processor.getParameters().getRawParameterValue("oversampling")->load())), 3,
        "the tier caps the oversampling factor without rewriting the saved control");

    auto finite = true;
    for (int channel = 0; channel < 2; ++channel)
        for (int sample = 0; sample < blockSize; ++sample)
            finite = finite && std::isfinite(buffer.getSample(channel, sample));
    tests.expect(finite, "audio stays finite across tier changes");

    // The shell's tier selector attaches to the parameter by string id, and a ComboBoxAttachment
    // against an id that does not exist asserts rather than failing quietly -- so constructing
    // the editor is the check that the control is wired to something real. Doing it at each tier
    // also runs applyPerformanceTierVisibility down every page.
    for (const auto tier : { 0, 1, 2 })
    {
        selectTier(tier);
        std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
        tests.expect(editor != nullptr, "the editor builds with the performance tier selector");
        juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
    }
    // And the tier the editor last wrote is the one that survives: the selector must not have
    // pushed a value of its own on construction.
    tests.expectEqual(static_cast<int>(processor.performanceTier()), 2,
                      "building the editor does not disturb the selected tier");

    {
        /* Nothing in the header rail may overlap anything else in it, at any window width.

           The rail's left cluster was a fixed 372 px holding 526 px of cells, so `removeFromLeft`
           ran dry partway along: the Performance selector was clamped to two thirds of its width,
           and the switch column after it was built by `withSizeKeepingCentre` from an *empty*
           rectangle, which grew it back around the cluster edge and drew Bypass and Pro Mode on top
           of that selector. It was plainly visible in a screenshot and no test saw it, because the
           editor was only ever checked for constructing rather than for laying out.

           Checked at the extremes of the resize range: the narrowest width is where a fixed-pixel
           rail runs out of room, and the widest is where a centred cell can drift out of its
           cluster. Sibling components only -- a caption sitting inside its own cell's rectangle is
           the layout working, not a collision. */
        std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
        if (editor == nullptr) { tests.expect(false, "the editor builds for the layout check"); return; }

        {
            /* Every slider and knob has to explain itself on hover.

               Checked by walking the built editor rather than by counting call sites, so a control
               added to any page later is covered without anyone remembering to extend a list. The
               editor is opened on each module in turn because pages build their controls in their
               constructors but only the visible page is laid out -- an unvisited page still has its
               components, which is all this needs.

               Sliders only. Buttons and combo boxes carry tooltips where they earn one, but a
               labelled toggle usually says what it does in its own text, and demanding a tooltip on
               all of them would be enforcing noise. */
            std::function<void(juce::Component&, std::vector<juce::Slider*>&)> collect =
                [&collect](juce::Component& parent, std::vector<juce::Slider*>& found)
            {
                for (auto* child : parent.getChildren())
                {
                    if (child == nullptr) continue;
                    if (auto* slider = dynamic_cast<juce::Slider*>(child)) found.push_back(slider);
                    collect(*child, found);
                }
            };

            editor->setSize(1240, 820);
            juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
            std::vector<juce::Slider*> sliders;
            collect(*editor, sliders);
            // 48 at the time of writing, across every page. The floor is a guard against the walk
            // silently stopping at the header if pages ever stop being built as children.
            tests.expect(sliders.size() >= 40,
                         "the editor exposes sliders to check: " + std::to_string(sliders.size()));
            std::size_t missing {};
            for (auto* slider : sliders)
                if (slider->getTooltip().isEmpty())
                {
                    ++missing;
                    tests.expect(false, "a slider has no tooltip describing what it does: '"
                                            + slider->getName().toStdString() + "'");
                }
            tests.expectEqual(missing, std::size_t { 0 }, "every slider explains itself on hover");
        }

        for (const auto width : { 1040, 1240, 1800 })
        {
            editor->setSize(width, 820);
            juce::MessageManager::getInstance()->runDispatchLoopUntil(20);

            // The rail is the top strip; everything laid out inside it is a direct child of the
            // editor, so comparing direct children with a y inside that strip catches the cluster
            // collisions without dragging the page host's contents in.
            constexpr int railBottom = 150;
            std::vector<std::pair<juce::Component*, juce::Rectangle<int>>> railChildren;
            for (auto* child : editor->getChildren())
            {
                if (child == nullptr || ! child->isVisible()) continue;
                const auto bounds = child->getBounds();
                if (bounds.isEmpty() || bounds.getY() >= railBottom) continue;
                railChildren.emplace_back(child, bounds);
            }
            tests.expect(railChildren.size() > 4,
                         "the rail has components to check at " + std::to_string(width) + " px");

            // The specific collision, asserted by name and position rather than only caught by the
            // sweep below. Bypass must begin to the right of where the Performance selector ends,
            // and that selector must not have been clamped narrower than its sibling -- the two
            // symptoms of the cluster running out of room. Named components, so this survives the
            // cells being rearranged as long as the relationship holds.
            juce::Rectangle<int> performanceBounds, bypassBounds, engineBounds;
            for (const auto& [component, bounds] : railChildren)
            {
                if (component->getName() == "BYPASS") bypassBounds = bounds;
                if (auto* box = dynamic_cast<juce::ComboBox*>(component))
                {
                    // Engine is laid out first, so it is the leftmost of the two selectors.
                    if (engineBounds.isEmpty() || bounds.getX() < engineBounds.getX())
                    {
                        if (! engineBounds.isEmpty()) performanceBounds = engineBounds;
                        engineBounds = bounds;
                    }
                    else if (performanceBounds.isEmpty() || bounds.getX() > performanceBounds.getX())
                        performanceBounds = bounds;
                }
            }
            if (! performanceBounds.isEmpty() && ! bypassBounds.isEmpty())
                tests.expect(bypassBounds.getX() >= performanceBounds.getRight(),
                             "the switch column starts clear of the Performance selector at "
                                 + std::to_string(width) + " px: bypass x="
                                 + std::to_string(bypassBounds.getX()) + " vs selector right="
                                 + std::to_string(performanceBounds.getRight()));
            if (! performanceBounds.isEmpty() && ! engineBounds.isEmpty())
                tests.expect(performanceBounds.getWidth() >= engineBounds.getWidth(),
                             "the Performance selector is not clamped narrower than the Engine one at "
                                 + std::to_string(width) + " px: "
                                 + std::to_string(performanceBounds.getWidth()) + " vs "
                                 + std::to_string(engineBounds.getWidth()));

            for (std::size_t first = 0; first < railChildren.size(); ++first)
                for (std::size_t second = first + 1; second < railChildren.size(); ++second)
                {
                    const auto overlap = railChildren[first].second
                                             .getIntersection(railChildren[second].second);
                    // A couple of pixels of shared edge is a rounding artefact of centring; a
                    // control sitting on another one is not.
                    const auto area = overlap.getWidth() * overlap.getHeight();
                    if (area > 4)
                        tests.expect(false, "rail components overlap at " + std::to_string(width)
                                                + " px: " + railChildren[first].first->getName().toStdString()
                                                + " over " + railChildren[second].first->getName().toStdString()
                                                + " by " + std::to_string(area) + " px2");
                }
        }
    }

    // Automatic reduction must not fire on a machine that is coping. A tier that quietly drops
    // during ordinary playback would change the user's tone for no reason, and it is the failure
    // mode a load-watching heuristic has -- so the no-op case is the one worth pinning.
    selectTier(2);
    for (int block = 0; block < 400; ++block)
    {
        for (int sample = 0; sample < blockSize; ++sample)
        {
            const auto value = 0.2f * std::sin(0.05f * static_cast<float>(sample));
            buffer.setSample(0, sample, value); buffer.setSample(1, sample, value);
        }
        processor.processBlock(buffer, midi);
    }
    juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
    tests.expectEqual(static_cast<int>(processor.performanceTier()), 2,
                      "the tier is not reduced automatically while the callback keeps up");
}

/// Programs and MIDI program change: how a rig gets switched from a foot controller.
void testProgramsAndMidi(TestHarness& tests)
{
    constexpr int blockSize = 128;
    TubeForgeAudioProcessor processor;
    processor.setPlayConfigDetails(2, 2, 48000.0, blockSize);
    processor.prepareToPlay(48000.0, blockSize);

    auto& base = static_cast<juce::AudioProcessor&>(processor);
    // Derived from the enum rather than written down, so appending a voicing does not fail a
    // test that is only encoding the old count. What is being asserted is the *packing* --
    // every instrument crossed with every topology -- not the number seven.
    const auto topologies = static_cast<int>(nts::amp::topologyCount);
    tests.expectEqual(base.getNumPrograms(), 2 * topologies,
                      "every instrument x topology pair is exposed as a program");
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
    const auto topologyOf = [&processor]
    {
        auto* parameter = processor.getParameters().getParameter("topology");
        return parameter == nullptr ? -1.0f : parameter->convertFrom0to1(parameter->getValue());
    };
    // The last program is the last topology on bass, which is the pair that would break first
    // if the packing and the table size ever disagreed.
    base.setCurrentProgram(base.getNumPrograms() - 1);
    tests.expectNear(instrumentOf(), 1.0, 0.01, "a bass program selects the bass instrument");
    tests.expectNear(topologyOf(), static_cast<double>(topologies - 1), 0.01,
                     "the last program selects the last topology");
    base.setCurrentProgram(topologies - 1);
    tests.expectNear(instrumentOf(), 0.0, 0.01,
                     "the program before the bass half is still on guitar");

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
    // The tuner and assistant feeds are only filled while an editor exists to drain them, so a
    // test that ticks the shell has to declare the shell open as well.
    processor.setEditorActive(true);

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
/** The gear picker's contract with the parameters it drives.

    Three properties, and the third is the one that matters most: every entry of a choice
    parameter has to appear in exactly one tile. A voicing or a pedal kind added to the engine
    and forgotten in the catalogue would be silently unreachable from the interface -- there is
    a `jassert` guarding the count, but `jassert` is compiled out of the shipping build, which
    is exactly where an unreachable amplifier would matter.

    Driven through the built catalogues rather than through the widget: the picker is a painted
    grid with no child components, so there is nothing to click in a headless test. What can be
    checked is that the data the widget is built from is complete and that the commit path
    writes what it says it writes.
*/
void testGearPickerCatalogues(TestHarness& tests)
{
    // Both catalogues are built from the same tables the pages use, so this is the real data
    // rather than a copy of it.
    const auto ampTiles = tf::ui::faceplateStyleCount();
    const auto pedalTiles = nts::pedals::modelCount();

    TubeForgeAudioProcessor processor;
    auto& state = processor.getParameters();

    const auto choiceCount = [&state](const juce::String& id)
    {
        const auto* choice = dynamic_cast<const juce::AudioParameterChoice*>(state.getParameter(id));
        return choice == nullptr ? -1 : choice->choices.size();
    };

    tests.expectEqual(static_cast<int>(ampTiles), choiceCount("topology"),
                      "every topology the parameter offers has a faceplate to show for it");
    tests.expectEqual(static_cast<int>(pedalTiles),
                      choiceCount(TubeForgeAudioProcessor::pedalParameterId(
                          0, TubeForgeAudioProcessor::PedalControl::kind)),
                      "every pedal kind the parameter offers has a face to show for it");

    // Each amp voicing is filed under exactly one character, and each character label is real.
    auto ampShelvesValid = true;
    for (std::size_t index = 0; index < ampTiles; ++index)
    {
        const auto style = tf::ui::faceplateStyle(static_cast<int>(index), 0);
        const auto character = static_cast<std::size_t>(style.character);
        ampShelvesValid = ampShelvesValid && character < tf::ui::ampCharacterCount
                       && tf::ui::ampCharacterName(style.character).isNotEmpty()
                       && style.blurb.isNotEmpty();
    }
    tests.expect(ampShelvesValid, "every voicing has a real shelf and a line describing it");

    /* The instrument filter hides tiles from a view; it never renumbers a parameter.

       This is the one place the two could quietly disagree. The picker skips bass-native voicings
       for a guitarist, and that is only safe because a tile carries the index it writes -- the
       moment anything derives a value from a tile's *position*, the eighth tile starts writing 7
       in one instrument and 8 in the other, and nothing reports it. The same trap is why the
       topology combo box on the tone page does not take a `ComboBoxAttachment`, which maps by
       position rather than by item id.

       So: the parameter must keep offering every voicing regardless of instrument, and the set
       hidden from guitar must be exactly the bass-native block.
    */
    auto affinityPartitions = true;
    auto hiddenFromGuitar = 0;
    for (std::size_t index = 0; index < ampTiles; ++index)
    {
        const auto bassNative = nts::amp::topologyAffinity(static_cast<nts::amp::Topology>(index))
                             == nts::amp::TopologyAffinity::bass;
        if (bassNative) ++hiddenFromGuitar;
        // Every voicing answers for both instruments even when it is only offered for one, so a
        // project or an automation lane naming an out-of-affinity pair still gets an amplifier.
        affinityPartitions = affinityPartitions
            && ! nts::amp::makeOriginalPreset(static_cast<nts::amp::Topology>(index),
                                              nts::amp::Instrument::guitar).name.empty()
            && ! nts::amp::makeOriginalPreset(static_cast<nts::amp::Topology>(index),
                                              nts::amp::Instrument::bass).name.empty();
    }
    tests.expect(affinityPartitions, "every voicing returns a real preset for both instruments");
    tests.expect(hiddenFromGuitar > 0 && hiddenFromGuitar < static_cast<int>(ampTiles),
                 "the instrument filter hides some voicings from guitar and keeps others");
    tests.expectEqual(static_cast<int>(ampTiles), choiceCount("topology"),
                      "the topology parameter offers every voicing whatever the instrument is");

    /* The normalised round trip the hand-driven combo box depends on.

       Without an attachment, the tone page converts its selected item id back to a topology index
       and writes it through `convertTo0to1`. If that conversion is off by one anywhere -- which is
       exactly the sort of thing that happens when a choice parameter's option count changes -- the
       box selects one amplifier and the engine loads its neighbour, quietly, for every voicing
       past the discrepancy.
    */
    auto normalisedRoundTrips = true;
    if (auto* topology = state.getParameter("topology"))
        for (std::size_t index = 0; index < ampTiles; ++index)
        {
            const auto value = static_cast<float>(index);
            normalisedRoundTrips = normalisedRoundTrips
                && std::abs(topology->convertFrom0to1(topology->convertTo0to1(value)) - value) < 0.01f;
        }
    tests.expect(normalisedRoundTrips,
                 "every topology index survives the normalised conversion the combo box uses");

    /* The editor builds for both instruments.

       `testEditorLifecycle` builds it 250 times and never moves the instrument, so it exercises
       the guitar branch of the filtered topology list and nothing else. The bass branch shows six
       more voicings and is the one the filter was written for.
    */
    auto editorsBuild = true;
    for (const auto instrument : { 0.0f, 1.0f })
    {
        if (auto* parameter = state.getParameter("instrument"))
            parameter->setValueNotifyingHost(parameter->convertTo0to1(instrument));
        std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
        editorsBuild = editorsBuild && editor != nullptr;
        if (editor != nullptr) editor->setSize(900, 620);
    }
    tests.expect(editorsBuild, "the editor builds with the voicing list filtered for either instrument");

    /* And on a voicing that carries front-panel switches.

       The amplifier page builds its switch list from `panelSwitchAppliesTo` and hides the control
       on the eleven voicings that offer nothing, so the default voicing exercises only the empty
       branch. Selecting one that has switches is what runs the other.
    */
    auto switchedEditorBuilds = true;
    for (std::size_t index = 0; index < nts::amp::topologyCount; ++index)
    {
        auto carriesSwitches = false;
        for (std::size_t option = 1; option < nts::amp::panelSwitchCount; ++option)
            carriesSwitches = carriesSwitches
                || nts::amp::panelSwitchAppliesTo(static_cast<nts::amp::Topology>(index),
                                                  static_cast<nts::amp::PanelSwitch>(option));
        if (! carriesSwitches) continue;
        if (auto* parameter = state.getParameter("topology"))
            parameter->setValueNotifyingHost(parameter->convertTo0to1(static_cast<float>(index)));
        std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
        switchedEditorBuilds = switchedEditorBuilds && editor != nullptr;
        if (editor != nullptr) editor->setSize(900, 620);
    }
    tests.expect(switchedEditorBuilds, "the editor builds on a voicing that carries panel switches");

    const auto readBack = [&state](const juce::String& id)
    {
        const auto* value = state.getRawParameterValue(id);
        return value == nullptr ? 0.0f : value->load(std::memory_order_relaxed);
    };

    /* Reset actually returns the amplifier to the selected voicing.

       This is the gap a player found by ear before any test did. A voicing supplies a complete
       parameter set, but only the values with no host control ever reach the engine from it —
       everything a knob owns is taken from the knob. So a rig left at a previous session's
       settings keeps them when a new amplifier is chosen, and there was no action anywhere that
       put them back.

       `tightness` is asserted specifically because it is the one that does real damage: it sets a
       high-pass floor of `35 + 310 * t^2` Hz, so the *parameter default* of 7.2 is a 196 Hz cut
       that removes the fundamental of every bass note below G3 and leaves its harmonics to be
       distorted alone. Resetting to parameter defaults would not have fixed this; only the
       voicing's own value does.
    */
    for (const auto instrument : { 0.0f, 1.0f })
    {
        const auto isBass = instrument > 0.5f;
        const auto topologyIndex = isBass ? static_cast<int>(nts::amp::Topology::valveFlagship) : 0;
        const auto setValue = [&state](const char* id, float value)
        {
            if (auto* parameter = state.getParameter(id))
                parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
        };
        setValue("instrument", instrument);
        setValue("topology", static_cast<float>(topologyIndex));

        // Everything a player could have left maxed, which is exactly what was found in the wild.
        setValue("gain", 10.0f); setValue("master", 12.0f); setValue("treble", 10.0f);
        setValue("presence", 10.0f); setValue("tightness", 10.0f); setValue("lowCut", 300.0f);
        processor.loadVoicingDefaults();

        const auto factory = nts::amp::makeOriginalPreset(
            static_cast<nts::amp::Topology>(topologyIndex),
            isBass ? nts::amp::Instrument::bass : nts::amp::Instrument::guitar).parameters;
        const auto label = juce::String(isBass ? "bass" : "guitar");
        tests.expectNear(readBack("gain"), 5.0f, 0.01f,
                         ("reset centres Gain so the stage drives are the voicing's own (" + label + ")").toStdString());
        tests.expectNear(readBack("tightness"), factory.preEq.tightness * 10.0f, 0.05f,
                         ("reset restores the voicing's tightness (" + label + ")").toStdString());
        tests.expectNear(readBack("lowCut"), factory.preEq.lowCutHz, 0.5f,
                         ("reset restores the voicing's low cut (" + label + ")").toStdString());
        tests.expectNear(readBack("master"), factory.powerAmp.masterDb, 0.05f,
                         ("reset restores the voicing's master level (" + label + ")").toStdString());
        tests.expectNear(readBack("treble"), factory.toneStack.treble * 10.0f, 0.05f,
                         ("reset restores the voicing's treble (" + label + ")").toStdString());
        // And it keeps what the player chose rather than jumping to a different amplifier.
        tests.expectNear(readBack("topology"), static_cast<float>(topologyIndex), 0.01f,
                         ("reset keeps the selected voicing (" + label + ")").toStdString());
        tests.expectNear(readBack("instrument"), instrument, 0.01f,
                         ("reset keeps the selected instrument (" + label + ")").toStdString());
    }

    /* A project saved before the bi-amp controls existed still lands in the right parameters.

       `applyProjectState` walks a saved `ampControls` array **positionally** against
       `ampControlIds`, so the table is append-only or it is nothing: four ids added in the middle
       shift every id after them, and an old project's Cab alignment arrives in Dry blend, its
       Tightness in Low drive, and so on to the end of the list.

       This is not hypothetical. It is what the first version of that change did, and **every test
       in this suite passed while it was broken** -- a positional remap produces perfectly valid
       numbers in the wrong fields, so nothing is out of range, nothing is NaN and nothing fails.
       It was caught by launching the standalone and reading the panel.

       The fixture is a schema-4 project whose `ampControls` array is the length and order the
       build before this change wrote, with distinctive values at three positions well past where
       the new ids were briefly inserted. If any of them is displaced, the append-only rule has
       been broken again.
    */
    juce::String older = R"({"schemaVersion":4,"applicationVersion":"0.10.0",)"
                         R"("engine":{"inputGainDb":0.0,"outputGainDb":0.0,"bypass":false,"ampControls":[)";
    // Positions from the pre-change table: 20 crossover, 23 tightness, 24 pickEmphasis.
    constexpr int olderLength = 51;
    for (int index = 0; index < olderLength; ++index)
    {
        const auto value = index == 20 ? 412.0 : index == 23 ? 6.5 : index == 24 ? -7.5 : 0.0;
        older << (index == 0 ? "" : ",") << juce::String(value, 4);
    }
    older << R"(]},"device":{},"graph":{"physicalCircuitJson":""},"ui":{},)"
             R"("assets":{"relativePaths":[],"cabinetIrPathA":"","cabinetIrPathB":""},)"
             R"("pedalboard":{"slots":[]}})";

    const auto olderUtf8 = older.toStdString();
    processor.setStateInformation(olderUtf8.data(), static_cast<int>(olderUtf8.size()));
    tests.expectNear(readBack("crossover"), 412.0f, 1.0f,
                     "an older project's bass crossover is not displaced by the bi-amp controls");
    tests.expectNear(readBack("tightness"), 6.5f, 0.05f,
                     "an older project's tightness is not displaced by the bi-amp controls");
    tests.expectNear(readBack("pickEmphasis"), -7.5f, 0.05f,
                     "an older project's pick emphasis is not displaced by the bi-amp controls");
    // And the controls that project never knew about keep their defaults rather than picking up
    // whatever happened to sit at their position.
    tests.expectNear(readBack("dryBlend"), 0.0f, 0.01f,
                     "a control an older project predates loads at its default");
    tests.expectNear(readBack("lowBandDrive"), 2.6f, 0.05f,
                     "the low-band drive an older project predates loads at its default");

    auto pedalShelvesValid = true;
    for (std::size_t index = 0; index < pedalTiles; ++index)
    {
        const auto& face = tf::ui::pedalFace(static_cast<int>(index));
        const auto& model = nts::pedals::pedalModel(static_cast<int>(index));
        const auto character = static_cast<std::size_t>(face.character);
        pedalShelvesValid = pedalShelvesValid && character < tf::ui::pedalCharacterCount
                         && tf::ui::pedalCharacterName(face.character).isNotEmpty()
                         && ! model.name.empty() && ! model.blurb.empty();
    }
    tests.expect(pedalShelvesValid, "every pedal kind has a real shelf and a line describing it");

    // Exactly one kind is the pinned empty slot. Two would put a duplicate above the rail; none
    // would bury the way a slot is cleared one category deep.
    auto pinned = 0;
    for (std::size_t index = 0; index < pedalTiles; ++index)
        if (tf::ui::pedalFace(static_cast<int>(index)).pinned) ++pinned;
    tests.expectEqual(pinned, 1, "exactly one pedal kind is pinned above the shelves");

    /* The commit path. The picker writes its parameter through begin/setValueNotifyingHost/end
       rather than through an attachment, so what is checked here is that a host sees a gesture
       pair around the change -- a choice written without one shows up as an un-writable
       automation lane in several DAWs. */
    struct GestureSpy final : public juce::AudioProcessorParameter::Listener
    {
        void parameterValueChanged(int, float) override { ++values; }
        void parameterGestureChanged(int, bool starting) override
        {
            if (starting) ++begins; else ++ends;
        }
        int begins {}, ends {}, values {};
    };

    auto* topology = state.getParameter("topology");
    if (topology == nullptr) { tests.expect(false, "the topology parameter exists"); return; }

    GestureSpy spy;
    topology->addListener(&spy);
    // Exactly what GearChip's callback does, for the last voicing in the list.
    const auto target = static_cast<float>(tf::ui::faceplateStyleCount() - 1);
    topology->beginChangeGesture();
    topology->setValueNotifyingHost(topology->convertTo0to1(target));
    topology->endChangeGesture();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(30);
    topology->removeListener(&spy);

    tests.expectEqual(spy.begins, 1, "committing a choice opens exactly one gesture");
    tests.expectEqual(spy.ends, 1, "committing a choice closes the gesture it opened");
    tests.expectNear(topology->convertFrom0to1(topology->getValue()), static_cast<double>(target),
                     0.01, "committing a choice leaves the parameter on the chosen entry");
}

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
/** Auto Match: the applied rig is the rig that won, and the guard knows who moved a control.

    Reached through `applyRecoveredRig` rather than through `applyReconstructionCandidate`,
    because a candidate needs a whole reconstruction run behind it and none of what is checked
    here is about the search -- it is about what happens to the host parameters afterwards.
*/
void testAutoMatch(TestHarness& tests)
{
    TubeForgeAudioProcessor processor;
    processor.setPlayConfigDetails(2, 2, 48000.0, 32);
    processor.prepareToPlay(48000.0, 32);
    auto& parameters = processor.getParameters();

    const auto plainValue = [&parameters](const char* id)
    {
        const auto* value = parameters.getRawParameterValue(id);
        return value == nullptr ? 0.0f : value->load(std::memory_order_relaxed);
    };
    const auto setPlain = [&parameters](const char* id, float value)
    {
        if (auto* parameter = parameters.getParameter(id))
            parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
    };
    /// A knob drag, as an attachment performs one: gesture, value, gesture.
    const auto userMoves = [&parameters](const char* id, float value)
    {
        auto* parameter = parameters.getParameter(id);
        if (parameter == nullptr) return;
        parameter->beginChangeGesture();
        parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
        parameter->endChangeGesture();
    };

    // A rig that differs from every default, so nothing below can pass by coincidence.
    auto rig = nts::amp::makeOriginalPreset(nts::amp::Topology::britishCrunch,
                                            nts::amp::Instrument::guitar).parameters;
    rig.powerAmp.presence = 0.64f;
    rig.toneStack.bass = 0.31f;
    for (auto& stage : rig.stages) stage.driveDb = 17.0f;

    /* The Gain macro. `currentAmpParameters` adds (gain - 5) * 3 dB to every stage drive, so a
       rig applied over a user's Gain of 8 played 9 dB hotter than the one that was scored. The
       applied rig has to be independent of where Gain happened to be. */
    setPlain("gain", 8.5f);
    setPlain("panelSwitch", 1.0f);
    setPlain("oversampling", 0.0f);
    processor.applyRecoveredRig(rig, false);
    tests.expectNear(plainValue("gain"), 5.0f, 1.0e-3,
                     "applying a rig neutralises the Gain macro");
    tests.expectNear(plainValue("panelSwitch"), 0.0f, 1.0e-3,
                     "applying a rig clears a leftover panel switch");
    tests.expectNear(plainValue("oversampling"), 4.0f, 1.0e-3,
                     "applying a rig puts oversampling back on Auto");
    const auto firstStage = plainValue("stage1");

    setPlain("gain", 1.0f);
    processor.applyRecoveredRig(rig, false);
    tests.expectNear(plainValue("stage1"), firstStage, 1.0e-3,
                     "the applied rig does not depend on where Gain was left");

    // Nothing is guarded until the switch is on, however many rigs have been applied.
    tests.expect(processor.autoMatchState() == tf::automatch::State::off,
                 "Auto Match is off until it is asked for");
    tests.expect(! processor.autoMatchOwns("presence"), "nothing is owned while Auto Match is off");

    processor.setAutoMatchEnabled(true);
    processor.applyRecoveredRig(rig, false);
    tests.expect(processor.autoMatchState() == tf::automatch::State::holding,
                 "applying a rig with Auto Match on starts holding it");
    tests.expect(processor.autoMatchOwns("presence"), "the analyzer holds Presence");
    tests.expect(processor.autoMatchOwns("gateThreshold"), "the analyzer holds the Tone Shaping page");
    tests.expect(! processor.autoMatchOwns("output"),
                 "the output level is a level control and is never held");
    tests.expect(! processor.autoMatchOwns("pedal1Bypass"),
                 "the pedals are only held when the chain was isolated");
    tests.expectEqual(processor.takeAutoMatchWarning(), -1,
                      "applying a rig raises no warning about its own writes");

    // Recorded, and formatted the way the panel shows it.
    const auto matched = processor.autoMatchValueOf("presence");
    tests.expect(matched.has_value(), "the matched value of a held control is recorded");
    if (matched) tests.expectNear(*matched, plainValue("presence"), 1.0e-3,
                                  "the recorded value is the one that was written");

    // A person turning a knob: warned once, and only once.
    userMoves("presence", 2.0f);
    const auto warned = processor.takeAutoMatchWarning();
    tests.expect(warned >= 0 && tf::automatch::owned[static_cast<std::size_t>(warned)].id == "presence",
                 "a hand on a held control asks the question");
    tests.expectEqual(processor.takeAutoMatchWarning(), -1, "the question is asked once");

    // "Keep the matched value" puts it back and keeps holding it, and does not ask again about
    // its own write.
    processor.restoreAutoMatchParameter("presence");
    if (matched) tests.expectNear(plainValue("presence"), *matched, 1.0e-3,
                                  "keeping the matched value restores it");
    tests.expectEqual(processor.takeAutoMatchWarning(), -1,
                      "restoring a matched value does not warn about itself");
    tests.expect(processor.autoMatchState() == tf::automatch::State::holding,
                 "keeping the matched value leaves the rig fully held");

    // "Change it anyway" hands one control back and leaves the rest held.
    userMoves("presence", 2.0f);
    (void) processor.takeAutoMatchWarning();
    processor.releaseAutoMatchParameter("presence");
    tests.expect(processor.autoMatchReleased("presence"), "a released control reads as released");
    tests.expect(! processor.autoMatchOwns("presence"), "a released control is no longer held");
    tests.expect(processor.autoMatchOwns("bass"), "releasing one control holds the others");
    tests.expectEqual(processor.autoMatchReleasedCount(), 1, "one control has been handed back");
    tests.expect(processor.autoMatchState() == tf::automatch::State::overridden,
                 "a released control puts the rig in the overridden state");

    // Second and further moves of a released control are silent: it is the user's now.
    userMoves("presence", 7.0f);
    tests.expectEqual(processor.takeAutoMatchWarning(), -1,
                      "a control that has been handed back stops asking");

    processor.reclaimAllAutoMatchParameters();
    tests.expectEqual(processor.autoMatchReleasedCount(), 0, "reclaiming clears every release");
    if (matched) tests.expectNear(plainValue("presence"), *matched, 1.0e-3,
                                  "reclaiming puts the matched value back");

    /* A write with no gesture behind it is host automation, not a person. It must never raise a
       dialog -- a modal opened while a DAW plays back a lane is a hang -- so the control is
       handed back silently and reported afterwards. */
    setPlain("bass", 9.0f);
    tests.expectEqual(processor.takeAutoMatchWarning(), -1,
                      "a host automation write raises no dialog");
    tests.expect(processor.autoMatchReleased("bass"),
                 "a host automation write hands the control back");
    const auto external = processor.takeAutoMatchExternalReleases();
    tests.expectEqual(external.size(), 1, "the external write is reported once");
    tests.expect(processor.takeAutoMatchExternalReleases().isEmpty(),
                 "reporting an external release clears it");

    // A whole-rig replacement stops the hold rather than guarding a rig that is not there.
    processor.applyRecoveredRig(rig, false);
    tests.expect(processor.autoMatchState() == tf::automatch::State::holding, "re-applying holds again");
    processor.setCurrentProgram(3);
    tests.expect(processor.autoMatchState() == tf::automatch::State::armed,
                 "a program change stops the hold and leaves the switch on");
    tests.expectEqual(processor.takeAutoMatchWarning(), -1,
                      "a program change raises no dialog for the parameters it writes");

    /* Changing instrument invalidates the match rather than overriding it: the two instruments
       are searched over different sets of voicings, so there is no "matched value" for an
       instrument the search never ran on. The hold drops and no question is asked. */
    processor.applyRecoveredRig(rig, false);
    tests.expect(processor.autoMatchState() == tf::automatch::State::holding,
                 "the rig is held before the instrument moves");
    userMoves("instrument", 1.0f);
    tests.expectEqual(processor.takeAutoMatchWarning(), -1,
                      "changing instrument does not ask whether to keep the matched value");
    tests.expect(processor.autoMatchState() == tf::automatch::State::armed,
                 "changing instrument drops the hold and waits for a new match");
    userMoves("instrument", 0.0f);

    // With nothing matched there is nothing to apply, and asking must be harmless.
    processor.refreshAutoMatch();
    tests.expect(processor.autoMatchState() == tf::automatch::State::armed,
                 "a re-derivation with no match to apply changes nothing");

    /* A re-derivation keeps what the user took back; pressing Apply does not.

       The distinction is the whole reason `applyAutoMatchRig` exists beside `applyRecoveredRig`.
       A search that re-ran because the region changed must not undo an override set two minutes
       ago; choosing a different candidate by hand is a different rig, and the old override was
       about a different one. */
    processor.applyRecoveredRig(rig, false);
    userMoves("mid", 8.25f);
    (void) processor.takeAutoMatchWarning();
    processor.releaseAutoMatchParameter("mid");
    auto rederived = rig;
    rederived.toneStack.mid = 0.15f;
    rederived.toneStack.treble = 0.85f;
    processor.applyAutoMatchRig(rederived, false);
    tests.expectNear(plainValue("mid"), 8.25f, 0.05,
                     "a re-derivation leaves an overridden control where the user put it");
    tests.expect(processor.autoMatchReleased("mid"), "an overridden control stays overridden");
    tests.expectNear(plainValue("treble"), rederived.toneStack.treble * 10.0f, 0.05,
                     "a re-derivation still writes the controls it holds");
    tests.expectEqual(processor.takeAutoMatchWarning(), -1,
                      "a re-derivation raises no dialog for its own writes");
    processor.applyRecoveredRig(rederived, false);
    tests.expectNear(plainValue("mid"), rederived.toneStack.mid * 10.0f, 0.05,
                     "pressing Apply is a new rig and takes every control back");

    /* Live tracking. Bounded to 6 dB either side of the matched value, and it must do nothing
       at all until there is a measured signal to follow -- a rig that walked its input trim to
       a bound on silence would be worse than one that never moved. */
    processor.applyRecoveredRig(rig, false);
    const auto matchedTrim = processor.autoMatchValueOf("input");
    tests.expect(! processor.autoMatchTracking(), "tracking is off unless it is asked for");
    processor.setAutoMatchTracking(true);
    processor.refreshAutoMatch();
    if (matchedTrim) tests.expectNear(plainValue("input"), *matchedTrim, 1.0e-3,
                                      "tracking moves nothing without a measured signal");
    tests.expectEqual(processor.takeAutoMatchWarning(), -1, "tracking raises no dialog");
    processor.setAutoMatchTracking(false);

    // The "don't warn me again" answer outlives the window it was given in, so it lives on the
    // processor rather than in the editor.
    const auto suppressed = processor.autoMatchWarningsSuppressed();
    processor.setAutoMatchWarningsSuppressed(! suppressed);
    tests.expect(processor.autoMatchWarningsSuppressed() != suppressed,
                 "the warning preference can be set and read back");
    processor.setAutoMatchWarningsSuppressed(suppressed);

    // Isolating the chain is what puts the pedals and the sends under the analyzer.
    processor.applyRecoveredRig(rig, true);
    tests.expect(processor.autoMatchOwns("pedal1Bypass"),
                 "isolating the chain holds the pedal bypasses");
    tests.expect(processor.autoMatchOwns("reverbMix"), "isolating the chain holds the sends");

    /* The whole hold survives a save: the switch, the matched values, which controls the user
       had taken back, and whether the chain was part of the match. Anything less and a reopened
       project either stops guarding a rig that is still playing, or guards it with the wrong
       numbers. */
    processor.applyRecoveredRig(rig, false);
    processor.releaseAutoMatchParameter("treble");
    const auto matchedBass = processor.autoMatchValueOf("bass");
    const auto file = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getChildFile("tubeforge-automatch-test.tforge");
    tests.expect(processor.saveProject(file).wasOk(), "a project with Auto Match on saves");
    processor.setAutoMatchEnabled(false);
    tests.expect(processor.loadProject(file).wasOk(), "the project loads again");
    tests.expect(processor.autoMatchEnabled(), "the Auto Match switch is saved with the project");
    tests.expect(processor.autoMatchState() == tf::automatch::State::overridden,
                 "a reopened project resumes holding, with its releases intact");
    tests.expect(processor.autoMatchReleased("treble"), "the released control is still released");
    tests.expect(processor.autoMatchOwns("bass"), "the held controls are still held");
    const auto reloadedBass = processor.autoMatchValueOf("bass");
    tests.expect(reloadedBass.has_value(), "the matched values survive the round trip");
    if (matchedBass && reloadedBass)
        tests.expectNear(*reloadedBass, *matchedBass, 1.0e-3,
                         "a reloaded matched value is the one that was saved");
    tests.expectEqual(processor.takeAutoMatchWarning(), -1,
                      "recalling a project raises no dialog for the parameters it writes");

    // A project that was not holding clears the hold rather than leaving the previous one up.
    processor.setAutoMatchEnabled(false);
    tests.expect(processor.saveProject(file).wasOk(), "a project with Auto Match off saves");
    processor.setAutoMatchEnabled(true);
    processor.applyRecoveredRig(rig, false);
    tests.expect(processor.loadProject(file).wasOk(), "the second project loads");
    tests.expect(processor.autoMatchState() == tf::automatch::State::off,
                 "recalling a project that held nothing stops holding");
    file.deleteFile();
}

/** Every field of a recovered rig that has a parameter behind it lands on that parameter.

    The gap this closes is the one that made a match sound unlike its own audition: a fitted
    field that quietly never reached the amplifier. Written as an explicit table so that adding a
    field to `AmpParameters` and forgetting to apply it is a failing test rather than a tone
    nobody can account for.
*/
void testRecoveredRigReachesEveryParameter(TestHarness& tests)
{
    TubeForgeAudioProcessor processor;
    processor.setPlayConfigDetails(2, 2, 48000.0, 32);
    processor.prepareToPlay(48000.0, 32);
    auto& parameters = processor.getParameters();
    const auto plainValue = [&parameters](const char* id)
    {
        const auto* value = parameters.getRawParameterValue(id);
        return value == nullptr ? 0.0f : value->load(std::memory_order_relaxed);
    };

    // Values chosen to be inside every range and unlike every default, so a parameter that was
    // never written fails rather than coincidentally agreeing.
    auto rig = nts::amp::makeOriginalPreset(nts::amp::Topology::classAChime,
                                            nts::amp::Instrument::bass).parameters;
    rig.manualInputTrimDb = -4.5f;
    rig.toneStack.bass = 0.28f;
    rig.toneStack.mid = 0.71f;
    rig.toneStack.treble = 0.42f;
    rig.powerAmp.presence = 0.63f;
    rig.powerAmp.resonance = 0.19f;
    rig.powerAmp.masterDb = -7.5f;
    rig.powerAmp.sag = 0.62f;
    rig.powerAmp.feedback = 0.41f;
    rig.preEq.lowCutHz = 123.0f;
    rig.preEq.highCutHz = 9400.0f;
    rig.preEq.tightness = 0.37f;
    rig.preEq.pickEmphasisDb = -3.5f;
    rig.gateEnabled = false;
    rig.gateThresholdDb = -47.0f;
    rig.gateDepthDb = -22.0f;
    rig.gateAttackMs = 4.5f;
    rig.gateHoldMs = 120.0f;
    rig.gateReleaseMs = 380.0f;
    rig.loudnessMatch = false;
    rig.bass.crossoverHz = 240.0f;
    rig.bass.cleanBlend = 0.72f;
    rig.bass.lowDriveDb = 5.5f;
    rig.bass.lowLevelDb = -2.5f;
    rig.bass.highLevelDb = 1.5f;
    rig.dryBlend = 0.33f;
    rig.cabinet.blend = 0.62f;
    rig.cabinet.width = 0.24f;
    rig.cabinet.slots[1].delaySamples = 17;
    rig.cabinet.bypass = false;
    for (std::size_t stage = 0; stage < rig.stages.size(); ++stage)
    {
        rig.stages[stage].driveDb = 9.0f + static_cast<float>(stage) * 2.5f;
        rig.stages[stage].bias = 0.21f;
    }

    processor.applyRecoveredRig(rig, false);

    struct Expectation { const char* id; float expected; float tolerance; const char* what; };
    const std::vector<Expectation> expectations {
        { "instrument", 1.0f, 1.0e-3f, "instrument" },
        { "topology", static_cast<float>(static_cast<int>(rig.topology)), 1.0e-3f, "voicing" },
        { "input", rig.manualInputTrimDb, 0.05f, "input trim" },
        { "bass", rig.toneStack.bass * 10.0f, 0.05f, "bass" },
        { "mid", rig.toneStack.mid * 10.0f, 0.05f, "mid" },
        { "treble", rig.toneStack.treble * 10.0f, 0.05f, "treble" },
        { "presence", rig.powerAmp.presence * 10.0f, 0.05f, "presence" },
        { "resonance", rig.powerAmp.resonance * 10.0f, 0.05f, "resonance" },
        { "master", rig.powerAmp.masterDb, 0.05f, "master" },
        { "sag", rig.powerAmp.sag * 100.0f, 0.15f, "sag" },
        { "feedback", rig.powerAmp.feedback * 100.0f, 0.15f, "feedback" },
        { "lowCut", rig.preEq.lowCutHz, 1.0f, "pre low cut" },
        { "highCut", rig.preEq.highCutHz, 10.0f, "pre high cut" },
        { "tightness", rig.preEq.tightness * 10.0f, 0.05f, "tightness" },
        { "pickEmphasis", rig.preEq.pickEmphasisDb, 0.05f, "pick emphasis" },
        { "bias", rig.stages[0].bias, 0.01f, "bias" },
        { "stage1", rig.stages[0].driveDb, 0.05f, "stage 1 drive" },
        { "stage2", rig.stages[1].driveDb, 0.05f, "stage 2 drive" },
        { "stage3", rig.stages[2].driveDb, 0.05f, "stage 3 drive" },
        { "stage4", rig.stages[3].driveDb, 0.05f, "stage 4 drive" },
        { "gateEnabled", 0.0f, 1.0e-3f, "gate enabled" },
        { "gateThreshold", rig.gateThresholdDb, 0.05f, "gate threshold" },
        { "gateDepth", rig.gateDepthDb, 0.05f, "gate depth" },
        { "gateAttack", rig.gateAttackMs, 0.05f, "gate attack" },
        { "gateHold", rig.gateHoldMs, 1.0f, "gate hold" },
        { "gateRelease", rig.gateReleaseMs, 1.0f, "gate release" },
        { "loudnessMatch", 0.0f, 1.0e-3f, "loudness match" },
        { "crossover", rig.bass.crossoverHz, 1.0f, "bass crossover" },
        { "cleanBlend", rig.bass.cleanBlend * 100.0f, 0.15f, "clean blend" },
        { "dryBlend", rig.dryBlend * 100.0f, 0.15f, "dry blend" },
        { "lowBandDrive", rig.bass.lowDriveDb, 0.05f, "low band drive" },
        { "lowBandLevel", rig.bass.lowLevelDb, 0.05f, "low band level" },
        { "highBandLevel", rig.bass.highLevelDb, 0.05f, "high band level" },
        { "cabinet", 1.0f, 1.0e-3f, "cabinet enabled" },
        { "cabinetBlend", rig.cabinet.blend * 100.0f, 0.15f, "cabinet blend" },
        { "cabinetWidth", rig.cabinet.width * 100.0f, 0.15f, "cabinet width" },
        { "cabinetAlignment", static_cast<float>(rig.cabinet.slots[1].delaySamples), 0.5f, "cabinet alignment" },
        // The three the apply path normalises rather than copies -- see the Auto Match plan.
        { "gain", 5.0f, 1.0e-3f, "the neutralised gain macro" },
        { "panelSwitch", 0.0f, 1.0e-3f, "the cleared panel switch" },
        { "oversampling", 4.0f, 1.0e-3f, "oversampling back on Auto" },
    };

    for (const auto& expectation : expectations)
        tests.expectNear(plainValue(expectation.id), expectation.expected, expectation.tolerance,
                         std::string("a recovered rig sets ") + expectation.what);

    // The engine mode too: a match is a traditional rig, and applying one while a neural capture
    // is selected would leave the user listening to something the search never rendered.
    tests.expectNear(plainValue("engineMode"), 0.0f, 1.0e-3,
                     "a recovered rig selects the traditional engine");
}

/** Schema 5: the saved hold, and what an older project turns into.

    In the wrapper suite rather than in `nts_unit_tests` because the interesting half is the
    plug-in's own round trip -- the state library cannot say whether the values it stored map
    back onto the right controls.
*/
void testAutoMatchProjectFormat(TestHarness& tests)
{
    nts::state::ProjectState state;
    state.autoMatch.holding = true;
    state.autoMatch.isolatedChain = true;
    state.autoMatch.heldValues = { 5.0f, 6.5f, 7.25f };
    state.autoMatch.releasedIndices = { 1 };

    const auto restored = nts::state::deserialize(nts::state::serialize(state));
    tests.expect(static_cast<bool>(restored), "a project carrying a held rig round-trips");
    if (restored)
    {
        tests.expect(restored.state->autoMatch == state.autoMatch,
                     "every field of the held rig survives serialization");
        tests.expectEqual(restored.state->schemaVersion, nts::state::currentSchemaVersion,
                          "the project is written at the current schema");
    }

    // A released index that points past the saved values would mark a control released that the
    // file says nothing about, so it is corruption rather than something to clamp.
    auto broken = state;
    broken.autoMatch.releasedIndices = { 7 };
    std::string error;
    tests.expect(! nts::state::validate(broken, error),
                 "a released index outside the saved rig is rejected");

    auto emptyHold = state;
    emptyHold.autoMatch.heldValues.clear();
    emptyHold.autoMatch.releasedIndices.clear();
    tests.expect(! nts::state::validate(emptyHold, error),
                 "claiming to hold a rig with no values in it is rejected");

    /* A schema 4 project predates all of this and must open, holding nothing. Written by hand
       rather than by editing a serialized version number, so the test still means what it says
       if the writer's formatting changes. */
    const auto migrated = nts::state::deserialize(
        R"({"schemaVersion":4,"applicationVersion":"0.10.0"})");
    tests.expect(static_cast<bool>(migrated), "a schema 4 project still opens");
    if (migrated)
        tests.expect(! migrated.state->autoMatch.holding,
                     "a project written before Auto Match holds nothing");
}

/** Schema 6: the cabinet's own block, and what a project written before it turns into.

    The claim being tested is narrow and is the one that matters: a project from before the
    cabinet had controls of its own must come back with an **empty** block, because the plug-in's
    defaults for those controls are the values the fields held while they were unreachable. An
    empty block is a complete description of that cabinet, not a gap.
*/
void testCabinetProjectFormat(TestHarness& tests)
{
    nts::state::ProjectState state;
    state.cabinet.controls = { 0.0f, -3.5f, -100.0f, 100.0f, 0.0f, 1.0f, 12.0f, 0.0f, 0.0f,
                               70.0f, 10500.0f, 0.0f, -1.5f };

    const auto restored = nts::state::deserialize(nts::state::serialize(state));
    tests.expect(static_cast<bool>(restored), "a project carrying cabinet controls round-trips");
    if (restored)
        tests.expect(restored.state->cabinet == state.cabinet,
                     "every cabinet control survives serialization");

    // The bound exists to stop a corrupt file asking for an unbounded allocation, and the finite
    // check to stop a NaN reaching a convolver's delay line -- where, unlike a filter's, it would
    // never decay out again.
    std::string error;
    auto tooMany = state;
    tooMany.cabinet.controls.assign(nts::state::maximumCabinetControls + 1, 0.0f);
    tests.expect(! nts::state::validate(tooMany, error),
                 "a cabinet block longer than the format accepts is rejected");
    auto notFinite = state;
    notFinite.cabinet.controls[2] = std::numeric_limits<float>::quiet_NaN();
    tests.expect(! nts::state::validate(notFinite, error),
                 "a non-finite cabinet control is rejected");

    const auto migrated = nts::state::deserialize(
        R"({"schemaVersion":5,"applicationVersion":"0.10.0"})");
    tests.expect(static_cast<bool>(migrated), "a schema 5 project still opens");
    if (migrated)
        tests.expect(migrated.state->cabinet.controls.empty(),
                     "a project written before the cabinet stage names no cabinet controls");
}

/** The rule that stops the cabinet model re-voicing somebody's finished track.

    A fresh instance gets a real modelled cabinet; a project written before the model existed was
    mixed against the two synthetic decays that stood in for one, and has to keep them. Both halves
    are asserted, because getting either backwards is silent: the sound simply changes, and the
    only symptom is somebody's mix no longer being their mix.
*/
void testCabinetModelCompatibility(TestHarness& tests)
{
    const auto legacyIndex = static_cast<float>(nts::ir::CabinetKind::legacy);

    TubeForgeAudioProcessor fresh;
    const auto freshModel = fresh.getParameters().getRawParameterValue("cabModelA")->load();
    tests.expect(freshModel != legacyIndex,
                 "a fresh instance starts on a modelled cabinet rather than the legacy stand-in");

    // A schema-5 project: no cabinet block at all, which is exactly what predates the model.
    auto olderProject = nts::state::ProjectState {};
    olderProject.cabinet.controls.clear();
    const auto olderJson = nts::state::serialize(olderProject);
    // Written back down to schema 5 by hand, so the test still means what it says if the writer's
    // formatting changes -- the same reason the migration checks above are hand-written.
    auto downgraded = juce::String(olderJson).replace("\"schemaVersion\": 6", "\"schemaVersion\": 5")
                                             .replace("\"schemaVersion\":6", "\"schemaVersion\":5");
    const auto restored = nts::state::deserialize(downgraded.toStdString());
    tests.expect(static_cast<bool>(restored), "the hand-downgraded project parses");
    if (! restored) return;
    tests.expect(restored.state->cabinet.controls.empty(),
                 "and carries no cabinet block, which is what makes it the case under test");

    TubeForgeAudioProcessor older;
    // The downgraded text itself, not a re-serialisation of the parsed state: re-serialising would
    // write it back at the current schema with a cabinet block, which is the one property this
    // test exists to make sure is absent.
    older.setStateInformation(downgraded.toRawUTF8(), downgraded.getNumBytesAsUTF8());
    tests.expectNear(older.getParameters().getRawParameterValue("cabModelA")->load(), legacyIndex,
                     1.0e-3, "a project from before the model comes back on the legacy cabinet");
    tests.expectNear(older.getParameters().getRawParameterValue("cabModelB")->load(), legacyIndex,
                     1.0e-3, "in both slots");

    /* The response curve, which is what the page's plot draws.

       Asserted here rather than in `nts_ir_tests` because the claim is about the *processor* --
       that the curve it publishes describes the cabinet actually loaded -- and the interesting
       half of that is the wiring, not the model. A closed 4x12 loses a great deal above 5 kHz, so
       a curve that does not is a curve that came from somewhere else. */
    const auto curve = fresh.cabinetResponseCurve(0);
    tests.expectEqual(curve.size(), TubeForgeAudioProcessor::cabinetResponsePoints,
                      "the processor publishes a response curve for slot A");
    if (curve.size() == TubeForgeAudioProcessor::cabinetResponsePoints)
    {
        std::size_t oneKilohertz {}, eightKilohertz {};
        for (std::size_t point = 0; point < curve.size(); ++point)
        {
            const auto frequency = TubeForgeAudioProcessor::cabinetResponseFrequency(point);
            if (frequency <= 1000.0f) oneKilohertz = point;
            if (frequency <= 8000.0f) eightKilohertz = point;
        }
        tests.expect(curve[oneKilohertz] > curve[eightKilohertz] + 12.0f,
                     "the published curve carries the cabinet's top-end roll-off: "
                         + std::to_string(curve[oneKilohertz] - curve[eightKilohertz]) + " dB");
    }
    tests.expectEqual(fresh.cabinetSumResponseCurve().size(),
                      TubeForgeAudioProcessor::cabinetResponsePoints,
                      "and a summed curve for the section as a whole");
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
    testNeuralArtifactLoading(tests);
    testTuner(tests);
    testPerformanceTier(tests);
    testProgramsAndMidi(tests);
    testSharedFrontEnd(tests);
    testTimeBasedEffects(tests);
    testPedalboard(tests);
    testCaptureConvertsAndPlays(tests);
    testFinalAdditions(tests);
    testGearPickerCatalogues(tests);
    testHostTailReporting(tests);
    testAutoMatch(tests);
    testRecoveredRigReachesEveryParameter(tests);
    testAutoMatchProjectFormat(tests);
    testCabinetProjectFormat(tests);
    testCabinetModelCompatibility(tests);
    testEditorLifecycle(tests);
    return tests.result();
}
