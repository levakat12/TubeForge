#include "nts/nam/CaptureLibrary.h"

#include <nts/ml/NeuralModel.h>

#include <juce_cryptography/juce_cryptography.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace nts::nam
{
namespace
{
/// A capture is a few hundred kilobytes of JSON. Anything past this is not one, and decompressing
/// it to find that out is how an archive turns into a denial of service.
constexpr std::int64_t maximumCaptureBytes = 64 * 1024 * 1024;
/// Long enough to exercise the model past its own receptive field, short enough that converting
/// eighty-nine captures does not become a coffee break.
constexpr int verificationSamples = 2048;

/// Deterministic, dependency-free, and broadband: a chirp keeps the low end that a WaveNet's
/// deepest dilations respond to, and the noise keeps the top end that its shallow ones do.
std::vector<float> verificationSignal()
{
    std::vector<float> signal(verificationSamples);
    std::uint32_t state = 417u;
    for (int index = 0; index < verificationSamples; ++index)
    {
        state = state * 1664525u + 1013904223u;
        const auto noise = static_cast<float>(static_cast<double>(state >> 8) / 8388608.0 - 1.0);
        const auto phase = 0.0009 * static_cast<double>(index) * static_cast<double>(index);
        signal[static_cast<std::size_t>(index)] =
            0.06f * noise + 0.05f * static_cast<float>(std::sin(phase));
    }
    return signal;
}

juce::String sanitized(const juce::String& name)
{
    juce::String result;
    for (const auto character : name)
        result += juce::CharacterFunctions::isLetterOrDigit(character) || character == '-'
                          || character == '_' || character == ' '
                      ? character
                      : '-';
    return result.trim().substring(0, 60);
}

bool writeJson(const juce::File& file, juce::DynamicObject* object)
{
    return file.replaceWithText(juce::JSON::toString(juce::var(object), false) + "\n");
}

bool writeFloats(const juce::File& file, std::span<const float> values)
{
    juce::MemoryBlock block(values.data(), values.size() * sizeof(float));
    return file.replaceWithData(block.getData(), block.getSize());
}

/// The stem of an archive entry or file path, without directories or extension.
juce::String stemOf(const juce::String& path)
{
    const auto slash = std::max(path.lastIndexOfChar('/'), path.lastIndexOfChar('\\'));
    const auto name = slash >= 0 ? path.substring(slash + 1) : path;
    const auto dot = name.lastIndexOfChar('.');
    return dot > 0 ? name.substring(0, dot) : name;
}
} // namespace

juce::String LibraryEntry::summary() const
{
    juce::String line(gearKindName(gear));
    // The product, which the filename-derived display name usually abbreviates or omits.
    const auto product = gearMake.empty()                          ? juce::String(gearModel)
                       : gearModel.empty() || gearModel == gearMake ? juce::String(gearMake)
                                                                    : juce::String(gearMake) + " "
                                                                          + juce::String(gearModel);
    if (product.isNotEmpty()) line += "  /  " + product;
    if (! toneType.empty()) line += "  /  " + juce::String(toneType);
    line += "  /  " + juce::String(tierName(tier)) + " (" + juce::String(channels) + " ch)";
    line += "  /  " + juce::String(sampleRate / 1000) + " kHz";
    if (validationEsr >= 0.0) line += "  /  ESR " + juce::String(validationEsr, 4);
    return line;
}

CaptureLibrary::CaptureLibrary(juce::File rootDirectory) : root(std::move(rootDirectory))
{
    root.createDirectory();
    refresh();
}

bool CaptureLibrary::isMismatch(GearKind entry, GearKind destination) noexcept
{
    if (entry == GearKind::unknown || destination == GearKind::unknown) return false;
    // A full rig is an amplifier and a cabinet together, so it belongs where an amp does.
    const auto amplifierish = [](GearKind kind)
    { return kind == GearKind::amp || kind == GearKind::fullRig; };
    return amplifierish(entry) != amplifierish(destination);
}

std::optional<LibraryEntry> CaptureLibrary::readSidecar(const juce::File& directory) const
{
    const auto sidecar = directory.getChildFile("capture.json");
    juce::var parsed;
    if (! sidecar.existsAsFile()
        || juce::JSON::parse(sidecar.loadFileAsString(), parsed).failed() || ! parsed.isObject())
        return std::nullopt;
    // An artifact whose model is missing is not a capture the library can offer, whatever its
    // sidecar says.
    if (! directory.getChildFile("model.bin").existsAsFile()) return std::nullopt;

    LibraryEntry entry;
    entry.id = directory.getFileName().toStdString();
    entry.displayName = parsed.getProperty("displayName", "").toString().toStdString();
    entry.gearMake = parsed.getProperty("gearMake", "").toString().toStdString();
    entry.gearModel = parsed.getProperty("gearModel", "").toString().toStdString();
    entry.toneType = parsed.getProperty("toneType", "").toString().toStdString();
    entry.modeledBy = parsed.getProperty("modeledBy", "").toString().toStdString();
    entry.gear = gearKindFrom(parsed.getProperty("gearType", "").toString().toStdString());
    entry.tier = parsed.getProperty("tier", "standard").toString() == "lite" ? Tier::lite
                                                                            : Tier::standard;
    entry.sampleRate = static_cast<int>(parsed.getProperty("sampleRate", 0));
    entry.channels = static_cast<int>(parsed.getProperty("channels", 0));
    entry.validationEsr = static_cast<double>(parsed.getProperty("validationEsr", -1.0));
    entry.source = parsed.getProperty("source", "").toString().toStdString();
    entry.artifact = directory;
    if (entry.displayName.empty()) entry.displayName = entry.id;
    return entry;
}

void CaptureLibrary::refresh()
{
    cached.clear();
    for (const auto& child : juce::RangedDirectoryIterator(root, false, "*",
                                                           juce::File::findDirectories))
        if (auto entry = readSidecar(child.getFile())) cached.push_back(std::move(*entry));

    std::sort(cached.begin(), cached.end(), [](const LibraryEntry& left, const LibraryEntry& right)
    { return juce::String(left.displayName).compareIgnoreCase(right.displayName) < 0; });
}

std::vector<LibraryEntry> CaptureLibrary::entries() const { return cached; }

std::optional<LibraryEntry> CaptureLibrary::find(const std::string& id) const
{
    const auto found = std::find_if(cached.begin(), cached.end(),
                                    [&id](const LibraryEntry& entry) { return entry.id == id; });
    return found == cached.end() ? std::nullopt : std::optional(*found);
}

std::vector<LibraryEntry> CaptureLibrary::orderedFor(GearKind preferred) const
{
    auto ordered = cached;
    std::stable_sort(ordered.begin(), ordered.end(),
                     [preferred](const LibraryEntry& left, const LibraryEntry& right)
                     {
                         const auto leftFits = ! isMismatch(left.gear, preferred);
                         const auto rightFits = ! isMismatch(right.gear, preferred);
                         if (leftFits != rightFits) return leftFits;
                         return juce::String(left.displayName)
                                    .compareIgnoreCase(right.displayName) < 0;
                     });
    return ordered;
}

bool CaptureLibrary::remove(const std::string& id, std::string& error)
{
    const auto entry = find(id);
    if (! entry) { error = "That capture is no longer in the library"; return false; }
    // Guarded against a sidecar naming somewhere else entirely: only a direct child of the
    // library root is ever deleted.
    if (entry->artifact.getParentDirectory() != root)
    { error = "Refusing to delete a directory outside the capture library"; return false; }
    if (! entry->artifact.deleteRecursively())
    { error = "Could not delete " + entry->artifact.getFileName().toStdString(); return false; }
    refresh();
    return true;
}

std::optional<LibraryEntry> CaptureLibrary::convert(const juce::String& json,
                                                    const juce::String& sourceName,
                                                    const juce::String& archiveName, Tier tier,
                                                    bool& reused, std::string& error)
{
    reused = false;
    Capture capture;
    if (! readCapture(json, capture, error)) return std::nullopt;
    const auto* spec = capture.spec(tier);
    if (spec == nullptr) { error = "Capture holds no usable submodel"; return std::nullopt; }
    // A capture with only one tier is converted at the tier it has, and the entry records what
    // was actually written rather than what was asked for.
    const auto actualTier = capture.hasTier(tier)
                                ? tier
                                : (tier == Tier::lite ? Tier::standard : Tier::lite);

    std::vector<std::byte> packed;
    if (! packWaveNet(*spec, packed, error)) return std::nullopt;

    const auto digest = juce::SHA256(json.toRawUTF8(), static_cast<std::size_t>(json.getNumBytesAsUTF8()))
                            .toHexString().substring(0, 12);
    const auto directory = root.getChildFile(sanitized(stemOf(sourceName)) + "-"
                                             + tierName(actualTier) + "-" + digest);
    // Keyed by the capture's own digest, so re-importing an archive already in the library
    // resolves to the same directory and costs a stat rather than a conversion.
    if (auto existing = readSidecar(directory)) { reused = true; return existing; }

    // Run the packed model once so the artifact carries vectors that prove it loads, primes and
    // produces finite audio deterministically on this runtime.
    //
    // Be clear about what that is and is not. For an artifact produced by `nts-nam-import` the
    // vectors come from the Python renderer, which is checked against upstream
    // `neural-amp-modeler`, so re-running them is a genuine parity gate. Vectors produced here
    // cannot be: the thing rendering them is the thing they would be checking. What protects
    // parity for this path is `nts_nam_parity`, which requires the bytes written below to be
    // identical to the ones the Python converter writes for the same capture.
    nts::ml::NeuralModel model;
    if (! model.load(packed, error)) return std::nullopt;
    const auto input = verificationSignal();
    std::vector<float> expected(input.size());
    if (! model.process(input, expected))
    { error = "Converted model could not run its verification vector"; return std::nullopt; }
    if (! std::all_of(expected.begin(), expected.end(),
                      [](float value) { return std::isfinite(value); }))
    { error = "Converted model produced non-finite output"; return std::nullopt; }

    const auto vectors = directory.getChildFile("test-vectors");
    if (! vectors.createDirectory().wasOk())
    { error = "Could not create the artifact directory"; return std::nullopt; }

    const auto packedDigest = juce::SHA256(packed.data(), packed.size()).toHexString();
    juce::MemoryBlock modelBlock(packed.data(), packed.size());
    const auto rms = std::clamp(capture.metadata.loudness.value_or(-21.0), -60.0, 0.0);

    auto* manifest = new juce::DynamicObject();
    manifest->setProperty("modelFormatVersion", 3);
    manifest->setProperty("architecture", "wavenet");
    manifest->setProperty("sampleRate", spec->sampleRate);
    manifest->setProperty("inputChannels", 1);
    manifest->setProperty("outputChannels", 1);
    manifest->setProperty("stateSize", static_cast<int>(spec->receptiveField()));
    manifest->setProperty("latencySamples", 0);
    manifest->setProperty("expectedInputRmsDb", rms);
    manifest->setProperty("parameterSchema", juce::var(juce::Array<juce::var> {}));
    manifest->setProperty("sha256", packedDigest);
    // Recorded so the difference above is visible in the artifact itself rather than only in a
    // comment: an artifact converted here says so, and the editor says so too.
    manifest->setProperty("testVectorSource", "runtime");

    auto* normalization = new juce::DynamicObject();
    normalization->setProperty("inputRmsDb", rms);
    normalization->setProperty("inputScale", 1.0);
    normalization->setProperty("outputScale", 1.0);
    normalization->setProperty("dcOffset", 0.0);

    auto* vectorMetadata = new juce::DynamicObject();
    vectorMetadata->setProperty("samples", static_cast<int>(input.size()));
    vectorMetadata->setProperty("maximumAbsoluteErrorTolerance", 1.0e-4);
    vectorMetadata->setProperty("rmsErrorTolerance", 1.0e-5);
    vectorMetadata->setProperty("accumulatedDriftTolerance", 1.0e-3);
    vectorMetadata->setProperty("stateResetMaximumError", 1.0e-6);
    vectorMetadata->setProperty("primedState", true);
    vectorMetadata->setProperty("receptiveFieldSamples", static_cast<int>(spec->receptiveField()));

    LibraryEntry entry;
    entry.id = directory.getFileName().toStdString();
    entry.displayName = capture.metadata.displayName(stemOf(sourceName).toStdString());
    entry.gearMake = capture.metadata.gearMake;
    entry.gearModel = capture.metadata.gearModel;
    entry.toneType = capture.metadata.toneType;
    entry.modeledBy = capture.metadata.modeledBy;
    entry.gear = capture.metadata.gear;
    entry.tier = actualTier;
    entry.sampleRate = spec->sampleRate;
    entry.channels = spec->channels;
    entry.validationEsr = capture.metadata.validationEsr.value_or(-1.0);
    entry.source = (archiveName.isEmpty() ? sourceName : archiveName).toStdString();
    entry.artifact = directory;

    auto* sidecar = new juce::DynamicObject();
    sidecar->setProperty("displayName", juce::String(entry.displayName));
    sidecar->setProperty("gearMake", juce::String(entry.gearMake));
    sidecar->setProperty("gearModel", juce::String(entry.gearModel));
    sidecar->setProperty("gearType", juce::String(capture.metadata.gearType));
    sidecar->setProperty("toneType", juce::String(entry.toneType));
    sidecar->setProperty("modeledBy", juce::String(entry.modeledBy));
    sidecar->setProperty("tier", juce::String(tierName(actualTier)));
    sidecar->setProperty("sampleRate", entry.sampleRate);
    sidecar->setProperty("channels", entry.channels);
    sidecar->setProperty("validationEsr", entry.validationEsr);
    sidecar->setProperty("source", juce::String(entry.source));

    const auto written =
        directory.getChildFile("model.bin").replaceWithData(modelBlock.getData(), modelBlock.getSize())
        && writeFloats(vectors.getChildFile("input.f32"), input)
        && writeFloats(vectors.getChildFile("output.f32"), expected)
        && writeJson(vectors.getChildFile("metadata.json"), vectorMetadata)
        && writeJson(directory.getChildFile("manifest.json"), manifest)
        && writeJson(directory.getChildFile("normalization.json"), normalization)
        && directory.getChildFile("license.txt")
               .replaceWithText(juce::String(capture.metadata.licenseText()))
        && writeJson(directory.getChildFile("capture.json"), sidecar);
    if (! written)
    {
        // A half-written artifact would be found by the next refresh and offered as if it were
        // usable, so it goes rather than staying behind.
        directory.deleteRecursively();
        error = "Could not write the converted capture to disk";
        return std::nullopt;
    }
    return entry;
}

ImportReport CaptureLibrary::import(const juce::File& source, Tier tier,
                                    const std::stop_token& stopToken,
                                    const std::function<void(int, int, const juce::String&)>& progress)
{
    ImportReport report;
    const auto note = [&progress](int done, int total, const juce::String& label)
    { if (progress) progress(done, total, label); };

    /// One unit of work: a capture's JSON, what it was called, and which archive it came from.
    struct Pending
    {
        juce::String json, name, archive;
    };
    std::vector<Pending> pending;

    const auto readArchive = [&pending, &report](const juce::File& archive)
    {
        juce::ZipFile zip(archive);
        for (int index = 0; index < zip.getNumEntries(); ++index)
        {
            const auto* entry = zip.getEntry(index);
            if (entry == nullptr || ! entry->filename.endsWithIgnoreCase(".nam")) continue;
            if (entry->uncompressedSize > maximumCaptureBytes)
            {
                ++report.failed;
                report.messages.push_back(entry->filename.toStdString()
                                          + ": entry is too large to be a capture");
                continue;
            }
            // Read into memory and never onto disk under the entry's own name: the destination
            // is derived from a digest below, so a crafted path inside the archive has nowhere
            // to go.
            const std::unique_ptr<juce::InputStream> stream(zip.createStreamForEntry(index));
            if (stream == nullptr)
            {
                ++report.failed;
                report.messages.push_back(entry->filename.toStdString() + ": entry could not be read");
                continue;
            }
            pending.push_back({ stream->readEntireStreamAsString(), entry->filename, archive.getFileName() });
        }
    };

    const auto readFile = [&pending, &report](const juce::File& file)
    {
        if (file.getSize() > maximumCaptureBytes)
        {
            ++report.failed;
            report.messages.push_back(file.getFileName().toStdString()
                                      + ": file is too large to be a capture");
            return;
        }
        pending.push_back({ file.loadFileAsString(), file.getFileName(), {} });
    };

    if (source.isDirectory())
    {
        for (const auto& child : juce::RangedDirectoryIterator(source, true, "*.nam", juce::File::findFiles))
            readFile(child.getFile());
        for (const auto& child : juce::RangedDirectoryIterator(source, true, "*.zip", juce::File::findFiles))
            readArchive(child.getFile());
    }
    else if (source.hasFileExtension("zip"))
    {
        readArchive(source);
    }
    else
    {
        readFile(source);
    }

    const auto total = static_cast<int>(pending.size());
    if (total == 0 && report.failed == 0)
        report.messages.push_back("No .nam captures were found in " + source.getFileName().toStdString());

    for (int index = 0; index < total; ++index)
    {
        if (stopToken.stop_requested()) { report.cancelled = total - index; break; }
        const auto& item = pending[static_cast<std::size_t>(index)];
        note(index, total, stemOf(item.name));

        std::string error;
        auto reused = false;
        if (convert(item.json, item.name, item.archive, tier, reused, error))
            ++(reused ? report.reused : report.converted);
        else
        {
            ++report.failed;
            report.messages.push_back(stemOf(item.name).toStdString() + ": " + error);
        }
    }
    note(total, total, {});
    refresh();
    return report;
}
} // namespace nts::nam
