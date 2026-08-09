// Loading things from disk into the engine: cabinet responses, neural artifacts,
// the physical circuit and tone packages. All of it staged off the message thread.

#include "PluginProcessorInternal.h"

namespace
{
/** The outcome of reading an exported model artifact directory and staging what is in it.

    `cancelled` is not a failure: the worker was asked to stop, usually because a newer load
    replaced it, and it must leave the status line alone rather than overwriting the newer
    load's own message with a stale one.
*/
struct ArtifactStageResult
{
    bool ok {};
    bool cancelled {};
    std::string message;
};

/** Validates an artifact directory and stages its model into `target`.

    Shared by the amplifier's neural engine and by every pedal slot, so a capture loaded as a
    pedal gets exactly the guarantees one loaded as an amplifier does: manifest schema, a
    SHA-256 over model.bin that has to match the manifest, and the exported test vectors run
    through the model before it is allowed anywhere near the audio thread.

    Runs on a worker: it reads files and runs inference.
*/
ArtifactStageResult stageArtifactDirectory(const juce::File& artifactDirectory,
                                           nts::ml::NeuralAmpProcessor& target,
                                           const std::stop_token& stopToken)
{
    const auto fail = [](std::string message) { return ArtifactStageResult { false, false, std::move(message) }; };
    const auto modelFile = artifactDirectory.getChildFile("model.bin");
    const auto inputFile = artifactDirectory.getChildFile("test-vectors").getChildFile("input.f32");
    const auto outputFile = artifactDirectory.getChildFile("test-vectors").getChildFile("output.f32");
    const auto metadataFile = artifactDirectory.getChildFile("test-vectors").getChildFile("metadata.json");
    const auto normalizationFile = artifactDirectory.getChildFile("normalization.json");
    const auto manifestFile = artifactDirectory.getChildFile("manifest.json");
    if (! modelFile.existsAsFile() || ! inputFile.existsAsFile() || ! outputFile.existsAsFile()
        || ! metadataFile.existsAsFile() || ! normalizationFile.existsAsFile() || ! manifestFile.existsAsFile())
        return fail("Model artifact is incomplete");
    juce::var manifest;
    if (juce::JSON::parse(manifestFile.loadFileAsString(), manifest).failed() || ! manifest.isObject())
        return fail("Model manifest is invalid");
    // 3 is the WaveNet format that `nts-nam-import` produces from a Neural Amp Modeler capture.
    const auto formatVersion = static_cast<int>(manifest.getProperty("modelFormatVersion", 0));
    if (formatVersion < 1 || formatVersion > 3)
        return fail("Model manifest schema is unsupported");
    juce::MemoryBlock modelBlock, inputBlock, outputBlock;
    if (! modelFile.loadFileAsData(modelBlock) || ! inputFile.loadFileAsData(inputBlock)
        || ! outputFile.loadFileAsData(outputBlock) || inputBlock.getSize() == 0
        || inputBlock.getSize() != outputBlock.getSize() || inputBlock.getSize() % sizeof(float) != 0)
        return fail("Model or test-vector data is invalid");
    const auto expectedHash = manifest.getProperty("sha256", "").toString();
    const auto actualHash = juce::SHA256(modelBlock.getData(), modelBlock.getSize()).toHexString();
    if (expectedHash.isEmpty() || ! actualHash.equalsIgnoreCase(expectedHash))
        return fail("model.bin SHA-256 does not match the manifest");
    const auto sampleCount = inputBlock.getSize() / sizeof(float);
    std::vector<float> testInput(sampleCount), expectedOutput(sampleCount);
    std::memcpy(testInput.data(), inputBlock.getData(), inputBlock.getSize());
    std::memcpy(expectedOutput.data(), outputBlock.getData(), outputBlock.getSize());
    juce::var metadata, normalization;
    if (juce::JSON::parse(metadataFile.loadFileAsString(), metadata).failed()
        || juce::JSON::parse(normalizationFile.loadFileAsString(), normalization).failed())
        return fail("Model validation metadata is invalid");
    const auto maximumError = static_cast<float>(static_cast<double>(
        metadata.getProperty("maximumAbsoluteErrorTolerance", 1.0e-5)));
    const auto expectedRms = static_cast<float>(static_cast<double>(
        normalization.getProperty("inputRmsDb", -21.0)));
    if (stopToken.stop_requested()) return { false, true, {} };
    std::string error;
    const auto bytes = std::span(reinterpret_cast<const std::byte*>(modelBlock.getData()),
                                 modelBlock.getSize());
    if (! target.stageModel(bytes, testInput, expectedOutput, maximumError, expectedRms, error))
        return fail(std::move(error));
    return { true, false, "Ready: " + artifactDirectory.getFileName().toStdString() };
}
} // namespace

void TubeForgeAudioProcessor::requestCabinetIrLoad(int slot, const juce::File& irFile)
{
    requestCabinetIrLoad(slot, irFile, {});
}

void TubeForgeAudioProcessor::requestCabinetIrLoad(int slot, const juce::File& irFile,
                                                   const std::string& expectedDigest)
{
    if (slot < 0 || slot > 1) return;
    {
        const std::scoped_lock lock(cabinetIrMutex);
        cabinetIrSlots[static_cast<std::size_t>(slot)].status =
            "Loading " + irFile.getFileName().toStdString();
    }
    triggerAsyncUpdate();

    // Trimming, DC removal and peak normalisation are what turn an arbitrary recording into a
    // usable cabinet response; the loader already does all three off the message thread.
    const nts::dsp::ImpulsePreparationOptions options {
        static_cast<std::size_t>(std::max(1, getTotalNumOutputChannels())), -80.0f, true, true, -1.0f
    };
    /* The file's digest, taken from its bytes rather than from the decoded audio.

       The bytes are what a project recorded, and they are what changes when a pack is updated in
       place. The decoded audio would also change with the session's sample rate, which is not a
       fact about the file. Read here on the message thread, once, rather than at save time -- see
       `makeProjectState`. */
    std::string digest;
    if (juce::MemoryBlock bytes; irFile.loadFileAsData(bytes) && bytes.getSize() > 0)
        digest = juce::SHA256(bytes.getData(), bytes.getSize()).toHexString().toStdString();

    cabinetIrLoader.loadAsync(irFile, currentSampleRate, options,
        [this, slot, irFile, digest, expectedDigest](nts::ir::CabinetLoadResult result)
        {
            const auto index = static_cast<std::size_t>(slot);
            {
                const std::scoped_lock lock(cabinetIrMutex);
                if (! result)
                {
                    // The loader's own message, verbatim: it knows why it failed and inventing
                    // a friendlier one here would only lose the reason.
                    cabinetIrSlots[index].status = result.error.empty()
                        ? "Could not read an impulse response from this file"
                        : result.error;
                    triggerAsyncUpdate();
                    return;
                }
                cabinetIrSlots[index].file = irFile;
                cabinetIrSlots[index].decoded = std::move(result.impulse);
                cabinetIrSlots[index].digest = digest;
                cabinetIrSlots[index].changedSinceSaved =
                    ! expectedDigest.empty() && ! digest.empty() && expectedDigest != digest;
            }
            applyCabinetIr(slot);
            triggerAsyncUpdate();
        });
}

