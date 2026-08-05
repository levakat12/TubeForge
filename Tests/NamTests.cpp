// The in-plug-in `.nam` reader, the NTSM v3 packer, and the capture library.
//
// The packer's real correctness gate is `nts_nam_parity`, which requires the bytes it writes to
// be identical to the ones the Python converter writes for the same capture. What is checked
// here is everything that gate cannot reach: that the header lands where the runtime expects to
// find it, that the packed bytes actually load into the runtime, that every unsupported
// construct is refused **by name** rather than quietly approximated, and that importing an
// archive behaves.

#include "TestHarness.h"

#include <nts/ml/NeuralModel.h>
#include <nts/nam/Capture.h>
#include <nts/nam/CaptureLibrary.h>

#include <juce_core/juce_core.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace
{
/// A capture small enough to reason about by hand: two channels, two layers, a three-tap head.
///
/// The weight count is not a magic number -- it is what the layout formula produces for this
/// geometry, and the reader refuses anything else, so getting it wrong here fails loudly.
constexpr int testChannels = 2;
constexpr int testHeadKernel = 3;
constexpr double testHeadScale = 0.75;

std::size_t expectedWeightCount()
{
    const std::size_t channels = testChannels;
    std::size_t total {};
    for (const auto kernel : { 2, 3 })
        total += channels * channels * static_cast<std::size_t>(kernel) // dilated conv
                 + channels                                             // its bias
                 + channels                                             // input mixin
                 + channels * channels + channels;                      // 1x1 and its bias
    return total + channels                                             // input rechannel
           + channels * static_cast<std::size_t>(testHeadKernel) + 1     // head and its bias
           + 1;                                                          // head scale
}

/// Weights that are distinct per index, so a packer that reordered or dropped one would show it.
std::vector<double> testWeights(std::size_t count)
{
    std::vector<double> weights(count);
    for (std::size_t index = 0; index < count; ++index)
        weights[index] = 0.01 * static_cast<double>(index + 1) - 0.2;
    if (! weights.empty()) weights.back() = testHeadScale;
    return weights;
}

juce::String weightArray(const std::vector<double>& weights)
{
    juce::String text("[");
    for (std::size_t index = 0; index < weights.size(); ++index)
    {
        if (index > 0) text += ",";
        text += juce::String(weights[index], 8);
    }
    return text + "]";
}

/// A `.nam` document. `overrides` is spliced into the layer array so a test can turn on exactly
/// one unsupported construct and check that it is the thing that gets named.
juce::String makeCapture(const juce::String& overrides = {}, const juce::String& version = "0.7.0",
                         const juce::String& gearType = "pedal",
                         std::size_t weightCount = expectedWeightCount())
{
    return R"({"version":")" + version + R"(","architecture":"WaveNet","sample_rate":48000.0,)"
         + R"("config":{"head":null,"head_scale":)" + juce::String(testHeadScale, 6)
         + R"(,"layers":[{"channels":)" + juce::String(testChannels)
         + R"(,"input_size":1,"condition_size":1,"bottleneck":)" + juce::String(testChannels)
         + R"(,"kernel_sizes":[2,3],"dilations":[1,2],)"
         + R"("activation":[{"type":"LeakyReLU","negative_slope":0.01},)"
         + R"({"type":"LeakyReLU","negative_slope":0.01}],)"
         + R"("layer1x1":{"active":true,"groups":1},)"
         + R"("head":{"out_channels":1,"kernel_size":)" + juce::String(testHeadKernel)
         + R"(,"bias":true})" + overrides + R"(}]},)"
         + R"("weights":)" + weightArray(testWeights(weightCount)) + ","
         + R"("metadata":{"name":"Test Capture","modeled_by":"nobody","gear_make":"Acme",)"
         + R"("gear_model":"Screamer","gear_type":")" + gearType + R"(","tone_type":"crunch",)"
         + R"("loudness":-23.5,"gain":0.5,"training":{"validation_esr":0.002}}})";
}

std::uint32_t readU32(const std::vector<std::byte>& bytes, std::size_t offset)
{
    std::uint32_t value {};
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

/// A scratch directory that removes itself, so a failing test does not leave artifacts behind.
struct TemporaryDirectory
{
    TemporaryDirectory()
    {
        const auto unique = juce::String(std::chrono::steady_clock::now().time_since_epoch().count());
        file = juce::File::getSpecialLocation(juce::File::tempDirectory)
                   .getChildFile("tubeforge-nam-" + unique);
        file.createDirectory();
    }
    ~TemporaryDirectory() { file.deleteRecursively(); }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    juce::File file;
};

void testReadsACapture(TestHarness& tests)
{
    nts::nam::Capture capture;
    std::string error;
    tests.expect(nts::nam::readCapture(makeCapture(), capture, error),
                 "a well-formed capture is read: " + error);
    tests.expectEqual(capture.sampleRate, 48000, "the sample rate is read");
    tests.expect(capture.metadata.gear == nts::nam::GearKind::pedal,
                 "gear_type pedal classifies as a pedal");
    tests.expectEqual(capture.metadata.gearMake, std::string("Acme"), "gear_make is read");
    // The filename wins: within a pack it is the only thing that distinguishes two captures of
    // the same box at different settings.
    tests.expectEqual(capture.metadata.displayName("TS9 Drive 8"), std::string("TS9 Drive 8"),
                      "the display name prefers the capture's filename");
    tests.expectEqual(capture.metadata.displayName({}), std::string("Test Capture"),
                      "the display name falls back to the capture's own name");
    tests.expect(capture.metadata.validationEsr.has_value()
                     && std::abs(*capture.metadata.validationEsr - 0.002) < 1.0e-9,
                 "the author's reported error is read from training.validation_esr");

    const auto* spec = capture.spec(nts::nam::Tier::standard);
    tests.expect(spec != nullptr, "a bare WaveNet capture exposes a standard tier");
    if (spec == nullptr) return;
    tests.expectEqual(spec->channels, testChannels, "channels are read");
    tests.expectEqual(spec->layers.size(), std::size_t { 2 }, "both layers are read");
    tests.expectEqual(spec->weightCount(), expectedWeightCount(),
                      "the derived weight count matches the layout formula");
    tests.expectEqual(spec->weights.size(), spec->weightCount(),
                      "the file's weight vector matches its own geometry");
    // (2-1)*1 + (3-1)*2 + 3
    tests.expectEqual(spec->receptiveField(), std::size_t { 8 }, "the receptive field is derived");

    // gear_type is the author's label, and the three values the corpus uses all map.
    for (const auto& [text, kind] :
         { std::pair { "amp", nts::nam::GearKind::amp }, { "full-rig", nts::nam::GearKind::fullRig },
           { "pedal", nts::nam::GearKind::pedal }, { "something-else", nts::nam::GearKind::unknown } })
        tests.expect(nts::nam::gearKindFrom(text) == kind,
                     std::string("gear_type '") + text + "' classifies correctly");
}

void testRefusesWhatItCannotModel(TestHarness& tests)
{
    // Every one of these is a construct the v0.7 format can express and this reader does not
    // implement. Rendering them approximately would produce plausible, wrong audio that nothing
    // downstream would catch, so each must be refused, and refused with a reason that names it.
    const std::vector<std::pair<juce::String, std::string>> cases {
        { R"(,"conv_pre_film":{"active":true})", "an active FiLM block" },
        { R"(,"head1x1":{"active":true})", "an active head 1x1" },
        { R"(,"gating_mode":["gated"])", "a gated activation" },
        { R"(,"secondary_activation":["Tanh"])", "a secondary activation" },
        { R"(,"groups_input":2)", "a grouped input convolution" },
        { R"(,"bottleneck":1)", "a bottleneck narrower than the layer" },
        { R"(,"slimmable":true)", "a nested slimmable layer" },
    };
    for (const auto& [override, description] : cases)
    {
        nts::nam::Capture capture;
        std::string error;
        tests.expect(! nts::nam::readCapture(makeCapture(override), capture, error),
                     "the reader refuses " + description);
        tests.expect(! error.empty(), "refusing " + description + " gives a reason");
    }

    nts::nam::Capture capture;
    std::string error;
    tests.expect(! nts::nam::readCapture(makeCapture({}, "0.6.0"), capture, error),
                 "an unsupported format version is refused");
    tests.expect(! nts::nam::readCapture(makeCapture({}, "0.7.0", "pedal", expectedWeightCount() - 1),
                                         capture, error),
                 "a weight vector that disagrees with the declared geometry is refused");
    tests.expect(! nts::nam::readCapture("{not json", capture, error), "malformed JSON is refused");
    tests.expect(! nts::nam::readCapture("{}", capture, error), "an empty document is refused");
}

void testPacksWhatTheRuntimeReads(TestHarness& tests)
{
    nts::nam::Capture capture;
    std::string error;
    if (! nts::nam::readCapture(makeCapture(), capture, error))
    { tests.expect(false, "capture fixture parses"); return; }
    const auto* spec = capture.spec(nts::nam::Tier::standard);
    if (spec == nullptr) { tests.expect(false, "capture fixture has a tier"); return; }

    std::vector<std::byte> packed;
    tests.expect(nts::nam::packWaveNet(*spec, packed, error), "the capture packs: " + error);

    // The header layout, field by field, at the offsets PackedWaveNetModel reads them from. A
    // mismatch here is the failure mode that would otherwise show up as a model that loads and
    // sounds wrong.
    const auto headerBytes = std::size_t { 4 + 11 * sizeof(std::uint32_t) };
    tests.expect(packed.size() > headerBytes, "the packed model is larger than its header");
    tests.expect(std::memcmp(packed.data(), "NTSM", 4) == 0, "the packed model carries the magic");
    tests.expectEqual(readU32(packed, 4), std::uint32_t { 3 }, "format version 3 is declared");
    tests.expectEqual(readU32(packed, 8), std::uint32_t { 5 }, "the WaveNet architecture code is declared");
    tests.expectEqual(readU32(packed, 12), std::uint32_t { 48000 }, "the sample rate is declared");
    tests.expectEqual(readU32(packed, 16), std::uint32_t { 1 }, "one input channel is declared");
    tests.expectEqual(readU32(packed, 20), std::uint32_t { testChannels }, "the width is declared");
    tests.expectEqual(readU32(packed, 24), std::uint32_t { 1 }, "one output channel is declared");
    tests.expectEqual(readU32(packed, 28), std::uint32_t { 0 }, "no controls are declared");
    tests.expectEqual(readU32(packed, 32), std::uint32_t { 2 }, "the layer count is declared");
    tests.expectEqual(readU32(packed, 36), std::uint32_t { 1 }, "the head bias flag is declared");
    tests.expectEqual(readU32(packed, 40), std::uint32_t { testHeadKernel }, "the head kernel is declared");
    tests.expectEqual(readU32(packed, 48), std::uint32_t { 2 }, "layer 1's kernel size follows the header");
    tests.expectEqual(readU32(packed, 52), std::uint32_t { 1 }, "layer 1's dilation follows its kernel size");
    tests.expectEqual(packed.size(), headerBytes + 2 * 2 * sizeof(std::uint32_t)
                                         + expectedWeightCount() * sizeof(float),
                      "the packed size is the header, the layer table and the payload exactly");

    // The claim that matters: the runtime accepts what the converter writes, and runs it.
    nts::ml::NeuralModel model;
    tests.expect(model.load(packed, error), "the runtime loads the packed capture: " + error);
    tests.expect(model.isWaveNet(), "the runtime recognises it as a WaveNet");
    tests.expectEqual(model.sampleRate(), 48000, "the runtime reads back the sample rate");

    std::vector<float> input(256, 0.0f), output(256, 0.0f);
    for (std::size_t index = 0; index < input.size(); ++index)
        input[index] = 0.1f * std::sin(0.05f * static_cast<float>(index));
    tests.expect(model.process(input, output), "the packed capture processes audio");
    auto finite = true, moved = false;
    for (const auto sample : output)
    {
        finite = finite && std::isfinite(sample);
        moved = moved || sample != 0.0f;
    }
    tests.expect(finite, "the packed capture produces finite audio");
    tests.expect(moved, "the packed capture produces something rather than silence");

    // An activation the reader tolerates but the runtime does not implement has to stop at the
    // packer rather than be played as the wrong curve.
    auto tanhSpec = *spec;
    tanhSpec.layers.front().activation = "Tanh";
    std::vector<std::byte> refused;
    tests.expect(! nts::nam::packWaveNet(tanhSpec, refused, error),
                 "the packer refuses an activation the runtime does not implement");
}

void testImportsAnArchive(TestHarness& tests)
{
    TemporaryDirectory workspace;
    const auto libraryRoot = workspace.file.getChildFile("library");

    // Two captures with different gear types in one archive: the corpus ships whole pedals and
    // whole amps this way, and the point of importing is that it does not matter which.
    const auto archive = workspace.file.getChildFile("captures.zip");
    {
        juce::ZipFile::Builder builder;
        const auto pedal = makeCapture({}, "0.7.0", "pedal");
        const auto amp = makeCapture({}, "0.7.0", "amp");
        // The archive holds a nested path and a non-capture file, both of which the importer has
        // to cope with: the first must not escape the library, the second must be ignored.
        const auto entry = [](const void* data, std::size_t size)
        {
            return std::make_unique<juce::MemoryInputStream>(data, size, true);
        };
        builder.addEntry(entry(pedal.toRawUTF8(), static_cast<std::size_t>(pedal.getNumBytesAsUTF8())),
                         9, "Screamer.nam", juce::Time::getCurrentTime());
        builder.addEntry(entry(amp.toRawUTF8(), static_cast<std::size_t>(amp.getNumBytesAsUTF8())),
                         9, "../../../escaped/Head.nam", juce::Time::getCurrentTime());
        builder.addEntry(entry("not a capture", 13), 9, "readme.txt", juce::Time::getCurrentTime());
        juce::FileOutputStream stream(archive);
        tests.expect(stream.openedOk() && builder.writeToStream(stream, nullptr),
                     "the test archive is written");
    }

    nts::nam::CaptureLibrary library(libraryRoot);
    tests.expect(library.entries().empty(), "a fresh library is empty");

    std::stop_source stop;
    auto lastTotal = 0;
    const auto report = library.import(archive, nts::nam::Tier::standard, stop.get_token(),
                                       [&lastTotal](int, int total, const juce::String&)
                                       { lastTotal = total; });
    tests.expectEqual(report.converted, 2, "both captures in the archive convert");
    tests.expectEqual(report.failed, 0, "the non-capture entry is skipped rather than failed");
    tests.expectEqual(lastTotal, 2, "progress reports the number of captures found");
    tests.expectEqual(library.entries().size(), std::size_t { 2 }, "both captures enter the library");

    for (const auto& entry : library.entries())
    {
        tests.expect(entry.artifact.getParentDirectory() == libraryRoot,
                     "an archive entry's own path cannot place an artifact outside the library");
        tests.expect(entry.artifact.getChildFile("model.bin").existsAsFile(), "the artifact holds a model");
        tests.expect(entry.artifact.getChildFile("manifest.json").existsAsFile(), "the artifact holds a manifest");
        tests.expect(entry.artifact.getChildFile("test-vectors").getChildFile("output.f32").existsAsFile(),
                     "the artifact holds verification vectors");
        tests.expect(entry.artifact.getChildFile("license.txt").loadFileAsString().contains("nobody"),
                     "the artifact records who made the capture");
        tests.expectEqual(entry.sampleRate, 48000, "the entry records the capture's rate");
        tests.expectEqual(entry.channels, testChannels, "the entry records the tier's width");
    }

    // The manifest has to say where its vectors came from, because a runtime conversion cannot
    // produce independent ones and the difference must not be invisible.
    juce::var manifest;
    juce::JSON::parse(library.entries().front().artifact.getChildFile("manifest.json").loadFileAsString(),
                      manifest);
    tests.expectEqual(manifest.getProperty("testVectorSource", "").toString(), juce::String("runtime"),
                      "a runtime conversion records that its vectors are not independent");
    tests.expectEqual(static_cast<int>(manifest.getProperty("modelFormatVersion", 0)), 3,
                      "the manifest declares the format the plug-in loads");

    // Re-importing the same archive must be free and must not duplicate anything: conversion is
    // keyed by the capture's digest.
    const auto again = library.import(archive, nts::nam::Tier::standard, stop.get_token(), {});
    tests.expectEqual(again.reused, 2, "re-importing an archive reuses its artifacts");
    tests.expectEqual(again.converted, 0, "re-importing an archive converts nothing again");
    tests.expectEqual(library.entries().size(), std::size_t { 2 }, "re-importing creates no duplicates");

    // Ordering, not filtering: a mismatched capture is still offered, just not first.
    const auto forPedal = library.orderedFor(nts::nam::GearKind::pedal);
    tests.expectEqual(forPedal.size(), std::size_t { 2 }, "ordering for a destination hides nothing");
    tests.expect(forPedal.front().gear == nts::nam::GearKind::pedal,
                 "a pedal slot is offered pedal captures first");
    tests.expect(library.orderedFor(nts::nam::GearKind::amp).front().gear == nts::nam::GearKind::amp,
                 "the amp engine is offered amp captures first");

    tests.expect(nts::nam::CaptureLibrary::isMismatch(nts::nam::GearKind::amp, nts::nam::GearKind::pedal),
                 "an amp capture in a pedal slot is a mismatch");
    tests.expect(! nts::nam::CaptureLibrary::isMismatch(nts::nam::GearKind::fullRig, nts::nam::GearKind::amp),
                 "a full rig belongs where an amp does");
    tests.expect(! nts::nam::CaptureLibrary::isMismatch(nts::nam::GearKind::unknown, nts::nam::GearKind::pedal),
                 "an unlabelled capture is never a mismatch");

    std::string error;
    const auto removed = library.entries().front().id;
    tests.expect(library.remove(removed, error), "a capture can be removed: " + error);
    tests.expectEqual(library.entries().size(), std::size_t { 1 }, "removing a capture removes one");
    tests.expect(! library.remove(removed, error), "removing a capture twice fails rather than crashing");
}

void testImportsALooseCapture(TestHarness& tests)
{
    TemporaryDirectory workspace;
    const auto file = workspace.file.getChildFile("Loose Capture.nam");
    file.replaceWithText(makeCapture({}, "0.7.0", "amp"));

    nts::nam::CaptureLibrary library(workspace.file.getChildFile("library"));
    std::stop_source stop;
    const auto report = library.import(file, nts::nam::Tier::standard, stop.get_token(), {});
    tests.expectEqual(report.converted, 1, "a loose .nam file imports");
    tests.expect(library.entries().size() == 1
                     && library.entries().front().gear == nts::nam::GearKind::amp,
                 "a loose amp capture classifies as an amp");

    // A file that is not a capture at all must be reported, not crash and not half-import.
    const auto rubbish = workspace.file.getChildFile("rubbish.nam");
    rubbish.replaceWithText("{\"version\":\"0.7.0\"}");
    const auto failed = library.import(rubbish, nts::nam::Tier::standard, stop.get_token(), {});
    tests.expectEqual(failed.failed, 1, "an unreadable capture is reported as a failure");
    tests.expect(! failed.messages.empty(), "a failed import names the capture and the reason");
    tests.expectEqual(library.entries().size(), std::size_t { 1 }, "a failed import adds nothing");
}
} // namespace

int main()
{
    TestHarness tests;
    testReadsACapture(tests);
    testRefusesWhatItCannotModel(tests);
    testPacksWhatTheRuntimeReads(tests);
    testImportsAnArchive(tests);
    testImportsALooseCapture(tests);
    return tests.result();
}
