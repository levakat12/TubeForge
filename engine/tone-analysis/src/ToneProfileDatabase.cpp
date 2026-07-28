#include <nts/tone/ToneProfileDatabase.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <unordered_set>

namespace nts::tone
{
namespace
{
float clamp01(float value) noexcept { return std::clamp(value, 0.0f, 1.0f); }
float distance(float left, float right, float scale = 1.0f) noexcept
{
    return clamp01(std::abs(left - right) / std::max(scale, 1.0e-6f));
}

Instrument instrumentFromString(const juce::String& value)
{
    if (value == "guitar") return Instrument::guitar;
    if (value == "bass") return Instrument::bass;
    return Instrument::unknown;
}
Technique techniqueFromString(const juce::String& value)
{
    if (value == "pick") return Technique::pick;
    if (value == "fingerstyle") return Technique::fingerstyle;
    return Technique::unknown;
}
GainCategory gainFromString(const juce::String& value)
{
    if (value == "edge-of-breakup") return GainCategory::edgeOfBreakup;
    if (value == "crunch") return GainCategory::crunch;
    if (value == "high-gain") return GainCategory::highGain;
    if (value == "fuzz") return GainCategory::fuzz;
    return GainCategory::clean;
}
SourceType sourceFromString(const juce::String& value)
{
    if (value == "isolated-stem") return SourceType::isolatedStem;
    if (value == "paired-capture") return SourceType::pairedCapture;
    if (value == "plugin-render") return SourceType::pluginRender;
    if (value == "physical-amp") return SourceType::physicalAmp;
    return SourceType::userRecording;
}

juce::var floatArray(std::span<const float> values)
{
    juce::Array<juce::var> array;
    for (const auto value : values) array.add(value);
    return array;
}

juce::var featuresVar(const ToneFeatures& features)
{
    auto* object = new juce::DynamicObject();
    const std::array spectral { features.spectral.lowFrequencyExtension, features.spectral.lowMidBuildup,
        features.spectral.midEmphasis, features.spectral.upperMidAttack, features.spectral.highFrequencyRolloff,
        features.spectral.resonantPeakStrength, features.spectral.spectralCentroidHz,
        features.spectral.spectralSlopeDbPerOctave, features.spectral.spectralFlatness };
    const std::array dynamics { features.dynamic.crestFactorDb, features.dynamic.transientPreservation,
        features.dynamic.compressionAmount, features.dynamic.attackMilliseconds, features.dynamic.releaseMilliseconds,
        features.dynamic.sustain, features.dynamic.gainReductionEstimateDb, features.dynamic.loudnessRangeDb,
        features.dynamic.integratedLoudnessDb };
    const std::array nonlinear { features.nonlinear.oddEvenBalance, features.nonlinear.intermodulationIndicator,
        features.nonlinear.saturationOnset, features.nonlinear.clippingAsymmetry,
        features.nonlinear.levelDependentSpectralChange };
    const std::array spatial { features.spatial.stereoWidth, features.spatial.roomReverbEstimate,
        features.spatial.doubleTrackingLikelihood, features.spatial.panning, features.spatial.modulation,
        features.spatial.delayPresence, features.spatial.channelCorrelation };
    object->setProperty("spectral", floatArray(spectral));
    object->setProperty("dynamic", floatArray(dynamics));
    object->setProperty("harmonics", floatArray(features.nonlinear.harmonicDistribution));
    object->setProperty("nonlinear", floatArray(nonlinear));
    object->setProperty("spatial", floatArray(spatial));
    object->setProperty("durationSeconds", features.durationSeconds);
    object->setProperty("peak", features.peak); object->setProperty("rms", features.rms);
    object->setProperty("zeroCrossingRate", features.zeroCrossingRate);
    return juce::var(object);
}

juce::var descriptorVar(const Descriptor& descriptor)
{
    juce::Array<juce::var> array; array.add(descriptor.value); array.add(descriptor.uncertainty); return array;
}

juce::var reportVar(const ToneReport& report)
{
    auto* object = new juce::DynamicObject();
    object->setProperty("instrument", juce::String(toString(report.context.instrument).data()));
    object->setProperty("technique", juce::String(toString(report.context.technique).data()));
    object->setProperty("gainCategory", juce::String(toString(report.context.gainCategory).data()));
    const std::array context { report.context.instrumentConfidence, report.context.techniqueConfidence,
        report.context.cleanDistortedConfidence, report.context.approximateRegisterHz };
    object->setProperty("context", floatArray(context));
    object->setProperty("gain", descriptorVar(report.gain));
    object->setProperty("brightness", descriptorVar(report.brightness));
    object->setProperty("tightness", descriptorVar(report.tightness));
    object->setProperty("compression", descriptorVar(report.compression));
    object->setProperty("cleanLowBlendEstimate", descriptorVar(report.cleanLowBlendEstimate));
    object->setProperty("cabinetDarkness", descriptorVar(report.cabinetDarkness));
    object->setProperty("roomAmount", descriptorVar(report.roomAmount));
    const std::array confidence { report.confidence.aggregate, report.confidence.signalToLeakage,
        report.confidence.classifierCertainty, report.confidence.duration, report.confidence.spectralCoverage,
        report.confidence.polyphony, report.confidence.separationQuality, report.confidence.modelDomainProximity };
    object->setProperty("confidence", floatArray(confidence));
    juce::Array<juce::var> warnings;
    for (const auto& warning : report.warnings) warnings.add(juce::String(warning));
    object->setProperty("warnings", warnings);
    return juce::var(object);
}

bool copyArray(const juce::var& value, std::span<float> destination)
{
    const auto* array = value.getArray();
    if (array == nullptr || array->size() != static_cast<int>(destination.size())) return false;
    for (std::size_t index = 0; index < destination.size(); ++index)
    {
        destination[index] = static_cast<float>(array->getReference(static_cast<int>(index)));
        if (!std::isfinite(destination[index])) return false;
    }
    return true;
}

Descriptor descriptorFromVar(const juce::var& value)
{
    Descriptor result;
    if (const auto* array = value.getArray(); array != nullptr && array->size() == 2)
    { result.value = static_cast<float>((*array)[0]); result.uncertainty = static_cast<float>((*array)[1]); }
    return result;
}
} // namespace

SimilarityBreakdown ToneSimilarity::compare(const ToneProfile& left, const ToneProfile& right) noexcept
{
    SimilarityBreakdown result;
    float dot {}, leftEnergy {}, rightEnergy {};
    for (std::size_t index = 0; index < toneEmbeddingDimensions; ++index)
    {
        dot += left.embedding.values[index] * right.embedding.values[index];
        leftEnergy += left.embedding.values[index] * left.embedding.values[index];
        rightEnergy += right.embedding.values[index] * right.embedding.values[index];
    }
    const auto cosine = dot / std::sqrt(std::max(leftEnergy * rightEnergy, 1.0e-12f));
    result.embedding = clamp01((cosine + 1.0f) * 0.5f);
    const auto& ls = left.features.spectral; const auto& rs = right.features.spectral;
    const auto spectralDistance = (distance(ls.lowMidBuildup, rs.lowMidBuildup)
        + distance(ls.midEmphasis, rs.midEmphasis) + distance(ls.upperMidAttack, rs.upperMidAttack)
        + distance(ls.highFrequencyRolloff, rs.highFrequencyRolloff)
        + distance(ls.spectralSlopeDbPerOctave, rs.spectralSlopeDbPerOctave, 24.0f)
        + distance(ls.spectralFlatness, rs.spectralFlatness)) / 6.0f;
    result.spectral = 1.0f - spectralDistance;
    const auto& ld = left.features.dynamic; const auto& rd = right.features.dynamic;
    result.dynamics = 1.0f - (distance(ld.crestFactorDb, rd.crestFactorDb, 18.0f)
        + distance(ld.compressionAmount, rd.compressionAmount) + distance(ld.sustain, rd.sustain)
        + distance(ld.loudnessRangeDb, rd.loudnessRangeDb, 24.0f)) * 0.25f;
    result.transient = 1.0f - (distance(ld.transientPreservation, rd.transientPreservation)
        + distance(ld.attackMilliseconds, rd.attackMilliseconds, 40.0f)) * 0.5f;
    result.lowFrequency = 1.0f - (distance(ls.lowFrequencyExtension, rs.lowFrequencyExtension)
        + distance(ls.lowMidBuildup, rs.lowMidBuildup)) * 0.5f;
    const auto& ln = left.features.nonlinear; const auto& rn = right.features.nonlinear;
    float harmonicDistance {};
    for (std::size_t index = 0; index < ln.harmonicDistribution.size(); ++index)
        harmonicDistance += distance(ln.harmonicDistribution[index], rn.harmonicDistribution[index], 0.025f);
    harmonicDistance /= static_cast<float>(ln.harmonicDistribution.size());
    result.nonlinear = 1.0f - (harmonicDistance * 0.42f
        + distance(ln.saturationOnset, rn.saturationOnset, 0.15f) * 0.28f
        + distance(ln.levelDependentSpectralChange, rn.levelDependentSpectralChange, 0.15f) * 0.18f
        + distance(ln.oddEvenBalance, rn.oddEvenBalance, 0.25f) * 0.12f);
    const auto bass = left.instrument == Instrument::bass || right.instrument == Instrument::bass;
    const auto embeddingWeight = bass ? 0.30f : 0.30f;
    const auto spectralWeight = bass ? 0.07f : 0.07f;
    const auto dynamicsWeight = bass ? 0.12f : 0.10f;
    const auto transientWeight = bass ? 0.07f : 0.06f;
    const auto lowWeight = bass ? 0.14f : 0.05f;
    const auto nonlinearWeight = 1.0f - embeddingWeight - spectralWeight - dynamicsWeight
                               - transientWeight - lowWeight;
    result.score = clamp01(result.embedding * embeddingWeight + result.spectral * spectralWeight
        + result.dynamics * dynamicsWeight + result.transient * transientWeight
        + result.lowFrequency * lowWeight + result.nonlinear * nonlinearWeight);
    return result;
}

bool ToneProfileDatabase::addOrReplace(ToneProfile profile, std::string& error)
{
    if (profile.profileId.empty() || profile.name.empty()) { error = "Tone profile id and name are required"; return false; }
    if (profile.embedding.version.empty()) { error = "Tone profile embedding must be versioned"; return false; }
    if (!std::all_of(profile.embedding.values.begin(), profile.embedding.values.end(),
                     [](float value) { return std::isfinite(value); }))
    { error = "Tone profile embedding contains a non-finite value"; return false; }
    profile.captureQuality = clamp01(profile.captureQuality);
    const auto found = std::find_if(profiles.begin(), profiles.end(), [&](const auto& existing)
    { return existing.profileId == profile.profileId; });
    if (found == profiles.end()) profiles.push_back(std::move(profile)); else *found = std::move(profile);
    error.clear(); return true;
}

std::optional<ToneProfile> ToneProfileDatabase::find(std::string_view profileId) const
{
    const auto found = std::find_if(profiles.begin(), profiles.end(), [&](const auto& profile)
    { return profile.profileId == profileId; });
    return found == profiles.end() ? std::nullopt : std::optional<ToneProfile>(*found);
}

std::vector<SearchResult> ToneProfileDatabase::search(const ToneProfile& query, std::size_t limit) const
{
    std::vector<SearchResult> results;
    results.reserve(profiles.size());
    for (const auto& profile : profiles)
    {
        if (profile.profileId == query.profileId) continue;
        results.push_back({ profile.profileId, profile.name, ToneSimilarity::compare(query, profile) });
    }
    std::sort(results.begin(), results.end(), [](const auto& left, const auto& right)
    { return left.similarity.score > right.similarity.score; });
    if (results.size() > limit) results.resize(limit);
    return results;
}

std::string ToneProfileDatabase::serialize(bool pretty) const
{
    auto* root = new juce::DynamicObject(); root->setProperty("schemaVersion", 1);
    juce::Array<juce::var> entries;
    for (const auto& profile : profiles)
    {
        auto* object = new juce::DynamicObject();
        object->setProperty("profileId", juce::String(profile.profileId));
        object->setProperty("name", juce::String(profile.name));
        object->setProperty("embeddingVersion", juce::String(profile.embedding.version));
        object->setProperty("embedding", floatArray(profile.embedding.values));
        object->setProperty("features", featuresVar(profile.features));
        object->setProperty("report", reportVar(profile.report));
        object->setProperty("instrument", juce::String(toString(profile.instrument).data()));
        object->setProperty("sourceType", juce::String(toString(profile.sourceType).data()));
        object->setProperty("captureQuality", profile.captureQuality);
        object->setProperty("modelVersion", juce::String(profile.modelVersion));
        juce::Array<juce::var> tags; for (const auto& tag : profile.userTags) tags.add(juce::String(tag));
        object->setProperty("userTags", tags);
        object->setProperty("licensingMetadata", juce::String(profile.licensingMetadata));
        entries.add(juce::var(object));
    }
    root->setProperty("profiles", entries);
    return juce::JSON::toString(juce::var(root), pretty).toStdString();
}

bool ToneProfileDatabase::deserialize(std::string_view json, std::string& error)
{
    juce::var parsed;
    const auto status = juce::JSON::parse(juce::String::fromUTF8(json.data(), static_cast<int>(json.size())), parsed);
    if (status.failed() || !parsed.isObject()) { error = "Invalid tone profile JSON: " + status.getErrorMessage().toStdString(); return false; }
    const auto* root = parsed.getDynamicObject();
    if (static_cast<int>(root->getProperty("schemaVersion")) != 1) { error = "Unsupported tone database schema"; return false; }
    const auto* entries = root->getProperty("profiles").getArray();
    if (entries == nullptr) { error = "Tone database has no profiles array"; return false; }
    std::vector<ToneProfile> loaded;
    std::unordered_set<std::string> ids;
    for (const auto& value : *entries)
    {
        const auto* object = value.getDynamicObject(); if (object == nullptr) { error = "Invalid tone profile entry"; return false; }
        ToneProfile profile;
        profile.profileId = object->getProperty("profileId").toString().toStdString();
        profile.name = object->getProperty("name").toString().toStdString();
        profile.embedding.version = object->getProperty("embeddingVersion").toString().toStdString();
        if (!ids.insert(profile.profileId).second || profile.profileId.empty()) { error = "Duplicate or empty tone profile id"; return false; }
        if (!copyArray(object->getProperty("embedding"), profile.embedding.values)) { error = "Invalid tone embedding"; return false; }
        const auto* features = object->getProperty("features").getDynamicObject();
        const auto* report = object->getProperty("report").getDynamicObject();
        if (features == nullptr || report == nullptr) { error = "Tone profile is missing features or report"; return false; }
        std::array<float, 9> spectral {}, dynamics {};
        std::array<float, 5> nonlinear {}; std::array<float, 7> spatial {};
        if (!copyArray(features->getProperty("spectral"), spectral)
            || !copyArray(features->getProperty("dynamic"), dynamics)
            || !copyArray(features->getProperty("harmonics"), profile.features.nonlinear.harmonicDistribution)
            || !copyArray(features->getProperty("nonlinear"), nonlinear)
            || !copyArray(features->getProperty("spatial"), spatial))
        { error = "Invalid tone feature dimensions"; return false; }
        auto& s = profile.features.spectral;
        s = { spectral[0], spectral[1], spectral[2], spectral[3], spectral[4], spectral[5], spectral[6], spectral[7], spectral[8] };
        auto& d = profile.features.dynamic;
        d = { dynamics[0], dynamics[1], dynamics[2], dynamics[3], dynamics[4], dynamics[5], dynamics[6], dynamics[7], dynamics[8] };
        profile.features.nonlinear.oddEvenBalance = nonlinear[0];
        profile.features.nonlinear.intermodulationIndicator = nonlinear[1];
        profile.features.nonlinear.saturationOnset = nonlinear[2];
        profile.features.nonlinear.clippingAsymmetry = nonlinear[3];
        profile.features.nonlinear.levelDependentSpectralChange = nonlinear[4];
        profile.features.spatial = { spatial[0], spatial[1], spatial[2], spatial[3], spatial[4], spatial[5], spatial[6] };
        profile.features.durationSeconds = static_cast<float>(features->getProperty("durationSeconds"));
        profile.features.peak = static_cast<float>(features->getProperty("peak"));
        profile.features.rms = static_cast<float>(features->getProperty("rms"));
        profile.features.zeroCrossingRate = static_cast<float>(features->getProperty("zeroCrossingRate"));
        profile.instrument = instrumentFromString(object->getProperty("instrument").toString());
        profile.sourceType = sourceFromString(object->getProperty("sourceType").toString());
        profile.captureQuality = static_cast<float>(object->getProperty("captureQuality"));
        profile.modelVersion = object->getProperty("modelVersion").toString().toStdString();
        profile.licensingMetadata = object->getProperty("licensingMetadata").toString().toStdString();
        if (const auto* tags = object->getProperty("userTags").getArray())
            for (const auto& tag : *tags) profile.userTags.push_back(tag.toString().toStdString());
        profile.report.context.instrument = instrumentFromString(report->getProperty("instrument").toString());
        profile.report.context.technique = techniqueFromString(report->getProperty("technique").toString());
        profile.report.context.gainCategory = gainFromString(report->getProperty("gainCategory").toString());
        std::array<float, 4> context {}; std::array<float, 8> confidence {};
        if (!copyArray(report->getProperty("context"), context) || !copyArray(report->getProperty("confidence"), confidence))
        { error = "Invalid report context or confidence"; return false; }
        profile.report.context.instrumentConfidence = context[0]; profile.report.context.techniqueConfidence = context[1];
        profile.report.context.cleanDistortedConfidence = context[2]; profile.report.context.approximateRegisterHz = context[3];
        profile.report.gain = descriptorFromVar(report->getProperty("gain"));
        profile.report.brightness = descriptorFromVar(report->getProperty("brightness"));
        profile.report.tightness = descriptorFromVar(report->getProperty("tightness"));
        profile.report.compression = descriptorFromVar(report->getProperty("compression"));
        profile.report.cleanLowBlendEstimate = descriptorFromVar(report->getProperty("cleanLowBlendEstimate"));
        profile.report.cabinetDarkness = descriptorFromVar(report->getProperty("cabinetDarkness"));
        profile.report.roomAmount = descriptorFromVar(report->getProperty("roomAmount"));
        profile.report.confidence = { confidence[0], confidence[1], confidence[2], confidence[3], confidence[4], confidence[5], confidence[6], confidence[7] };
        if (const auto* warnings = report->getProperty("warnings").getArray())
            for (const auto& warning : *warnings) profile.report.warnings.push_back(warning.toString().toStdString());
        loaded.push_back(std::move(profile));
    }
    profiles = std::move(loaded); error.clear(); return true;
}
} // namespace nts::tone