void TubeForgeAudioProcessor::applyCabinetIr(int slot)
{
    // A file in slot A replaces a matched cabinet, which is the release rule in full: there is no
    // partial state, the matched response is either still sounding or it is not.
    if (slot == 0) releaseCabinetHold();
    const auto index = static_cast<std::size_t>(slot);
    const auto first = slot == 0;
    // 0 is Peak, which is what the loader did before the control existed.
    const auto normalisation = std::clamp(
        static_cast<int>(parameterOf(first ? Param::cabIrNormA : Param::cabIrNormB)), 0, 2);

    std::vector<float> left, right, leftToRight, rightToLeft;
    juce::File source;
    auto trueStereo = false;
    {
        const std::scoped_lock lock(cabinetIrMutex);
        const auto& decoded = cabinetIrSlots[index].decoded;
        if (decoded.channels.empty()) return;
        source = cabinetIrSlots[index].file;
        /* Four channels means a true-stereo capture, and it is asked for as four.

           The loader was previously asked for the output channel count, which is two, so the two
           cross terms of a four-channel file were dropped on the floor and it played as its direct
           pair. That is a *reasonable* interpretation and it is not what the file says. Ordering
           is the near-universal one: L->L, L->R, R->L, R->R. */
        trueStereo = decoded.channels.size() >= 4 && getTotalNumOutputChannels() >= 2;
        // Re-prepared against the current rate every time, so the response stays correct
        // across a host sample-rate change rather than playing back at the wrong length.
        //
        // Peak normalisation stays the loader's job rather than being reimplemented here, so the
        // default path is bit-for-bit what it always was; the other two modes ask the loader not
        // to normalise and are applied below.
        const auto wanted = trueStereo
            ? std::size_t { 4 }
            : static_cast<std::size_t>(std::max(1, getTotalNumOutputChannels()));
        const auto prepared = nts::dsp::prepareImpulseResponse(
            decoded, currentSampleRate,
            { wanted, -80.0f, true, normalisation == 0, -1.0f });
        if (prepared.channels.empty()) return;
        left = prepared.channels.front();
        right = prepared.channels.size() > 1 ? prepared.channels[1] : std::vector<float> {};
        if (trueStereo && prepared.channels.size() >= 4)
        {
            leftToRight = prepared.channels[1];
            rightToLeft = prepared.channels[2];
            // The direct pair is channels 0 and 3; channel 1 is L->R, which the two lines above
            // have just claimed, so the right-hand direct term has to come from channel 3.
            right = prepared.channels[3];
        }
        else trueStereo = false;
    }

    /* RMS normalisation, measured across both channels together.

       Per-channel would move a stereo response's image, which is the one thing normalising a
       stereo impulse must not do. -18 dBFS RMS lands a typical cabinet response at roughly the
       same perceived level as the peak-normalised path, so switching modes is a change of
       *balance* between two loaded responses rather than a jump in output.
    */
    if (normalisation == 1)
    {
        double energy {};
        std::size_t samples {};
        for (const auto* channel : { &left, &right, &leftToRight, &rightToLeft })
            for (const auto sample : *channel) { energy += static_cast<double>(sample) * sample; ++samples; }
        if (samples > 0)
        {
            const auto rms = std::sqrt(energy / static_cast<double>(samples));
            if (rms > 1.0e-9)
            {
                const auto scale = static_cast<float>(nts::dsp::dbToLinear(-18.0f) / rms);
                for (auto* channel : { &left, &right, &leftToRight, &rightToLeft })
                    for (auto& sample : *channel) sample *= scale;
            }
        }
    }

    /* Minimum-phase conversion, before the length is decided.

       In that order because the conversion packs the response's energy to the front, so a
       converted response survives a shorter window than the measurement it came from. Doing it
       afterwards would truncate first and convert a response that had already lost its tail.
    */
    if (parameterOf(first ? Param::cabIrMinPhaseA : Param::cabIrMinPhaseB) >= 0.5f)
    {
        if (! left.empty()) left = nts::ir::minimumPhaseFromImpulse(left, left.size());
        if (! right.empty()) right = nts::ir::minimumPhaseFromImpulse(right, right.size());
        /* Deliberately **not** applied to the cross terms.

           Minimum-phase conversion removes a response's arrival time, and the arrival time of a
           cross term *is the information it carries*: it is how long the far side of the cabinet
           takes to reach the other microphone. Flattening it would collapse a true-stereo capture
           into something with no stereo depth at all -- the opposite of what loading one is for. */
    }

    // The performance tier's largest single lever, and now a tonal control as well. Direct
    // convolution is O(taps) per sample, so a 4096-tap response costs sixteen times a 256-tap one,
    // and a guitar cabinet's response past the first few milliseconds is mostly room rather than
    // speaker. Applied here, on the message thread, so the audio path only ever sees an
    // already-short response.
    //
    // Faded rather than cut: a hard truncation is a step in the impulse, and a step in an
    // impulse is broadband splatter at the top of the spectrum.
    //
    // The user's choice is a *floor* under the tier rather than an override of it: asking for 4096
    // taps on Eco is asking for the thing that tier exists to refuse.
    const auto chosenLength = [this, first]() -> std::size_t
    {
        // Index 0 is "Tier maximum", represented as zero and handled by the caller. The rest are
        // the lengths themselves, so the choice index is the answer rather than a key into a
        // second table that could disagree with the one in `createParameterLayout`.
        constexpr std::array<std::size_t, 7> lengths { 0, 128, 256, 512, 1024, 2048, 4096 };
        const auto choice = std::clamp(
            static_cast<int>(parameterOf(first ? Param::cabIrLengthA : Param::cabIrLengthB)),
            0, static_cast<int>(lengths.size()) - 1);
        return lengths[static_cast<std::size_t>(choice)];
    }();
    if (const auto taps = chosenLength == 0 ? tierLimits().maximumImpulseTaps
                                            : std::min(chosenLength, tierLimits().maximumImpulseTaps);
        taps > 0)
    {
        const auto shorten = [taps](std::vector<float>& response)
        {
            if (response.size() <= taps) return;
            response.resize(taps);
            const auto fade = std::min<std::size_t>(32, taps);
            for (std::size_t index = 0; index < fade; ++index)
            {
                const auto gain = static_cast<float>(fade - index) / static_cast<float>(fade);
                response[taps - fade + index] *= gain;
            }
        };
        shorten(left);
        shorten(right);
        shorten(leftToRight);
        shorten(rightToLeft);
    }

    nts::amp::CabinetMetadata metadata;
    metadata.name = source.getFileNameWithoutExtension().toStdString();
    metadata.microphone = "User impulse";
    // Into the stage, not the amplifier: the cabinet is downstream of all three engines now,
    // so a response loaded into `traditionalAmp` would be heard by neither the neural nor the
    // circuit path -- and by the traditional one only if its internal copy were still running.
    const nts::amp::CabinetSection::TrueStereoImpulse cross {
        trueStereo ? std::span<const float>(leftToRight) : std::span<const float> {},
        trueStereo ? std::span<const float>(rightToLeft) : std::span<const float> {} };
    const auto loaded = slot == 0
        ? cabinetStage.loadImpulseA(left, right, metadata, 2048, cross)
        : cabinetStage.loadImpulseB(left, right, metadata, 2048, cross);

    {
        const std::scoped_lock lock(cabinetIrMutex);
        const auto changed = cabinetIrSlots[index].changedSinceSaved;
        cabinetIrSlots[index].status = ! loaded
            ? "Response is longer than the cabinet section accepts"
            : source.getFileName().toStdString() + " (" + std::to_string(left.size()) + " samples"
                  + (trueStereo ? ", true stereo" : "") + ")"
                  // Said out loud rather than left to be discovered: a pack updated in place is
                  // exactly the thing that silently re-voices a finished mix.
                  + (changed ? " -- changed since this project was saved" : "");
    }
    // The plot follows whatever is sounding, so it is refreshed wherever that changes -- and only
    // there. Recomputing it on a timer would mean transforming a loaded response twenty times a
    // second to draw a curve that had not moved.
    refreshCabinetResponseCurve(slot);
}

juce::Result TubeForgeAudioProcessor::exportCabinetResponse(int slot, const juce::File& destination) const
{
    if (slot < 0 || slot > 1) return juce::Result::fail("There is no such cabinet slot");

    /* Rebuilt rather than read back out of the cabinet stage.

       The stage's copies are written by whichever thread finished a load, so reading them from
       the message thread would be a data race for the sake of avoiding one render. Rebuilding
       also means the exported file is exactly what the *controls* describe, which is what somebody
       exporting it is asking for.
    */
    std::vector<float> left, right;
    {
        const std::scoped_lock lock(cabinetIrMutex);
        const auto& decoded = cabinetIrSlots[static_cast<std::size_t>(slot)].decoded;
        if (! decoded.channels.empty())
        {
            const auto prepared = nts::dsp::prepareImpulseResponse(
                decoded, currentSampleRate, { 2, -80.0f, true, true, -1.0f });
            if (! prepared.channels.empty())
            {
                left = prepared.channels.front();
                if (prepared.channels.size() > 1) right = prepared.channels[1];
            }
        }
    }
    if (left.empty())
    {
        const auto settings = currentCabinetModel(slot);
        left = settings.cabinet == nts::ir::CabinetKind::legacy
            ? nts::amp::makeDefaultCabinetImpulse(slot)
            : nts::ir::renderCabinetImpulse(settings, currentSampleRate, 512);
    }
    if (left.empty()) return juce::Result::fail("This slot has no response to export");

    juce::AudioBuffer<float> buffer(right.empty() ? 1 : 2, static_cast<int>(left.size()));
    buffer.clear();
    buffer.copyFrom(0, 0, left.data(), static_cast<int>(left.size()));
    if (! right.empty())
        buffer.copyFrom(1, 0, right.data(),
                        static_cast<int>(std::min(right.size(), left.size())));

    juce::WavAudioFormat format;
    std::unique_ptr<juce::OutputStream> stream = destination.createOutputStream();
    if (stream == nullptr) return juce::Result::fail("Could not write to that location");
    // 24-bit: an impulse response is normalised to just under full scale and is then convolved,
    // so quantisation in it lands directly in the output. 16 bits would put a dither floor into
    // every note; 32-bit float would be larger for no audible gain at this length.
    // The writer takes ownership of the stream on success and leaves it alone on failure, which is
    // why `stream` is released into the call rather than kept alongside the result.
    auto writer = format.createWriterFor(stream, juce::AudioFormatWriterOptions {}
                                                     .withSampleRate(currentSampleRate)
                                                     .withNumChannels(buffer.getNumChannels())
                                                     .withBitsPerSample(24));
    if (writer == nullptr) return juce::Result::fail("Could not create a WAV writer");
    if (! writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples()))
        return juce::Result::fail("The response could not be written");
    return juce::Result::ok();
}

