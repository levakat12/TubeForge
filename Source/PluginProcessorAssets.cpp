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
    cabinetIrLoader.loadAsync(irFile, currentSampleRate, options,
        [this, slot, irFile](nts::ir::CabinetLoadResult result)
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
            }
            applyCabinetIr(slot);
            triggerAsyncUpdate();
        });
}

void TubeForgeAudioProcessor::applyCabinetIr(int slot)
{
    const auto index = static_cast<std::size_t>(slot);
    std::vector<float> left, right;
    juce::File source;
    {
        const std::scoped_lock lock(cabinetIrMutex);
        const auto& decoded = cabinetIrSlots[index].decoded;
        if (decoded.channels.empty()) return;
        source = cabinetIrSlots[index].file;
        // Re-prepared against the current rate every time, so the response stays correct
        // across a host sample-rate change rather than playing back at the wrong length.
        const auto prepared = nts::dsp::prepareImpulseResponse(
            decoded, currentSampleRate,
            { static_cast<std::size_t>(std::max(1, getTotalNumOutputChannels())), -80.0f, true, true, -1.0f });
        if (prepared.channels.empty()) return;
        left = prepared.channels.front();
        right = prepared.channels.size() > 1 ? prepared.channels[1] : std::vector<float> {};
    }

    // The performance tier's largest single lever. Direct convolution is O(taps) per sample, so
    // a 4096-tap response costs sixteen times a 256-tap one, and a guitar cabinet's response
    // past the first few milliseconds is mostly room rather than speaker. Applied here, on the
    // message thread, so the audio path only ever sees an already-short response.
    //
    // Faded rather than cut: a hard truncation is a step in the impulse, and a step in an
    // impulse is broadband splatter at the top of the spectrum.
    if (const auto taps = tierLimits().maximumImpulseTaps; taps > 0)
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
    }

    nts::amp::CabinetMetadata metadata;
    metadata.name = source.getFileNameWithoutExtension().toStdString();
    metadata.microphone = "User impulse";
    const auto loaded = traditionalAmp.loadCabinetImpulse(slot, left, right, metadata);

    const std::scoped_lock lock(cabinetIrMutex);
    cabinetIrSlots[index].status = loaded
        ? source.getFileName().toStdString() + " (" + std::to_string(left.size()) + " samples)"
        : "Response is longer than the cabinet section accepts";
}

void TubeForgeAudioProcessor::restoreCabinetIrPaths(const std::string& pathA, const std::string& pathB)
{
    const std::array paths { pathA, pathB };
    for (int slot = 0; slot < 2; ++slot)
    {
        const auto index = static_cast<std::size_t>(slot);
        if (paths[index].empty()) { clearCabinetIr(slot); continue; }

        const juce::File file(juce::String::fromUTF8(paths[index].c_str()));
        if (file.existsAsFile()) { requestCabinetIrLoad(slot, file); continue; }

        // The response is gone, so fall back to the built-in one -- but keep the path. A
        // project reopened on a machine that has not synced its IR folder yet should not
        // silently destroy the reference the next time it is saved.
        static_cast<void>(traditionalAmp.loadCabinetImpulse(
            slot, nts::amp::makeDefaultCabinetImpulse(slot), {}, nts::amp::defaultCabinetMetadata(slot)));
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
    static_cast<void>(traditionalAmp.loadCabinetImpulse(
        slot, nts::amp::makeDefaultCabinetImpulse(slot), {}, nts::amp::defaultCabinetMetadata(slot)));
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
    {
        const std::scoped_lock lock(neuralStatusMutex);
        neuralStatusDetail = staged.message;
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
                                                               const juce::String& author) const
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
    setParameterValue(parameterState, ParameterIds::cabinetAlignment, static_cast<float>(p.cabinet.delaySamplesB));
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
    const auto cabinetStyle = std::clamp(static_cast<int>(valueOf(parameterState, ParameterIds::circuitCabinetStyle)), 0, 2);
    const std::array preampIds { "tube.12au7.v1", "tube.12at7.v1", "tube.12ax7.v1" };
    const std::array powerIds { "power.6v6.v1", "power.el34.v1" };
    const std::array cabinetIds { "cabinet.reactive.v1", "cabinet.open-back.v1", "cabinet.bass-sealed.v1" };
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
        else if (node.type == nts::circuit::NodeType::cabinet)
        {
            node.modelId = cabinetIds[static_cast<std::size_t>(cabinetStyle)];
            constexpr std::array resonance { 0.50f, 0.38f, 0.78f };
            constexpr std::array brightness { 0.60f, 0.78f, 0.34f };
            setCircuitParameter(node, "resonance", resonance[static_cast<std::size_t>(cabinetStyle)]);
            setCircuitParameter(node, "brightness", brightness[static_cast<std::size_t>(cabinetStyle)]);
        }
    }

    const auto triode = std::find_if(graph.nodes.begin(), graph.nodes.end(), [](const auto& node)
    { return node.type == nts::circuit::NodeType::triodeStage; });
    if (triode != graph.nodes.end())
        graph.nodes.insert(std::next(triode), { "high-filter", nts::circuit::NodeType::filter, "rc.lowpass.v1",
            nts::circuit::ModelBackend::graybox, {}, { { "cutoff-hz", valueOf(parameterState, ParameterIds::highCut) }, { "high-pass", 0.0f } } });
    if (valueOf(parameterState, ParameterIds::cabinet) < 0.5f)
        std::erase_if(graph.nodes, [](const auto& node) { return node.type == nts::circuit::NodeType::cabinet; });
    graph.connections.clear();
    for (std::size_t index = 1; index < graph.nodes.size(); ++index)
        graph.connections.push_back({ graph.nodes[index - 1].id, 0, graph.nodes[index].id, 0, false });
    return graph;
}