bool TubeForgeAudioProcessor::cabinetMatchAvailable() const
{
    double rate {};
    return ! studio.reconstructionReferenceSamples(rate).empty();
}

TubeForgeAudioProcessor::CabinetMatchOutcome TubeForgeAudioProcessor::matchCabinetToReference(float depth)
{
    CabinetMatchOutcome outcome;
    double referenceRate {};
    const auto reference = studio.reconstructionReferenceSamples(referenceRate);
    if (reference.empty())
    {
        outcome.error = "Import a song on the Song Match page first: there is nothing to match to.";
        return outcome;
    }

    /* The render, made through a *known* cabinet.

       The whole method rests on the residual being attributable: swapping cabinet A for cabinet B
       changes the output by the difference between them, so the matcher has to be told which
       cabinet the render was made through. Slot A's current model is that cabinet, and a slot
       holding a user impulse response cannot serve -- its response is not something the model can
       express, so there would be nothing to subtract. */
    if (cabinetIrFile(0) != juce::File {})
    {
        outcome.error = "Slot A is playing a loaded response. Use the built-in cabinet there, or "
                        "clear it, so the match has a known starting point to measure against.";
        return outcome;
    }
    const auto measurementCabinet = currentCabinetModel(0);

    /* Rendered through the amplifier as it stands, including its own cabinet.

       `renderOffline` drives `TraditionalAmpProcessor`, which carries the `AmpVoice` copy of the
       cabinet -- the one the live path bypasses. That is exactly what is wanted here: the render
       has to contain a speaker, and it has to be the speaker the matcher is about to subtract. */
    auto rig = currentAmpParameters();
    rig.cabinet.bypass = false;
    nts::amp::AmpPreset preset;
    preset.parameters = rig;
    nts::amp::TraditionalAmpProcessor renderer;
    renderer.prepare({ referenceRate, 512, 1 });
    renderer.loadPreset(preset, 0);
    const auto modelImpulse = nts::ir::renderCabinetImpulse(measurementCabinet, referenceRate, 512);
    static_cast<void>(renderer.loadCabinetImpulse(0, modelImpulse, {}, {}, 0));
    static_cast<void>(renderer.loadCabinetImpulse(1, modelImpulse, {}, {}, 0));
    const auto render = nts::amp::renderOffline(renderer, reference, 256);

    const auto residual = nts::ir::measureCabinetResidual(reference, render, referenceRate);
    if (! residual.valid)
    {
        outcome.error = juce::String(residual.error);
        return outcome;
    }
    outcome.confidence = residual.confidence;

    // The closest built-in cabinet to the correction, named for the interface. Offered as a
    // description of what the correction resembles, not as a claim about the record.
    if (const auto candidates = nts::ir::matchFromLibrary(residual, measurementCabinet, 1);
        ! candidates.empty())
        outcome.nearestCabinet = juce::String(std::string(
                                     nts::ir::cabinetName(candidates.front().settings.cabinet)))
                               + " / " + juce::String(std::string(
                                     nts::ir::microphoneName(candidates.front().settings.microphone)));

    const auto matched = nts::ir::synthesizeMatchedCabinet(residual, measurementCabinet, depth,
                                                           currentSampleRate, 1024);
    if (matched.empty())
    {
        outcome.error = "The matched response could not be built.";
        return outcome;
    }

    nts::amp::CabinetMetadata metadata;
    metadata.name = "Matched to " + studio.reconstructionSourceName().toStdString();
    metadata.microphone = "Cabinet Match";
    if (! cabinetStage.loadImpulseA(matched, {}, metadata))
    {
        outcome.error = "The cabinet was busy loading another response; try again.";
        return outcome;
    }
    refreshCabinetResponseCurve(0);
    /* Held only while the analyzer is actually holding the rig.

       Pressing Match with Auto Match off is an ordinary edit the user made and owns; the same
       press while a matched rig is held is the analyzer completing that rig, and replacing it
       afterwards is worth reporting. The distinction is the same one the parameter guard makes,
       which is why it is drawn from the same state rather than from whether the button was
       pressed. */
    const auto holding = autoMatchState() == tf::automatch::State::holding
                      || autoMatchState() == tf::automatch::State::overridden;
    cabinetHold.store(holding ? CabinetHold::held : CabinetHold::none, std::memory_order_release);
    outcome.applied = true;
    return outcome;
}

float TubeForgeAudioProcessor::cabinetResponseFrequency(std::size_t index) noexcept
{
    // Log-spaced, because a cabinet's identity lives in ratios: the two octaves between 1 and
    // 4 kHz where a guitar speaker breaks up would be a tenth of a linear axis and are half of
    // what there is to look at.
    constexpr auto lowest = 40.0f;
    constexpr auto highest = 16000.0f;
    const auto position = static_cast<float>(index)
                        / static_cast<float>(cabinetResponsePoints - 1);
    return lowest * std::pow(highest / lowest, position);
}

void TubeForgeAudioProcessor::refreshCabinetResponseCurve(int slot)
{
    const auto index = static_cast<std::size_t>(slot);
    std::vector<float> curve(cabinetResponsePoints, 0.0f);

    std::vector<float> response;
    {
        const std::scoped_lock lock(cabinetIrMutex);
        const auto& decoded = cabinetIrSlots[index].decoded;
        if (! decoded.channels.empty())
        {
            const auto prepared = nts::dsp::prepareImpulseResponse(
                decoded, currentSampleRate, { 1, -80.0f, true, true, -1.0f });
            if (! prepared.channels.empty()) response = prepared.channels.front();
        }
    }

    if (response.empty())
    {
        // No user response, so the slot is sounding the model -- and the model can answer for
        // itself without being rendered and transformed back, which is why `cabinetMagnitudeDb`
        // is public. The curve drawn and the curve heard come from the same function.
        const auto settings = currentCabinetModel(slot);
        for (std::size_t point = 0; point < cabinetResponsePoints; ++point)
            curve[point] = nts::ir::cabinetMagnitudeDb(settings, cabinetResponseFrequency(point));
    }
    else
    {
        /* A loaded response has to be transformed. Evaluated directly at the frequencies being
           drawn rather than through an FFT and interpolation: 192 points against a few hundred
           taps is a fraction of a millisecond, it happens once per load, and it avoids the
           question of which bins a log axis should read between. */
        for (std::size_t point = 0; point < cabinetResponsePoints; ++point)
        {
            const auto frequency = cabinetResponseFrequency(point);
            const auto step = -2.0 * juce::MathConstants<double>::pi * frequency / currentSampleRate;
            double real {}, imaginary {};
            for (std::size_t tap = 0; tap < response.size(); ++tap)
            {
                const auto angle = step * static_cast<double>(tap);
                real += response[tap] * std::cos(angle);
                imaginary += response[tap] * std::sin(angle);
            }
            curve[point] = static_cast<float>(
                20.0 * std::log10(std::max(1.0e-6, std::hypot(real, imaginary))));
        }
    }

    const std::scoped_lock lock(cabinetIrMutex);
    cabinetCurves[index] = std::move(curve);
}

std::vector<float> TubeForgeAudioProcessor::cabinetResponseCurve(int slot) const
{
    if (slot < 0 || slot > 1) return {};
    {
        const std::scoped_lock lock(cabinetIrMutex);
        const auto& cached = cabinetCurves[static_cast<std::size_t>(slot)];
        if (! cached.empty()) return cached;
    }
    /* Nothing cached yet, which means nothing has been loaded yet -- so the model is what this
       slot is sounding, and the model can answer without a transform.

       This is not merely a convenience. A host may create the editor before it calls
       `prepareToPlay`, and a response plot that is blank until the transport is armed reads as a
       broken plot rather than as an early one. Computed rather than cached here because the
       branch is only taken before the first load, and caching from a const accessor would mean a
       mutable field for a path that runs once. */
    const auto settings = currentCabinetModel(slot);
    std::vector<float> curve(cabinetResponsePoints);
    for (std::size_t point = 0; point < cabinetResponsePoints; ++point)
        curve[point] = nts::ir::cabinetMagnitudeDb(settings, cabinetResponseFrequency(point));
    return curve;
}

std::vector<float> TubeForgeAudioProcessor::cabinetSumResponseCurve() const
{
    const auto first = cabinetResponseCurve(0);
    const auto second = cabinetResponseCurve(1);
    if (first.size() != cabinetResponsePoints || second.size() != cabinetResponsePoints) return {};

    const auto cabinet = currentCabinetParameters();
    /* Summed as *magnitudes*, which is a deliberate simplification and worth naming.

       Two responses at the same frequency sum according to their relative phase, and this adds
       them as though they were in phase. That over-states the sum wherever they are not -- most
       visibly where a slot delay is combing. The alternative is to draw the true complex sum,
       which needs both slots' phase and would make the plot swing wildly with the alignment
       control; and the reading that already exists for exactly that hazard is the mono-fold
       meter, which is measured from the audio rather than predicted. So the plot shows the
       voicing and the meter shows the cancellation. */
    const auto firstGain = cabinet.slots[0].mute
        ? 0.0f : nts::dsp::dbToLinear(cabinet.slots[0].levelDb) * (1.0f - cabinet.blend);
    const auto secondGain = cabinet.slots[1].mute
        ? 0.0f : nts::dsp::dbToLinear(cabinet.slots[1].levelDb) * cabinet.blend;
    const auto trim = nts::dsp::dbToLinear(cabinet.outputTrimDb);

    std::vector<float> curve(cabinetResponsePoints, -60.0f);
    for (std::size_t point = 0; point < cabinetResponsePoints; ++point)
    {
        const auto frequency = cabinetResponseFrequency(point);
        const auto magnitude = nts::dsp::dbToLinear(first[point]) * firstGain
                             + nts::dsp::dbToLinear(second[point]) * secondGain;
        /* The two section cuts, as second-order Butterworth magnitudes.

           The analogue prototype rather than the digital biquad's exact response: they differ
           only near Nyquist, the high cut tops out at 22 kHz and the plot stops at 16, and the
           prototype is four lines instead of evaluating a transfer function on the unit circle. */
        const auto lowRatio = frequency / std::max(1.0f, cabinet.lowCutHz);
        const auto highRatio = frequency / std::max(1.0f, cabinet.highCutHz);
        const auto lowSquared = lowRatio * lowRatio;
        const auto highSquared = highRatio * highRatio;
        const auto lowCut = lowSquared / std::sqrt(1.0f + lowSquared * lowSquared);
        const auto highCut = 1.0f / std::sqrt(1.0f + highSquared * highSquared);
        curve[point] = nts::dsp::linearToDb(std::max(1.0e-6f, magnitude * lowCut * highCut * trim));
    }
    return curve;
}

TubeForgeAudioProcessor::CabinetAlignment TubeForgeAudioProcessor::suggestedCabinetAlignment() const
{
    /* What each slot is actually convolving, rebuilt here rather than read back out of the
       cabinet stage. The stage's copies are written by whichever thread finished a load, so
       reaching into them from the message thread would be a data race for the sake of avoiding
       two small allocations. */
    const auto responseFor = [this](int slot)
    {
        const auto index = static_cast<std::size_t>(slot);
        const std::scoped_lock lock(cabinetIrMutex);
        const auto& decoded = cabinetIrSlots[index].decoded;
        if (decoded.channels.empty()) return nts::amp::makeDefaultCabinetImpulse(slot);
        const auto prepared = nts::dsp::prepareImpulseResponse(
            decoded, currentSampleRate,
            { 1, -80.0f, true, true, -1.0f });
        if (prepared.channels.empty()) return nts::amp::makeDefaultCabinetImpulse(slot);
        return prepared.channels.front();
    };

    const auto first = responseFor(0);
    const auto second = responseFor(1);
    if (first.empty() || second.empty()) return {};

    /* Cross-correlation over the alignment control's own range, both ways round.

       Only the first few hundred samples of each response are used: past that a cabinet impulse
       is mostly room, which correlates with nothing in particular and would drag the peak
       towards whichever tail happens to be louder. Normalised by the energy of the two windows
       so the returned confidence means "how alike are these", not "how loud are these". */
    const auto window = std::min<std::size_t>(512, std::min(first.size(), second.size()));
    const auto maximumLag = static_cast<int>(nts::amp::CabinetSection::maximumAlignmentSamples);
    double firstEnergy {}, secondEnergy {};
    for (std::size_t index = 0; index < window; ++index)
    {
        firstEnergy += static_cast<double>(first[index]) * first[index];
        secondEnergy += static_cast<double>(second[index]) * second[index];
    }
    const auto normaliser = std::sqrt(firstEnergy * secondEnergy);
    if (normaliser <= 1.0e-12) return {};

    auto bestScore = 0.0;
    auto bestLag = 0;
    for (int lag = -maximumLag; lag <= maximumLag; ++lag)
    {
        double sum {};
        for (std::size_t index = 0; index < window; ++index)
        {
            const auto shifted = static_cast<int>(index) - lag;
            if (shifted < 0 || shifted >= static_cast<int>(window)) continue;
            sum += static_cast<double>(first[index]) * second[static_cast<std::size_t>(shifted)];
        }
        // Strictly greater, so a tie keeps the smaller absolute lag: with two identical responses
        // every lag of zero correlation ties, and "do not move it" is the right answer there.
        if (sum > bestScore) { bestScore = sum; bestLag = lag; }
    }

    CabinetAlignment result;
    result.confidence = static_cast<float>(std::clamp(bestScore / normaliser, 0.0, 1.0));
    // A positive lag means slot B's features arrive *before* slot A's, so B is the early one and
    // B is what gets delayed. The sign convention is worth stating because getting it backwards
    // produces a control that doubles the misalignment it was pressed to remove.
    result.delaySlotA = bestLag < 0;
    result.delaySamples = std::abs(bestLag);
    return result;
}

nts::ir::CabinetModelSettings TubeForgeAudioProcessor::currentCabinetModel(int slot) const noexcept
{
    const auto first = slot == 0;
    nts::ir::CabinetModelSettings settings;
    settings.cabinet = static_cast<nts::ir::CabinetKind>(std::clamp(
        static_cast<int>(parameterOf(first ? Param::cabModelA : Param::cabModelB)),
        0, static_cast<int>(nts::ir::CabinetKind::count) - 1));
    settings.microphone = static_cast<nts::ir::MicrophoneKind>(std::clamp(
        static_cast<int>(parameterOf(first ? Param::cabMicA : Param::cabMicB)),
        0, static_cast<int>(nts::ir::MicrophoneKind::count) - 1));
    settings.position = parameterOf(first ? Param::cabPositionA : Param::cabPositionB) * 0.01f;
    settings.distanceInches = parameterOf(first ? Param::cabDistanceA : Param::cabDistanceB);
    return settings;
}

std::uint64_t TubeForgeAudioProcessor::cabinetModelHash() const noexcept
{
    /* Eight controls into one word, so the audio thread can ask "has the model moved" with a
       compare rather than by rendering anything. The position and distance are quantised to a
       tenth before hashing, which is finer than either parameter's own step, so this cannot miss
       a change the user can make -- and cannot fire on float noise either.

       Deliberately not a cryptographic hash. The failure mode of a collision here is one cabinet
       render that does not happen, and the inputs are eight small bounded numbers. */
    std::uint64_t hash { 1469598103934665603ull };
    const auto mix = [&hash](float value)
    {
        const auto quantised = static_cast<std::uint64_t>(
            static_cast<std::int64_t>(std::lround(value * 10.0f)) + 100000);
        hash = (hash ^ quantised) * 1099511628211ull;
    };
    for (const auto id : { Param::cabModelA, Param::cabModelB, Param::cabMicA, Param::cabMicB,
                           Param::cabPositionA, Param::cabPositionB,
                           Param::cabDistanceA, Param::cabDistanceB,
                           // The loaded-response controls, for the reason given in
                           // `refreshCabinetResponses`: they change what is sounding too.
                           Param::cabIrLengthA, Param::cabIrLengthB, Param::cabIrNormA,
                           Param::cabIrNormB, Param::cabIrMinPhaseA, Param::cabIrMinPhaseB })
        mix(parameterOf(id));
    // The tier's impulse ceiling changes the rendered length, so a tier change has to re-render
    // too -- otherwise Eco keeps playing a 512-tap model it was supposed to have shortened.
    mix(static_cast<float>(tierLimits().maximumImpulseTaps));
    return hash;
}

void TubeForgeAudioProcessor::refreshCabinetResponses()
{
    for (int slot = 0; slot < 2; ++slot)
    {
        /* A slot holding a user response re-prepares that response rather than being handed a
           model it is not showing. Both paths belong here because both are driven by the same
           hash: the controls that decide how a file is prepared -- its length, its normalisation,
           whether it is converted to minimum phase -- change what is sounding exactly as the model
           controls do, and a slot that reloaded only when the file changed would leave those three
           controls apparently inert. */
        if (cabinetIrFile(slot) != juce::File {}) { applyCabinetIr(slot); continue; }
        restoreBuiltInCabinet(slot);
        refreshCabinetResponseCurve(slot);
    }
    renderedCabinetModelHash.store(cabinetModelHash(), std::memory_order_release);
}

void TubeForgeAudioProcessor::releaseCabinetHold() noexcept
{
    auto expected = CabinetHold::held;
    // Compare-exchange rather than a store: `released` must not be overwritten back to itself by
    // a second replacement, and `none` must not be promoted to `released` by one.
    cabinetHold.compare_exchange_strong(expected, CabinetHold::released,
                                        std::memory_order_acq_rel, std::memory_order_relaxed);
}

void TubeForgeAudioProcessor::restoreBuiltInCabinet(int slot)
{
    // Slot A is where a matched cabinet lives, so anything putting a model back there has
    // replaced it. Slot B never holds one and never releases.
    if (slot == 0) releaseCabinetHold();
    // One place, because there are four callers and each of them previously wrote the same
    // three-argument load by hand -- which is how a slot ends up restored with the *other*
    // slot's metadata.
    const auto settings = currentCabinetModel(slot);
    nts::amp::CabinetMetadata metadata;

    std::vector<float> impulse;
    if (settings.cabinet == nts::ir::CabinetKind::legacy)
    {
        /* The original samples, verbatim. See `nts::ir::CabinetKind::legacy`.

           Approximating them from the model would be the tidier code and would defeat the whole
           purpose: this entry exists so that a project mixed against those two impulses plays back
           against those two impulses. "Close enough" is the thing it is here to prevent. */
        impulse = nts::amp::makeDefaultCabinetImpulse(slot);
        metadata = nts::amp::defaultCabinetMetadata(slot);
    }
    else
    {
        // Capped by the tier, which is the same ceiling a user's loaded response is shortened to;
        // 512 taps is ample for a synthesised cabinet with no room in it.
        const auto taps = std::min<std::size_t>(512, tierLimits().maximumImpulseTaps);
        impulse = nts::ir::renderCabinetImpulse(settings, currentSampleRate, taps);
        metadata.name = std::string(nts::ir::cabinetName(settings.cabinet));
        metadata.microphone = std::string(nts::ir::microphoneName(settings.microphone));
        metadata.position = settings.position;
    }

    static_cast<void>(slot == 0 ? cabinetStage.loadImpulseA(impulse, {}, metadata)
                                : cabinetStage.loadImpulseB(impulse, {}, metadata));
}

void TubeForgeAudioProcessor::restoreCabinetIrPaths(const std::string& pathA, const std::string& pathB,
                                                    const std::string& hashA, const std::string& hashB)
{
    const std::array paths { pathA, pathB };
    const std::array hashes { hashA, hashB };
    for (int slot = 0; slot < 2; ++slot)
    {
        const auto index = static_cast<std::size_t>(slot);
        if (paths[index].empty()) { clearCabinetIr(slot); continue; }

        const juce::File file(juce::String::fromUTF8(paths[index].c_str()));
        if (file.existsAsFile()) { requestCabinetIrLoad(slot, file, hashes[index]); continue; }

        // The response is gone, so fall back to the built-in one -- but keep the path. A
        // project reopened on a machine that has not synced its IR folder yet should not
        // silently destroy the reference the next time it is saved.
        restoreBuiltInCabinet(slot);
        const std::scoped_lock lock(cabinetIrMutex);
        cabinetIrSlots[index].file = file;
        cabinetIrSlots[index].decoded = {};
        cabinetIrSlots[index].status = "Missing: " + file.getFileName().toStdString();
    }
    triggerAsyncUpdate();
}

void TubeForgeAudioProcessor::clearCabinetIr(int slot)
{
    if (slot < 0 || slot > 1) return;
    const auto index = static_cast<std::size_t>(slot);
    {
        const std::scoped_lock lock(cabinetIrMutex);
        cabinetIrSlots[index] = {};
    }
    restoreBuiltInCabinet(slot);
    refreshCabinetResponseCurve(slot);
    triggerAsyncUpdate();
}

juce::String TubeForgeAudioProcessor::cabinetIrStatusText(int slot) const
{
    if (slot < 0 || slot > 1) return {};
    const std::scoped_lock lock(cabinetIrMutex);
    return juce::String::fromUTF8(cabinetIrSlots[static_cast<std::size_t>(slot)].status.c_str());
}

juce::File TubeForgeAudioProcessor::cabinetIrFile(int slot) const
{
    if (slot < 0 || slot > 1) return {};
    const std::scoped_lock lock(cabinetIrMutex);
    return cabinetIrSlots[static_cast<std::size_t>(slot)].file;
}

void TubeForgeAudioProcessor::requestNeuralModelLoad(const juce::File& artifactDirectory)
{
    if (neuralLoader.joinable()) neuralLoader.request_stop();
    neuralLoadStatus.store(nts::diagnostics::AssetLoadStatus::loading, std::memory_order_relaxed);
    diagnostics.setModelLoadStatus(nts::diagnostics::AssetLoadStatus::loading);
    {
        const std::scoped_lock lock(neuralStatusMutex);
        neuralStatusDetail = "Loading " + artifactDirectory.getFileName().toStdString();
    }
    neuralLoader = std::jthread([this, artifactDirectory](std::stop_token stopToken)
    {
        loadNeuralArtifact(stopToken, artifactDirectory);
    });
}

void TubeForgeAudioProcessor::loadNeuralArtifact(std::stop_token stopToken,
                                                 juce::File artifactDirectory)
{
    const auto staged = stageArtifactDirectory(artifactDirectory, neuralAmp, stopToken);
    if (staged.cancelled) return;

    /* A full-rig capture already has a speaker in it. See `pendingFullRigCabinetBypass`.

       The entry is looked up by artifact directory name, which is the library's own id, so a
       model folder loaded from outside the library simply is not found -- and is then treated as
       an amplifier capture, which is the safe way to be wrong: the user hears a cabinet they can
       switch off, rather than silence they have to diagnose. */
    auto message = staged.message;
    if (staged.ok)
        if (const auto entry = captures.find(artifactDirectory.getFileName().toStdString());
            entry && entry->gear == nts::nam::GearKind::fullRig)
        {
            pendingFullRigCabinetBypass.store(true, std::memory_order_release);
            message += " -- full rig, so the cabinet section was switched off";
            triggerAsyncUpdate();
        }
    {
        const std::scoped_lock lock(neuralStatusMutex);
        neuralStatusDetail = std::move(message);
    }
    const auto status = staged.ok ? nts::diagnostics::AssetLoadStatus::ready
                                  : nts::diagnostics::AssetLoadStatus::failed;
    neuralLoadStatus.store(status, std::memory_order_relaxed);
    diagnostics.setModelLoadStatus(status);
}

void TubeForgeAudioProcessor::requestPedalModelLoad(int slot, const juce::File& artifactDirectory)
{
    if (slot < 0 || slot >= static_cast<int>(nts::pedals::slotCount)) return;
    const auto index = static_cast<std::size_t>(slot);
    {
        const std::scoped_lock lock(pedalModelMutex);
        pedalModelSlots[index].status = "Loading " + artifactDirectory.getFileName().toStdString();
    }
    // Move-assigning a running jthread stops and joins it first, so a second load into the
    // same slot replaces the first rather than racing it. Slots do not share a worker, so
    // loading into one never cancels another.
    pedalLoaders[index] = std::jthread(
        [this, index, artifactDirectory](std::stop_token stopToken)
        {
            const auto staged = stageArtifactDirectory(
                artifactDirectory, pedalBoard.slot(index).model(), stopToken);
            if (staged.cancelled) return;
            const std::scoped_lock lock(pedalModelMutex);
            // The path is kept only on success, so a project saved after a failed load does
            // not point at something that will fail again on the next open.
            if (staged.ok) pedalModelSlots[index].file = artifactDirectory;
            pedalModelSlots[index].status = staged.message;
        });
}

juce::String TubeForgeAudioProcessor::pedalModelStatusText(int slot) const
{
    if (slot < 0 || slot >= static_cast<int>(nts::pedals::slotCount)) return {};
    const std::scoped_lock lock(pedalModelMutex);
    return juce::String::fromUTF8(pedalModelSlots[static_cast<std::size_t>(slot)].status.c_str());
}

juce::File TubeForgeAudioProcessor::pedalModelFile(int slot) const
{
    if (slot < 0 || slot >= static_cast<int>(nts::pedals::slotCount)) return {};
    const std::scoped_lock lock(pedalModelMutex);
    return pedalModelSlots[static_cast<std::size_t>(slot)].file;
}

void TubeForgeAudioProcessor::requestCaptureImport(const juce::File& source, nts::nam::Tier tier)
{
    if (source == juce::File {}) return;
    importRunning.store(true, std::memory_order_relaxed);
    importProgress.store(0.0f, std::memory_order_relaxed);
    {
        const std::scoped_lock lock(importStatusMutex);
        importStatus = "Reading " + source.getFileName().toStdString();
    }
    // Move-assignment stops and joins any import already running, so starting a second one
    // replaces the first rather than letting two workers write the same artifact directories.
    captureImporter = std::jthread(
        [this, source, tier](std::stop_token stopToken)
        {
            // The worker gets its own view of the same directory rather than sharing the
            // library the editor is reading. Both only ever read the filesystem concurrently;
            // the shared library's in-memory list is rebuilt on the message thread below, so it
            // is never mutated out from under a page that is drawing it.
            nts::nam::CaptureLibrary worker(captureLibraryPath());
            const auto report = worker.import(
                source, tier, stopToken,
                [this](int done, int total, const juce::String& label)
                {
                    importProgress.store(total > 0 ? static_cast<float>(done) / static_cast<float>(total)
                                                   : 1.0f,
                                         std::memory_order_relaxed);
                    {
                        const std::scoped_lock lock(importStatusMutex);
                        importStatus = "Converting " + std::to_string(done + 1) + " of "
                                     + std::to_string(total)
                                     + (label.isEmpty() ? std::string {} : ": " + label.toStdString());
                    }
                    // Rescan periodically rather than only at the end, so an archive of hundreds
                    // fills the list as it goes instead of appearing all at once after a minute
                    // of apparently nothing happening. A partly written artifact cannot be seen:
                    // its sidecar is the last file written, and the library ignores a directory
                    // without one.
                    if (done > 0 && done % 16 == 0)
                    {
                        pendingCaptureRefresh.store(true, std::memory_order_release);
                        triggerAsyncUpdate();
                    }
                });

            std::string summary;
            if (report.cancelled > 0)
                summary = "Cancelled with " + std::to_string(report.cancelled) + " left to convert; ";
            summary += std::to_string(report.converted) + " converted";
            if (report.reused > 0) summary += ", " + std::to_string(report.reused) + " already imported";
            if (report.failed > 0) summary += ", " + std::to_string(report.failed) + " refused";
            // The first refusal, verbatim. A count alone tells the user something went wrong
            // without telling them the one thing that would let them do anything about it.
            if (! report.messages.empty()) summary += " -- " + report.messages.front();
            {
                const std::scoped_lock lock(importStatusMutex);
                importStatus = std::move(summary);
            }
            importProgress.store(1.0f, std::memory_order_relaxed);
            importRunning.store(false, std::memory_order_relaxed);
            pendingCaptureRefresh.store(true, std::memory_order_release);
            triggerAsyncUpdate();
        });
}

void TubeForgeAudioProcessor::cancelCaptureImport()
{
    if (captureImporter.joinable()) captureImporter.request_stop();
}

juce::String TubeForgeAudioProcessor::captureImportStatusText() const
{
    const std::scoped_lock lock(importStatusMutex);
    return juce::String::fromUTF8(importStatus.c_str());
}

void TubeForgeAudioProcessor::loadPackagedNeuralModel(std::stop_token stopToken,
                                                      juce::File packageDirectory)
{
    const auto fail = [this](std::string message)
    {
        { const std::scoped_lock lock(neuralStatusMutex); neuralStatusDetail = std::move(message); }
        neuralLoadStatus.store(nts::diagnostics::AssetLoadStatus::failed, std::memory_order_relaxed);
        diagnostics.setModelLoadStatus(nts::diagnostics::AssetLoadStatus::failed);
    };
    juce::MemoryBlock modelBlock;
    if (! packageDirectory.getChildFile("model.bin").loadFileAsData(modelBlock))
    { fail("Packaged neural model is missing"); return; }
    juce::var test;
    if (juce::JSON::parse(packageDirectory.getChildFile("model-test.json").loadFileAsString(), test).failed())
    { fail("Packaged model test vector is invalid"); return; }
    const auto inputValue = test.getProperty("input", {});
    const auto expectedValue = test.getProperty("expected", {});
    const auto* input = inputValue.getArray();
    const auto* expected = expectedValue.getArray();
    const auto tolerance = static_cast<float>(static_cast<double>(test.getProperty("tolerance", 1.0e-5)));
    if (input == nullptr || expected == nullptr || input->isEmpty() || input->size() != expected->size())
    { fail("Packaged model test vector is incomplete"); return; }
    std::vector<float> testInput(static_cast<std::size_t>(input->size()));
    std::vector<float> expectedOutput(testInput.size());
    for (int index = 0; index < input->size(); ++index)
    {
        testInput[static_cast<std::size_t>(index)] = static_cast<float>(input->getReference(index));
        expectedOutput[static_cast<std::size_t>(index)] = static_cast<float>(expected->getReference(index));
    }
    if (stopToken.stop_requested()) return;
    std::string error;
    const auto bytes = std::span(reinterpret_cast<const std::byte*>(modelBlock.getData()), modelBlock.getSize());
    // The capture's own declared calibration, where it ships one. This used to pass a flat -21 dB
    // regardless, so a capture made at any other level was told the wrong thing about its input and
    // the compensation built on top of that reading was off by the difference. `stageArtifactDirectory`
    // already reads the same field; this path was the inconsistent one. The fallback stays -21 because
    // that is the runtime default the older packages were exported against.
    auto expectedRms = -21.0f;
    juce::var normalization;
    const auto normalizationFile = packageDirectory.getChildFile("normalization.json");
    if (normalizationFile.existsAsFile()
        && ! juce::JSON::parse(normalizationFile.loadFileAsString(), normalization).failed())
        expectedRms = static_cast<float>(static_cast<double>(
            normalization.getProperty("inputRmsDb", -21.0)));
    if (! neuralAmp.stageModel(bytes, testInput, expectedOutput, tolerance, expectedRms, error))
    { fail(error); return; }
    { const std::scoped_lock lock(neuralStatusMutex); neuralStatusDetail = "Ready: " + packageDirectory.getFileName().toStdString(); }
    neuralLoadStatus.store(nts::diagnostics::AssetLoadStatus::ready, std::memory_order_relaxed);
    diagnostics.setModelLoadStatus(nts::diagnostics::AssetLoadStatus::ready);
}

juce::String TubeForgeAudioProcessor::neuralModelStatusText() const
{
    const std::scoped_lock lock(neuralStatusMutex);
    return juce::String::fromUTF8(neuralStatusDetail.c_str());
}

void TubeForgeAudioProcessor::refreshPhysicalCircuit()
{
    if (currentBlockSize <= 0) return;
    const auto graph = currentCircuitGraph();
    const auto hash = circuitHash(graph);
    if (hash == physicalCircuitControlHash.load(std::memory_order_acquire)
        || hash == physicalCircuitRequestedHash.load(std::memory_order_acquire)) return;
    physicalCircuitRequestedHash.store(hash, std::memory_order_release);
    if (circuitCompiler.joinable()) circuitCompiler.request_stop();
    circuitCompiler = std::jthread([this, graph, hash](std::stop_token stopToken)
    {
        if (stopToken.stop_requested()) return;
        const auto report = physicalCircuit.stageGraph(graph);
        if (stopToken.stop_requested()) return;
        const auto publicationBusy = std::any_of(report.messages.begin(), report.messages.end(), [](const auto& message)
        {
            return message.message == "A graph publication is already pending";
        });
        if (!report.isValid() || publicationBusy)
        {
            const std::scoped_lock lock(circuitStatusMutex);
            circuitStatusDetail = report.messages.empty() ? "Physical circuit validation failed" : report.messages.front().message;
            physicalCircuitRequestedHash.store(0, std::memory_order_release);
            return;
        }
        physicalCircuitControlHash.store(hash, std::memory_order_release);
        physicalCircuitRequestedHash.store(0, std::memory_order_release);
        {
            const std::scoped_lock graphLock(circuitGraphMutex);
            desiredCircuitGraph = graph;
        }
        const std::scoped_lock lock(circuitStatusMutex);
        circuitStatusDetail = report.messages.empty()
            ? "Compiled on worker: " + std::to_string(graph.nodes.size()) + " immutable nodes, zero added latency"
            : "Compiled with bounded virtual-model warning: " + report.messages.front().message;
    });
}

nts::circuit::CircuitGraphDescription TubeForgeAudioProcessor::circuitGraphSnapshot() const
{
    // Parameter reads are atomic; returning the requested graph makes the editor
    // respond immediately while the immutable runtime copy compiles off-thread.
    return currentCircuitGraph();
}

juce::String TubeForgeAudioProcessor::circuitStatusText() const
{
    const std::scoped_lock lock(circuitStatusMutex);
    return juce::String::fromUTF8(circuitStatusDetail.c_str());
}

std::vector<nts::ecosystem::ProfileRecord> TubeForgeAudioProcessor::searchTonePackages(
    const nts::ecosystem::ProfileQuery& query) const
{
    return tonePackageLibrary.search(query);
}

juce::Result TubeForgeAudioProcessor::refreshTonePackages()
{
    std::string error;
    if (! tonePackageLibrary.refresh(error)) return juce::Result::fail(error);
    return juce::Result::ok();
}

juce::Result TubeForgeAudioProcessor::importTonePackage(const juce::File& packageDirectory)
{
    std::string error;
    if (! tonePackageLibrary.importPackage(std::filesystem::path(packageDirectory.getFullPathName().toStdString()), error))
        return juce::Result::fail(error);
    logger.log({ std::chrono::system_clock::now(), nts::diagnostics::LogSeverity::info,
                 "ecosystem", "package-import", "Tone package imported", "metadata-only" });
    return juce::Result::ok();
}

juce::Result TubeForgeAudioProcessor::exportCurrentTonePackage(const juce::File& destination,
                                                               const juce::String& name,
                                                               const juce::String& author,
                                                               bool includeCabinets) const
{
    if (name.trim().isEmpty() || author.trim().isEmpty())
        return juce::Result::fail("Profile name and author are required");
    nts::amp::AmpPreset preset;
    preset.name = name.trim().toStdString(); preset.parameters = currentAmpParameters();
    nts::ecosystem::PackageExportRequest request;
    request.manifest.name = preset.name; request.manifest.author = author.trim().toStdString();
    request.manifest.instrument = preset.parameters.instrument == nts::amp::Instrument::bass ? "bass" : "guitar";
    request.manifest.packageLicense = "user-owned"; request.manifest.tags = { "local", "amp-rig" };
    request.rigJson = nts::amp::serializePreset(preset, true);

    /* The loaded responses, when the user has said they may travel. See the header for why that
       is a question rather than a default.

       Recorded in slot order with a parallel slot mapping, because the exporter names the files
       by index: a package carrying only slot B's response would otherwise be indistinguishable
       from one carrying only slot A's, and the receiving end would load it into the wrong slot. */
    if (includeCabinets)
    {
        const std::scoped_lock lock(cabinetIrMutex);
        for (int slot = 0; slot < 2; ++slot)
        {
            const auto& file = cabinetIrSlots[static_cast<std::size_t>(slot)].file;
            if (! file.existsAsFile()) continue;
            request.cabinetFiles.push_back(
                std::filesystem::path(file.getFullPathName().toStdString()));
            request.manifest.cabinetSlots.push_back(slot);
        }
    }
    std::string error;
    if (! tonePackageLibrary.exportPackage(
            std::filesystem::path(destination.withFileExtension("ntone").getFullPathName().toStdString()),
            std::move(request), error))
        return juce::Result::fail(error);
    return juce::Result::ok();
}

juce::Result TubeForgeAudioProcessor::applyTonePackage(const juce::String& packageId)
{
    const auto record = tonePackageLibrary.find(packageId.toStdString());
    if (! record || ! record->compatible) return juce::Result::fail("Profile is missing or needs a newer runtime");
    const auto rigPath = record->packagePath / "rig.json";
    const auto presetText = juce::File(juce::String(rigPath.wstring().c_str())).loadFileAsString().toStdString();
    const auto preset = nts::amp::deserializePreset(presetText);
    if (! preset) return juce::Result::fail("Profile rig failed schema validation");
    const auto& p = preset->parameters;
    // A profile replaces the rig, so this is not a person turning knobs and there is no matched
    // rig left to hold afterwards. See applyProjectState for the same pairing and why.
    const AutoWriteScope autoWrite(*this);
    setParameterValue(parameterState, ParameterIds::instrument, p.instrument == nts::amp::Instrument::bass ? 1.0f : 0.0f);
    setParameterValue(parameterState, ParameterIds::topology, static_cast<float>(static_cast<int>(p.topology)));
    setParameterValue(parameterState, ParameterIds::gain, 5.0f);
    setParameterValue(parameterState, ParameterIds::bass, p.toneStack.bass * 10.0f);
    setParameterValue(parameterState, ParameterIds::mid, p.toneStack.mid * 10.0f);
    setParameterValue(parameterState, ParameterIds::treble, p.toneStack.treble * 10.0f);
    setParameterValue(parameterState, ParameterIds::presence, p.powerAmp.presence * 10.0f);
    setParameterValue(parameterState, ParameterIds::resonance, p.powerAmp.resonance * 10.0f);
    setParameterValue(parameterState, ParameterIds::master, p.powerAmp.masterDb);
    setParameterValue(parameterState, ParameterIds::cabinet, p.cabinet.bypass ? 0.0f : 1.0f);
    constexpr std::array<const char*, 4> stageIds { ParameterIds::stage1, ParameterIds::stage2,
                                                    ParameterIds::stage3, ParameterIds::stage4 };
    for (std::size_t index = 0; index < p.stages.size(); ++index)
        setParameterValue(parameterState, stageIds[index], p.stages[index].driveDb);
    setParameterValue(parameterState, ParameterIds::bias, p.stages[0].bias);
    setParameterValue(parameterState, ParameterIds::lowCut, p.preEq.lowCutHz);
    setParameterValue(parameterState, ParameterIds::highCut, p.preEq.highCutHz);
    setParameterValue(parameterState, ParameterIds::tightness, p.preEq.tightness * 10.0f);
    setParameterValue(parameterState, ParameterIds::pickEmphasis, p.preEq.pickEmphasisDb);
    // Loading a preset must not silently drop the user out of Auto; Auto already
    // resolves the factor from the preset's own drive settings.
    if (static_cast<int>(parameterOf(Param::oversampling)) != automaticOversamplingIndex)
    {
        const auto factor = p.stages[0].oversamplingFactor;
        setParameterValue(parameterState, ParameterIds::oversampling, factor >= 8 ? 3.0f : factor >= 4 ? 2.0f : factor >= 2 ? 1.0f : 0.0f);
    }
    setParameterValue(parameterState, ParameterIds::gateEnabled, p.gateEnabled ? 1.0f : 0.0f);
    setParameterValue(parameterState, ParameterIds::gateThreshold, p.gateThresholdDb);
    setParameterValue(parameterState, ParameterIds::gateDepth, p.gateDepthDb);
    setParameterValue(parameterState, ParameterIds::gateAttack, p.gateAttackMs);
    setParameterValue(parameterState, ParameterIds::gateHold, p.gateHoldMs);
    setParameterValue(parameterState, ParameterIds::gateRelease, p.gateReleaseMs);
    setParameterValue(parameterState, ParameterIds::loudnessMatch, p.loudnessMatch ? 1.0f : 0.0f);
    setParameterValue(parameterState, ParameterIds::sag, p.powerAmp.sag * 100.0f);
    setParameterValue(parameterState, ParameterIds::feedback, p.powerAmp.feedback * 100.0f);
    setParameterValue(parameterState, ParameterIds::crossover, p.bass.crossoverHz);
    setParameterValue(parameterState, ParameterIds::cleanBlend, p.bass.cleanBlend * 100.0f);
    setParameterValue(parameterState, ParameterIds::dryBlend, p.dryBlend * 100.0f);
    setParameterValue(parameterState, ParameterIds::lowBandDrive, p.bass.lowDriveDb);
    setParameterValue(parameterState, ParameterIds::lowBandLevel, p.bass.lowLevelDb);
    setParameterValue(parameterState, ParameterIds::highBandLevel, p.bass.highLevelDb);
    setParameterValue(parameterState, ParameterIds::cabinetAlignment, static_cast<float>(p.cabinet.slots[1].delaySamples));
    // The rest of the cabinet the rig describes. Written here as well as in `applyRecoveredRig`
    // because a profile is the other way a whole rig arrives, and a cabinet section restored
    // without its cuts is not the cabinet the author saved.
    setParameterValue(parameterState, ParameterIds::cabLowCut, p.cabinet.lowCutHz);
    setParameterValue(parameterState, ParameterIds::cabHighCut, p.cabinet.highCutHz);
    setParameterValue(parameterState, ParameterIds::cabDiBlend, p.cabinet.bassDiBlend * 100.0f);
    setParameterValue(parameterState, ParameterIds::cabOutputTrim, p.cabinet.outputTrimDb);
    setParameterValue(parameterState, ParameterIds::cabLevelA, p.cabinet.slots[0].levelDb);
    setParameterValue(parameterState, ParameterIds::cabLevelB, p.cabinet.slots[1].levelDb);
    setParameterValue(parameterState, ParameterIds::cabPanA, p.cabinet.slots[0].pan * 100.0f);
    setParameterValue(parameterState, ParameterIds::cabPanB, p.cabinet.slots[1].pan * 100.0f);
    setParameterValue(parameterState, ParameterIds::cabPhaseA, p.cabinet.slots[0].phaseInvert ? 1.0f : 0.0f);
    setParameterValue(parameterState, ParameterIds::cabPhaseB, p.cabinet.slots[1].phaseInvert ? 1.0f : 0.0f);
    setParameterValue(parameterState, ParameterIds::cabDelayA, static_cast<float>(p.cabinet.slots[0].delaySamples));
    setParameterValue(parameterState, ParameterIds::cabMuteA, p.cabinet.slots[0].mute ? 1.0f : 0.0f);
    setParameterValue(parameterState, ParameterIds::cabMuteB, p.cabinet.slots[1].mute ? 1.0f : 0.0f);

    /* Impulse responses the package carries, if it carries any.

       Loaded from inside the package rather than copied out of it: the package directory is the
       library's own, it is validated before anything here runs, and a copy would be a second file
       nobody asked for. A slot the package says nothing about is left alone -- a profile that did
       not travel with a cabinet should not clear the one the user already had. */
    for (std::size_t index = 0; index < record->manifest.cabinetSlots.size(); ++index)
    {
        const auto slot = record->manifest.cabinetSlots[index];
        const auto asset = std::find_if(record->manifest.assets.begin(), record->manifest.assets.end(),
            [index](const auto& entry)
            { return entry.path.rfind("cabinet/" + std::to_string(index + 1) + ".", 0) == 0; });
        if (asset == record->manifest.assets.end()) continue;
        const auto file = juce::File(juce::String(
            (record->packagePath / asset->path).wstring().c_str()));
        if (file.existsAsFile()) requestCabinetIrLoad(slot, file);
    }
    clearAutoMatchHold();
    std::string error;
    (void) tonePackageLibrary.markUsed(packageId.toStdString(), error);
    const auto hasModel = std::any_of(record->manifest.assets.begin(), record->manifest.assets.end(), [](const auto& asset)
    { return asset.path == "model.bin"; });
    if (hasModel)
    {
        if (neuralLoader.joinable()) neuralLoader.request_stop();
        neuralLoadStatus.store(nts::diagnostics::AssetLoadStatus::loading, std::memory_order_relaxed);
        diagnostics.setModelLoadStatus(nts::diagnostics::AssetLoadStatus::loading);
        const juce::File packageDirectory(juce::String(record->packagePath.wstring().c_str()));
        neuralLoader = std::jthread([this, packageDirectory](std::stop_token stopToken)
        { loadPackagedNeuralModel(stopToken, packageDirectory); });
    }
    logger.log({ std::chrono::system_clock::now(), nts::diagnostics::LogSeverity::info,
                 "ecosystem", "package-applied", "Tone package applied", packageId.toStdString() });
    return juce::Result::ok();
}

juce::Result TubeForgeAudioProcessor::setTonePackageFavorite(const juce::String& packageId, bool favorite)
{
    std::string error;
    if (! tonePackageLibrary.setFavorite(packageId.toStdString(), favorite, error)) return juce::Result::fail(error);
    return juce::Result::ok();
}

nts::circuit::SimpleControls TubeForgeAudioProcessor::currentCircuitControls() const noexcept
{
    nts::circuit::SimpleControls controls;
    controls.tubeCharacter = std::clamp((parameterOf(Param::bias) + 0.8f) / 1.6f, 0.0f, 1.0f);
    controls.headroom = std::clamp((parameterOf(Param::master) + 60.0f) / 72.0f, 0.0f, 1.0f);
    controls.breakup = std::clamp(parameterOf(Param::gain) * 0.1f, 0.0f, 1.0f);
    controls.tightness = std::clamp(parameterOf(Param::tightness) * 0.1f, 0.0f, 1.0f);
    controls.sag = std::clamp(parameterOf(Param::sag) * 0.01f, 0.0f, 1.0f);
    controls.powerSize = std::clamp((parameterOf(Param::resonance)
                                     + parameterOf(Param::presence)) * 0.05f, 0.0f, 1.0f);
    controls.feedback = std::clamp(parameterOf(Param::feedback) * 0.01f, 0.0f, 1.0f);
    controls.cabinet = std::clamp(parameterOf(Param::cabinet) >= 0.5f
                                      ? parameterOf(Param::treble) * 0.1f : 0.0f, 0.0f, 1.0f);
    return controls;
}

nts::circuit::CircuitGraphDescription TubeForgeAudioProcessor::currentCircuitGraph() const
{
    auto graph = nts::circuit::makeSimpleCircuit(currentCircuitControls());
    graph.id = "tubeforge.circuit.user";
    graph.name = "TubeForge User Circuit";

    const auto preampTube = std::clamp(static_cast<int>(valueOf(parameterState, ParameterIds::circuitPreampTube)), 0, 2);
    const auto powerTube = std::clamp(static_cast<int>(valueOf(parameterState, ParameterIds::circuitPowerTube)), 0, 1);
    const auto powerTopology = std::clamp(static_cast<int>(valueOf(parameterState, ParameterIds::circuitPowerTopology)), 0, 2);
    const auto toneStyle = std::clamp(static_cast<int>(valueOf(parameterState, ParameterIds::circuitToneStack)), 0, 2);
    const auto numerical = valueOf(parameterState, ParameterIds::circuitBackend) >= 0.5f;
    const std::array preampIds { "tube.12au7.v1", "tube.12at7.v1", "tube.12ax7.v1" };
    const std::array powerIds { "power.6v6.v1", "power.el34.v1" };
    const std::array toneIds { "passive.fmv.vintage.v1", "passive.fmv.modern.v1", "passive.fmv.bass.v1" };

    for (auto& node : graph.nodes)
    {
        if (node.type == nts::circuit::NodeType::filter)
            setCircuitParameter(node, "cutoff-hz", valueOf(parameterState, ParameterIds::lowCut));
        else if (node.type == nts::circuit::NodeType::triodeStage)
        {
            node.modelId = preampIds[static_cast<std::size_t>(preampTube)];
            node.backend = numerical ? nts::circuit::ModelBackend::numerical : nts::circuit::ModelBackend::graybox;
            const auto stageDrive = (valueOf(parameterState, ParameterIds::stage1) + 12.0f) / 54.0f;
            setCircuitParameter(node, "drive", std::clamp(0.55f * stageDrive + 0.45f * valueOf(parameterState, ParameterIds::gain) * 0.1f, 0.0f, 1.0f));
            setCircuitParameter(node, "bias", std::clamp((valueOf(parameterState, ParameterIds::bias) + 0.8f) / 1.6f, 0.0f, 1.0f));
        }
        else if (node.type == nts::circuit::NodeType::toneStack)
        {
            node.modelId = toneIds[static_cast<std::size_t>(toneStyle)];
            setCircuitParameter(node, "bass", valueOf(parameterState, ParameterIds::bass) * 0.1f);
            setCircuitParameter(node, "middle", valueOf(parameterState, ParameterIds::mid) * 0.1f);
            setCircuitParameter(node, "treble", valueOf(parameterState, ParameterIds::treble) * 0.1f);
            constexpr std::array slope { 100000.0f, 56000.0f, 100000.0f };
            constexpr std::array bassCap { 22.0e-9f, 47.0e-9f, 100.0e-9f };
            constexpr std::array midCap { 22.0e-9f, 22.0e-9f, 47.0e-9f };
            constexpr std::array trebleCap { 250.0e-12f, 470.0e-12f, 330.0e-12f };
            setCircuitParameter(node, "slope-resistance-ohm", slope[static_cast<std::size_t>(toneStyle)]);
            setCircuitParameter(node, "bass-cap-f", bassCap[static_cast<std::size_t>(toneStyle)]);
            setCircuitParameter(node, "mid-cap-f", midCap[static_cast<std::size_t>(toneStyle)]);
            setCircuitParameter(node, "treble-cap-f", trebleCap[static_cast<std::size_t>(toneStyle)]);
        }
        else if (node.type == nts::circuit::NodeType::phaseInverter)
            setCircuitParameter(node, "drive", std::clamp((valueOf(parameterState, ParameterIds::stage3) + 12.0f) / 54.0f, 0.0f, 1.0f));
        else if (node.type == nts::circuit::NodeType::powerStage)
        {
            node.modelId = powerIds[static_cast<std::size_t>(powerTube)];
            setCircuitParameter(node, "topology", static_cast<float>(powerTopology));
            setCircuitParameter(node, "tube-count", powerTopology == 0 ? 1.0f : powerTopology == 1 ? 2.0f : 4.0f);
            setCircuitParameter(node, "drive", std::clamp((valueOf(parameterState, ParameterIds::stage4) + 12.0f) / 54.0f, 0.0f, 1.0f));
            setCircuitParameter(node, "sag", valueOf(parameterState, ParameterIds::sag) * 0.01f);
            setCircuitParameter(node, "feedback", valueOf(parameterState, ParameterIds::feedback) * 0.0075f);
        }
        else if (node.type == nts::circuit::NodeType::feedback)
        {
            setCircuitParameter(node, "amount", valueOf(parameterState, ParameterIds::feedback) * 0.0075f);
            setCircuitParameter(node, "presence", valueOf(parameterState, ParameterIds::presence) * 0.1f);
        }
    }

    const auto triode = std::find_if(graph.nodes.begin(), graph.nodes.end(), [](const auto& node)
    { return node.type == nts::circuit::NodeType::triodeStage; });
    if (triode != graph.nodes.end())
        graph.nodes.insert(std::next(triode), { "high-filter", nts::circuit::NodeType::filter, "rc.lowpass.v1",
            nts::circuit::ModelBackend::graybox, {}, { { "cutoff-hz", valueOf(parameterState, ParameterIds::highCut) }, { "high-pass", 0.0f } } });
    /* The circuit engine no longer carries a speaker of its own.

       It used to synthesise a cabinet node here, with its own resonance/brightness pair and three
       fixed model ids, while the impulse-response cabinet lived inside the traditional
       amplifier -- so the two engines had different speakers and a loaded response did nothing on
       this one. The cabinet is a stage after all three engines now, and leaving this node in
       would put two cabinets in series, which is not a feature anybody asked for.

       `circuitCabinetStyle` stays registered and saved but is no longer read. Removing the
       parameter would shift every id after it in `ampControlIds` and silently corrupt every saved
       project -- see the note at that list. An inert parameter is the cheap half of that trade. */
    std::erase_if(graph.nodes, [](const auto& node) { return node.type == nts::circuit::NodeType::cabinet; });
    graph.connections.clear();
    for (std::size_t index = 1; index < graph.nodes.size(); ++index)
        graph.connections.push_back({ graph.nodes[index - 1].id, 0, graph.nodes[index].id, 0, false });
    return graph;
}
