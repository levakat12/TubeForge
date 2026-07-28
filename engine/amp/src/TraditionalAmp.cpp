#include "nts/amp/TraditionalAmp.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <numbers>
#include <sstream>

namespace nts::amp
{
namespace
{
template <typename Value>
Value clamp01(Value value) noexcept { return std::clamp(value, Value {}, Value { 1 }); }

float coefficient(double sampleRate, double milliseconds) noexcept
{
    return static_cast<float>(std::exp(-1.0 / (std::max(1.0, sampleRate)
        * std::max(0.01, milliseconds) * 0.001)));
}

dsp::OversamplingFactor factorFromInt(int factor) noexcept
{
    if (factor >= 8) return dsp::OversamplingFactor::x8;
    if (factor >= 4) return dsp::OversamplingFactor::x4;
    if (factor >= 2) return dsp::OversamplingFactor::x2;
    return dsp::OversamplingFactor::x1;
}

std::optional<double> jsonNumber(std::string_view json, std::string_view key)
{
    const auto marker = std::string("\"") + std::string(key) + "\"";
    auto position = json.find(marker);
    if (position == std::string_view::npos) return std::nullopt;
    position = json.find(':', position + marker.size());
    if (position == std::string_view::npos) return std::nullopt;
    ++position;
    while (position < json.size() && (json[position] == ' ' || json[position] == '\n' || json[position] == '\r')) ++position;
    const auto start = position;
    while (position < json.size() && (std::isdigit(static_cast<unsigned char>(json[position]))
           || json[position] == '-' || json[position] == '+' || json[position] == '.'
           || json[position] == 'e' || json[position] == 'E')) ++position;
    double value {};
    const auto text = json.substr(start, position - start);
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc {} ? std::optional<double> { value } : std::nullopt;
}

std::optional<std::string> jsonString(std::string_view json, std::string_view key)
{
    const auto marker = std::string("\"") + std::string(key) + "\"";
    auto position = json.find(marker);
    if (position == std::string_view::npos) return std::nullopt;
    position = json.find(':', position + marker.size());
    if (position == std::string_view::npos) return std::nullopt;
    position = json.find('"', position + 1);
    if (position == std::string_view::npos) return std::nullopt;
    const auto end = json.find('"', position + 1);
    if (end == std::string_view::npos) return std::nullopt;
    return std::string(json.substr(position + 1, end - position - 1));
}

bool jsonBool(std::string_view json, std::string_view key, bool fallback)
{
    const auto marker = std::string("\"") + std::string(key) + "\"";
    auto position = json.find(marker);
    if (position == std::string_view::npos) return fallback;
    position = json.find(':', position + marker.size());
    if (position == std::string_view::npos) return fallback;
    const auto tail = json.substr(position + 1);
    return tail.find("true") < tail.find("false");
}

float numberOr(std::string_view json, std::string_view key, float fallback)
{
    const auto value = jsonNumber(json, key);
    return value ? static_cast<float>(*value) : fallback;
}
} // namespace

void InputCalibrator::prepare(const dsp::ProcessSpec& spec) noexcept
{
    sampleRate = std::max(1.0, spec.sampleRate);
    reset();
}

void InputCalibrator::reset() noexcept
{
    smoothedEnergy = 0.0;
    lastReading = {};
}

void InputCalibrator::process(const float* const* channels, std::size_t channelCount,
                              std::size_t samples) noexcept
{
    const auto count = std::min(channelCount, dsp::maximumChannels);
    float peak {};
    double energy {};
    for (std::size_t sample = 0; sample < samples; ++sample)
        for (std::size_t channel = 0; channel < count; ++channel)
        {
            const auto value = channels[channel][sample];
            peak = std::max(peak, std::abs(value));
            energy += static_cast<double>(value) * value;
        }
    const auto mean = energy / static_cast<double>(std::max<std::size_t>(1, samples * count));
    const auto smoothing = std::exp(-static_cast<double>(samples) / (sampleRate * 0.4));
    smoothedEnergy = mean + smoothing * (smoothedEnergy - mean);
    lastReading.peakDb = dsp::linearToDb(std::max(peak, 1.0e-6f));
    lastReading.rmsDb = dsp::linearToDb(static_cast<float>(std::sqrt(std::max(1.0e-12, smoothedEnergy))));
    const auto targetRms = 0.5f * (calibration.targetRmsLowDb + calibration.targetRmsHighDb);
    const auto rmsTrim = targetRms - lastReading.rmsDb;
    const auto peakTrim = calibration.targetPeakDb - lastReading.peakDb;
    lastReading.suggestedTrimDb = std::clamp(std::min(rmsTrim, peakTrim + 3.0f), -24.0f, 24.0f);
}

AmpPreset makeOriginalPreset(Topology topology, Instrument instrument)
{
    AmpPreset preset;
    preset.parameters.topology = topology;
    preset.parameters.instrument = instrument;
    preset.parameters.pickup = PickupProfile::passive;
    preset.calibration = instrument == Instrument::guitar
        ? CalibrationProfile { -24.0f, -18.0f, -12.0f, PickupProfile::passive }
        : CalibrationProfile { -26.0f, -19.0f, -10.0f, PickupProfile::passive };
    auto& p = preset.parameters;
    if (topology == Topology::tightModern)
    {
        preset.name = instrument == Instrument::guitar ? "Original Tight Guitar" : "Original Tight Bass";
        p.stageCount = instrument == Instrument::guitar ? 4 : 3;
        p.preEq = { instrument == Instrument::guitar ? 95.0f : 48.0f, 16500.0f, 0.72f,
                    2.5f, true, -1.5f, true, 1.5f };
        for (std::size_t index = 0; index < p.stages.size(); ++index)
        {
            p.stages[index] = { 11.0f + static_cast<float>(index) * 2.5f,
                index % 2 == 0 ? 0.04f : -0.025f, 0.12f + 0.03f * static_cast<float>(index),
                instrument == Instrument::guitar ? 90.0f : 45.0f, 12500.0f - 800.0f * index,
                -7.0f, 4, 0.28f, 0.28f, 0.22f, 0.32f };
        }
        p.toneStack = { instrument == Instrument::bass ? ToneStackType::bassSemiParametric
                                                       : ToneStackType::passiveCoupled,
                        0.5f, 0.56f, 0.58f, instrument == Instrument::bass ? 650.0f : 850.0f, 0.85f };
        p.phaseInverter = { 1.7f, 0.78f, 0.14f, 0.06f, 0.3f };
        p.powerAmp = { -4.0f, 0.48f, 0.7f, 0.3f, 0.03f, 0.58f, 0.55f, 0.4f, 28.0f, 360.0f };
    }
    else
    {
        preset.name = instrument == Instrument::guitar ? "Original Vintage Bloom" : "Original Bass Bloom";
        p.stageCount = 2;
        p.preEq = { instrument == Instrument::guitar ? 62.0f : 38.0f, 19000.0f, 0.25f,
                    -1.0f, false, 0.0f, true, -1.0f };
        p.stages[0] = { 14.0f, -0.06f, 0.18f, 55.0f, 15000.0f, -5.0f, 4, 0.4f, 0.18f, 0.12f, 0.5f };
        p.stages[1] = { 10.0f, 0.08f, 0.24f, 48.0f, 13000.0f, -4.0f, 4, 0.45f, 0.22f, 0.1f, 0.58f };
        p.toneStack = { instrument == Instrument::bass ? ToneStackType::bassSemiParametric
                                                       : ToneStackType::passiveCoupled,
                        0.62f, 0.45f, 0.52f, 520.0f, 0.7f };
        p.phaseInverter = { 1.35f, 0.9f, 0.2f, 0.09f, 0.18f };
        p.powerAmp = { -1.5f, 0.62f, 0.42f, 0.62f, -0.08f, 0.48f, 0.65f, 0.22f, 55.0f, 650.0f };
    }
    if (instrument == Instrument::bass)
    {
        p.bass = { 170.0f, 0.62f, 0.52f, 5.0f, 1.0f, false };
        p.cabinet.bassDiBlend = 0.25f;
        p.outputGainDb = -5.0f;
    }
    return preset;
}

std::string serializePresetLegacy(const AmpPreset& preset, bool pretty)
{
    const auto separator = pretty ? "\n  " : "";
    std::ostringstream stream;
    stream << std::setprecision(8) << '{' << separator
           << "\"schemaVersion\":" << preset.schemaVersion << ',' << separator
           << "\"name\":\"" << preset.name << "\"," << separator
           << "\"instrument\":\"" << (preset.parameters.instrument == Instrument::guitar ? "guitar" : "bass") << "\"," << separator
           << "\"topology\":\"" << (preset.parameters.topology == Topology::tightModern ? "tightModern" : "vintageBloom") << "\"," << separator
           << "\"pickup\":\"" << (preset.parameters.pickup == PickupProfile::passive ? "passive" : "active") << "\"," << separator
           << "\"inputCalibrationDb\":" << preset.parameters.manualInputTrimDb << ',' << separator
           << "\"targetRmsLowDb\":" << preset.calibration.targetRmsLowDb << ',' << separator
           << "\"targetRmsHighDb\":" << preset.calibration.targetRmsHighDb << ',' << separator
           << "\"targetPeakDb\":" << preset.calibration.targetPeakDb << ',' << separator
           << "\"lowCutHz\":" << preset.parameters.preEq.lowCutHz << ',' << separator
           << "\"highCutHz\":" << preset.parameters.preEq.highCutHz << ',' << separator
           << "\"tightness\":" << preset.parameters.preEq.tightness << ',' << separator
           << "\"pickEmphasisDb\":" << preset.parameters.preEq.pickEmphasisDb << ',' << separator
           << "\"stageCount\":" << preset.parameters.stageCount << ',' << separator;
    for (std::size_t stage = 0; stage < preset.parameters.stages.size(); ++stage)
    {
        const auto& config = preset.parameters.stages[stage];
        stream << "\"stage" << stage << "DriveDb\":" << config.driveDb << ',' << separator
               << "\"stage" << stage << "Bias\":" << config.bias << ',' << separator
               << "\"stage" << stage << "Asymmetry\":" << config.asymmetry << ',' << separator
               << "\"stage" << stage << "Oversampling\":" << config.oversamplingFactor << ',' << separator;
    }
    stream << "\"bass\":" << preset.parameters.toneStack.bass << ',' << separator
           << "\"mid\":" << preset.parameters.toneStack.mid << ',' << separator
           << "\"treble\":" << preset.parameters.toneStack.treble << ',' << separator
           << "\"presence\":" << preset.parameters.powerAmp.presence << ',' << separator
           << "\"resonance\":" << preset.parameters.powerAmp.resonance << ',' << separator
           << "\"masterDb\":" << preset.parameters.powerAmp.masterDb << ',' << separator
           << "\"sag\":" << preset.parameters.powerAmp.sag << ',' << separator
           << "\"feedback\":" << preset.parameters.powerAmp.feedback << ',' << separator
           << "\"crossoverHz\":" << preset.parameters.bass.crossoverHz << ',' << separator
           << "\"cleanBlend\":" << preset.parameters.bass.cleanBlend << ',' << separator
           << "\"cabinetBlend\":" << preset.parameters.cabinet.blend << ',' << separator
           << "\"cabinetBypass\":" << (preset.parameters.cabinet.bypass ? "true" : "false") << ',' << separator
           << "\"cabinetAlignment\":" << preset.parameters.cabinet.delaySamplesB << ',' << separator
           << "\"postLowDb\":" << preset.parameters.postLowDb << ',' << separator
           << "\"postMidDb\":" << preset.parameters.postMidDb << ',' << separator
           << "\"postHighDb\":" << preset.parameters.postHighDb << ',' << separator
           << "\"outputGainDb\":" << preset.parameters.outputGainDb << (pretty ? "\n" : "") << '}';
    return stream.str();
}

std::string serializePreset(const AmpPreset& preset, bool pretty)
{
    const auto newline = pretty ? "\n" : "";
    const auto indent = pretty ? "  " : "";
    const auto& p = preset.parameters;
    std::ostringstream stream;
    stream << std::setprecision(8) << '{' << newline
           << indent << "\"schemaVersion\":" << preset.schemaVersion << ',' << newline
           << indent << "\"name\":\"" << preset.name << "\"," << newline
           << indent << "\"instrument\":\"" << (p.instrument == Instrument::guitar ? "guitar" : "bass") << "\"," << newline
           << indent << "\"topology\":\"" << (p.topology == Topology::tightModern ? "tightModern" : "vintageBloom") << "\"," << newline
           << indent << "\"pickup\":\"" << (p.pickup == PickupProfile::passive ? "passive" : "active") << "\"," << newline
           << indent << "\"inputCalibration\":{"
           << "\"inputCalibrationDb\":" << p.manualInputTrimDb
           << ",\"targetRmsLowDb\":" << preset.calibration.targetRmsLowDb
           << ",\"targetRmsHighDb\":" << preset.calibration.targetRmsHighDb
           << ",\"targetPeakDb\":" << preset.calibration.targetPeakDb << "}," << newline
           << indent << "\"preEq\":{"
           << "\"lowCutHz\":" << p.preEq.lowCutHz << ",\"highCutHz\":" << p.preEq.highCutHz
           << ",\"tightness\":" << p.preEq.tightness << ",\"pickEmphasisDb\":" << p.preEq.pickEmphasisDb
           << ",\"lowShelfEnabled\":" << (p.preEq.lowShelfEnabled ? "true" : "false")
           << ",\"lowShelfDb\":" << p.preEq.lowShelfDb
           << ",\"midEmphasisEnabled\":" << (p.preEq.midEmphasisEnabled ? "true" : "false")
           << ",\"midEmphasisDb\":" << p.preEq.midEmphasisDb << "}," << newline
           << indent << "\"preampStages\":{\"stageCount\":" << p.stageCount;
    for (std::size_t stage = 0; stage < p.stages.size(); ++stage)
    {
        const auto& config = p.stages[stage];
        const auto prefix = std::string("stage") + std::to_string(stage);
        stream << ",\"" << prefix << "DriveDb\":" << config.driveDb
               << ",\"" << prefix << "Bias\":" << config.bias
               << ",\"" << prefix << "Asymmetry\":" << config.asymmetry
               << ",\"" << prefix << "LowCutHz\":" << config.lowCutHz
               << ",\"" << prefix << "HighCutHz\":" << config.highCutHz
               << ",\"" << prefix << "OutputTrimDb\":" << config.outputTrimDb
               << ",\"" << prefix << "Oversampling\":" << config.oversamplingFactor
               << ",\"" << prefix << "DynamicBias\":" << config.dynamicBias
               << ",\"" << prefix << "FrequencySaturation\":" << config.frequencySaturation
               << ",\"" << prefix << "AttackReduction\":" << config.attackReduction
               << ",\"" << prefix << "MemoryAmount\":" << config.memoryAmount;
    }
    stream << "}," << newline
           << indent << "\"toneStack\":{\"toneStackType\":" << static_cast<int>(p.toneStack.type)
           << ",\"bass\":" << p.toneStack.bass << ",\"mid\":" << p.toneStack.mid
           << ",\"treble\":" << p.toneStack.treble
           << ",\"midFrequencyHz\":" << p.toneStack.midFrequencyHz << ",\"midQ\":" << p.toneStack.midQ << "}," << newline
           << indent << "\"phaseInverter\":{\"piDrive\":" << p.phaseInverter.drive
           << ",\"piHeadroom\":" << p.phaseInverter.headroom
           << ",\"piAsymmetry\":" << p.phaseInverter.asymmetry
           << ",\"differentialImbalance\":" << p.phaseInverter.differentialImbalance
           << ",\"piFeedback\":" << p.phaseInverter.feedback << "}," << newline
           << indent << "\"powerAmp\":{\"masterDb\":" << p.powerAmp.masterDb
           << ",\"saturation\":" << p.powerAmp.saturation << ",\"damping\":" << p.powerAmp.damping
           << ",\"sag\":" << p.powerAmp.sag << ",\"biasCharacter\":" << p.powerAmp.biasCharacter
           << ",\"presence\":" << p.powerAmp.presence << ",\"resonance\":" << p.powerAmp.resonance
           << ",\"feedback\":" << p.powerAmp.feedback << ",\"sagAttackMs\":" << p.powerAmp.sagAttackMs
           << ",\"sagRecoveryMs\":" << p.powerAmp.sagRecoveryMs << "}," << newline
           << indent << "\"cabinet\":{\"cabinetBlend\":" << p.cabinet.blend
           << ",\"phaseInvertB\":" << (p.cabinet.phaseInvertB ? "true" : "false")
           << ",\"cabinetAlignment\":" << p.cabinet.delaySamplesB
           << ",\"cabinetLowCutHz\":" << p.cabinet.lowCutHz
           << ",\"cabinetHighCutHz\":" << p.cabinet.highCutHz
           << ",\"cabinetBypass\":" << (p.cabinet.bypass ? "true" : "false")
           << ",\"bassDiBlend\":" << p.cabinet.bassDiBlend << "}," << newline
           << indent << "\"bassPath\":{\"crossoverHz\":" << p.bass.crossoverHz
           << ",\"cleanBlend\":" << p.bass.cleanBlend << ",\"lowCompression\":" << p.bass.lowCompression
           << ",\"highDriveDb\":" << p.bass.highDriveDb << ",\"lowMono\":" << p.bass.lowMono
           << ",\"lowSaturation\":" << (p.bass.lowSaturation ? "true" : "false") << "}," << newline
           << indent << "\"postEq\":{\"postLowDb\":" << p.postLowDb << ",\"postMidDb\":" << p.postMidDb
           << ",\"postHighDb\":" << p.postHighDb << "}," << newline
           << indent << "\"outputGainDb\":" << p.outputGainDb << ',' << newline
           << indent << "\"loudnessMatch\":" << (p.loudnessMatch ? "true" : "false") << newline << '}';
    return stream.str();
}

std::optional<AmpPreset> deserializePreset(std::string_view json)
{
    const auto version = jsonNumber(json, "schemaVersion");
    const auto name = jsonString(json, "name");
    if (! version || static_cast<int>(*version) != 1 || ! name) return std::nullopt;
    const auto instrumentName = jsonString(json, "instrument").value_or("guitar");
    const auto topologyName = jsonString(json, "topology").value_or("tightModern");
    auto preset = makeOriginalPreset(topologyName == "vintageBloom" ? Topology::vintageBloom : Topology::tightModern,
                                     instrumentName == "bass" ? Instrument::bass : Instrument::guitar);
    preset.name = *name;
    auto& p = preset.parameters;
    p.pickup = jsonString(json, "pickup").value_or("passive") == "active" ? PickupProfile::active : PickupProfile::passive;
    p.manualInputTrimDb = numberOr(json, "inputCalibrationDb", p.manualInputTrimDb);
    preset.calibration.targetRmsLowDb = numberOr(json, "targetRmsLowDb", preset.calibration.targetRmsLowDb);
    preset.calibration.targetRmsHighDb = numberOr(json, "targetRmsHighDb", preset.calibration.targetRmsHighDb);
    preset.calibration.targetPeakDb = numberOr(json, "targetPeakDb", preset.calibration.targetPeakDb);
    p.preEq.lowCutHz = numberOr(json, "lowCutHz", p.preEq.lowCutHz);
    p.preEq.highCutHz = numberOr(json, "highCutHz", p.preEq.highCutHz);
    p.preEq.tightness = numberOr(json, "tightness", p.preEq.tightness);
    p.preEq.pickEmphasisDb = numberOr(json, "pickEmphasisDb", p.preEq.pickEmphasisDb);
    p.preEq.lowShelfEnabled = jsonBool(json, "lowShelfEnabled", p.preEq.lowShelfEnabled);
    p.preEq.lowShelfDb = numberOr(json, "lowShelfDb", p.preEq.lowShelfDb);
    p.preEq.midEmphasisEnabled = jsonBool(json, "midEmphasisEnabled", p.preEq.midEmphasisEnabled);
    p.preEq.midEmphasisDb = numberOr(json, "midEmphasisDb", p.preEq.midEmphasisDb);
    p.stageCount = static_cast<std::size_t>(std::clamp(numberOr(json, "stageCount", static_cast<float>(p.stageCount)), 2.0f, 4.0f));
    for (std::size_t stage = 0; stage < p.stages.size(); ++stage)
    {
        p.stages[stage].driveDb = numberOr(json, "stage" + std::to_string(stage) + "DriveDb", p.stages[stage].driveDb);
        p.stages[stage].bias = numberOr(json, "stage" + std::to_string(stage) + "Bias", p.stages[stage].bias);
        p.stages[stage].asymmetry = numberOr(json, "stage" + std::to_string(stage) + "Asymmetry", p.stages[stage].asymmetry);
        p.stages[stage].lowCutHz = numberOr(json, "stage" + std::to_string(stage) + "LowCutHz", p.stages[stage].lowCutHz);
        p.stages[stage].highCutHz = numberOr(json, "stage" + std::to_string(stage) + "HighCutHz", p.stages[stage].highCutHz);
        p.stages[stage].outputTrimDb = numberOr(json, "stage" + std::to_string(stage) + "OutputTrimDb", p.stages[stage].outputTrimDb);
        p.stages[stage].oversamplingFactor = static_cast<int>(numberOr(json, "stage" + std::to_string(stage) + "Oversampling", static_cast<float>(p.stages[stage].oversamplingFactor)));
        p.stages[stage].dynamicBias = numberOr(json, "stage" + std::to_string(stage) + "DynamicBias", p.stages[stage].dynamicBias);
        p.stages[stage].frequencySaturation = numberOr(json, "stage" + std::to_string(stage) + "FrequencySaturation", p.stages[stage].frequencySaturation);
        p.stages[stage].attackReduction = numberOr(json, "stage" + std::to_string(stage) + "AttackReduction", p.stages[stage].attackReduction);
        p.stages[stage].memoryAmount = numberOr(json, "stage" + std::to_string(stage) + "MemoryAmount", p.stages[stage].memoryAmount);
    }
    p.toneStack.type = static_cast<ToneStackType>(std::clamp(static_cast<int>(numberOr(json, "toneStackType", static_cast<float>(p.toneStack.type))), 0, 2));
    p.toneStack.bass = numberOr(json, "bass", p.toneStack.bass);
    p.toneStack.mid = numberOr(json, "mid", p.toneStack.mid);
    p.toneStack.treble = numberOr(json, "treble", p.toneStack.treble);
    p.toneStack.midFrequencyHz = numberOr(json, "midFrequencyHz", p.toneStack.midFrequencyHz);
    p.toneStack.midQ = numberOr(json, "midQ", p.toneStack.midQ);
    p.phaseInverter.drive = numberOr(json, "piDrive", p.phaseInverter.drive);
    p.phaseInverter.headroom = numberOr(json, "piHeadroom", p.phaseInverter.headroom);
    p.phaseInverter.asymmetry = numberOr(json, "piAsymmetry", p.phaseInverter.asymmetry);
    p.phaseInverter.differentialImbalance = numberOr(json, "differentialImbalance", p.phaseInverter.differentialImbalance);
    p.phaseInverter.feedback = numberOr(json, "piFeedback", p.phaseInverter.feedback);
    p.powerAmp.presence = numberOr(json, "presence", p.powerAmp.presence);
    p.powerAmp.resonance = numberOr(json, "resonance", p.powerAmp.resonance);
    p.powerAmp.masterDb = numberOr(json, "masterDb", p.powerAmp.masterDb);
    p.powerAmp.sag = numberOr(json, "sag", p.powerAmp.sag);
    p.powerAmp.feedback = numberOr(json, "feedback", p.powerAmp.feedback);
    p.powerAmp.saturation = numberOr(json, "saturation", p.powerAmp.saturation);
    p.powerAmp.damping = numberOr(json, "damping", p.powerAmp.damping);
    p.powerAmp.biasCharacter = numberOr(json, "biasCharacter", p.powerAmp.biasCharacter);
    p.powerAmp.sagAttackMs = numberOr(json, "sagAttackMs", p.powerAmp.sagAttackMs);
    p.powerAmp.sagRecoveryMs = numberOr(json, "sagRecoveryMs", p.powerAmp.sagRecoveryMs);
    p.bass.crossoverHz = numberOr(json, "crossoverHz", p.bass.crossoverHz);
    p.bass.cleanBlend = numberOr(json, "cleanBlend", p.bass.cleanBlend);
    p.cabinet.blend = numberOr(json, "cabinetBlend", p.cabinet.blend);
    p.cabinet.bypass = jsonBool(json, "cabinetBypass", p.cabinet.bypass);
    p.cabinet.phaseInvertB = jsonBool(json, "phaseInvertB", p.cabinet.phaseInvertB);
    p.cabinet.delaySamplesB = static_cast<std::size_t>(numberOr(json, "cabinetAlignment", 0.0f));
    p.cabinet.lowCutHz = numberOr(json, "cabinetLowCutHz", p.cabinet.lowCutHz);
    p.cabinet.highCutHz = numberOr(json, "cabinetHighCutHz", p.cabinet.highCutHz);
    p.cabinet.bassDiBlend = numberOr(json, "bassDiBlend", p.cabinet.bassDiBlend);
    p.postLowDb = numberOr(json, "postLowDb", p.postLowDb);
    p.postMidDb = numberOr(json, "postMidDb", p.postMidDb);
    p.postHighDb = numberOr(json, "postHighDb", p.postHighDb);
    p.bass.lowCompression = numberOr(json, "lowCompression", p.bass.lowCompression);
    p.bass.highDriveDb = numberOr(json, "highDriveDb", p.bass.highDriveDb);
    p.bass.lowMono = numberOr(json, "lowMono", p.bass.lowMono);
    p.bass.lowSaturation = jsonBool(json, "lowSaturation", p.bass.lowSaturation);
    p.outputGainDb = numberOr(json, "outputGainDb", p.outputGainDb);
    p.loudnessMatch = jsonBool(json, "loudnessMatch", p.loudnessMatch);
    return preset;
}

void PreEq::prepare(const dsp::ProcessSpec& newSpec) noexcept
{
    spec = newSpec;
    lowCut.prepare(spec); highCut.prepare(spec); pick.prepare(spec); lowShelf.prepare(spec); mid.prepare(spec);
    setParameters({}); reset();
}
void PreEq::reset() noexcept { lowCut.reset(); highCut.reset(); pick.reset(); lowShelf.reset(); mid.reset(); }
void PreEq::setParameters(const PreEqParameters& p, std::size_t interpolationSamples) noexcept
{
    const auto tightCut = 35.0 + 310.0 * clamp01(p.tightness) * clamp01(p.tightness);
    lowCut.setCoefficients(dsp::BiquadCoefficients::make(dsp::FilterType::highPass, spec.sampleRate,
        std::max<double>(p.lowCutHz, tightCut), 0.707), interpolationSamples);
    highCut.setCoefficients(dsp::BiquadCoefficients::make(dsp::FilterType::lowPass, spec.sampleRate,
        p.highCutHz, 0.707), interpolationSamples);
    pick.setCoefficients(dsp::BiquadCoefficients::make(dsp::FilterType::peaking, spec.sampleRate,
        2800.0, 1.1, p.pickEmphasisDb), interpolationSamples);
    lowShelf.setCoefficients(dsp::BiquadCoefficients::make(dsp::FilterType::lowShelf, spec.sampleRate,
        140.0, 0.707, p.lowShelfDb), interpolationSamples);
    mid.setCoefficients(dsp::BiquadCoefficients::make(dsp::FilterType::peaking, spec.sampleRate,
        950.0, 0.9, p.midEmphasisDb), interpolationSamples);
    useLowShelf = p.lowShelfEnabled; useMid = p.midEmphasisEnabled;
}
void PreEq::process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept
{
    lowCut.process(channels, channelCount, samples); highCut.process(channels, channelCount, samples);
    pick.process(channels, channelCount, samples);
    if (useLowShelf) lowShelf.process(channels, channelCount, samples);
    if (useMid) mid.process(channels, channelCount, samples);
}

void ResponsivePreampStage::prepare(const dsp::ProcessSpec& newSpec)
{
    spec = newSpec;
    lowCut.prepare(spec); highCut.prepare(spec); dcBlock.prepare(spec);
    const std::array factors { dsp::OversamplingFactor::x1, dsp::OversamplingFactor::x2,
                               dsp::OversamplingFactor::x4, dsp::OversamplingFactor::x8 };
    for (std::size_t index = 0; index < oversamplers.size(); ++index) oversamplers[index].prepare(spec, factors[index]);
    drive.prepare(spec.sampleRate, 8.0, dsp::SmoothingMode::logarithmic);
    trim.prepare(spec.sampleRate, 8.0, dsp::SmoothingMode::logarithmic);
    setConfig(config, 0); reset();
}
void ResponsivePreampStage::reset() noexcept
{
    lowCut.reset(); highCut.reset(); dcBlock.reset();
    for (auto& oversampler : oversamplers) oversampler.reset();
    envelope.fill(0.0f); previousEnvelope.fill(0.0f); biasMemory.fill(0.0f);
    recoveryGain.fill(1.0f); frequencyState.fill(0.0f);
    drive.reset(dsp::dbToLinear(config.driveDb)); trim.reset(dsp::dbToLinear(config.outputTrimDb));
}
void ResponsivePreampStage::setConfig(const PreampStageConfig& next, std::size_t interpolationSamples) noexcept
{
    config = next;
    config.driveDb = std::clamp(config.driveDb, -12.0f, 42.0f);
    config.bias = std::clamp(config.bias, -0.8f, 0.8f);
    config.asymmetry = std::clamp(config.asymmetry, -0.8f, 0.8f);
    config.memoryAmount = clamp01(config.memoryAmount);
    config.dynamicBias = clamp01(config.dynamicBias);
    config.frequencySaturation = clamp01(config.frequencySaturation);
    config.attackReduction = clamp01(config.attackReduction);
    config.oversamplingFactor = static_cast<int>(factorFromInt(config.oversamplingFactor));
    lowCut.setCoefficients(dsp::BiquadCoefficients::make(dsp::FilterType::highPass, spec.sampleRate,
        config.lowCutHz, 0.707), interpolationSamples);
    highCut.setCoefficients(dsp::BiquadCoefficients::make(dsp::FilterType::lowPass, spec.sampleRate,
        config.highCutHz, 0.707), interpolationSamples);
    dcBlock.setCoefficients(dsp::BiquadCoefficients::make(dsp::FilterType::highPass, spec.sampleRate,
        12.0, 0.707), interpolationSamples);
    drive.setTarget(dsp::dbToLinear(config.driveDb)); trim.setTarget(dsp::dbToLinear(config.outputTrimDb));
}
dsp::Oversampler& ResponsivePreampStage::selectedOversampler() noexcept
{
    if (config.oversamplingFactor >= 8) return oversamplers[3];
    if (config.oversamplingFactor >= 4) return oversamplers[2];
    if (config.oversamplingFactor >= 2) return oversamplers[1];
    return oversamplers[0];
}
void ResponsivePreampStage::process(float* const* channels, std::size_t channelCount,
                                    std::size_t samples) noexcept
{
    const auto count = std::min(channelCount, dsp::maximumChannels);
    lowCut.process(channels, count, samples); highCut.process(channels, count, samples);
    const auto envAttack = coefficient(spec.sampleRate, 1.2);
    const auto envRelease = coefficient(spec.sampleRate, 75.0);
    const auto biasCoefficient = coefficient(spec.sampleRate, 140.0 + 500.0 * config.memoryAmount);
    const auto recoveryCoefficient = coefficient(spec.sampleRate, 180.0 + 900.0 * config.memoryAmount);
    for (std::size_t sample = 0; sample < samples; ++sample)
    {
        const auto stageDrive = drive.next();
        for (std::size_t channel = 0; channel < count; ++channel)
        {
            const auto input = channels[channel][sample];
            const auto detector = std::abs(input);
            const auto envCoefficient = detector > envelope[channel] ? envAttack : envRelease;
            envelope[channel] = detector + envCoefficient * (envelope[channel] - detector);
            const auto attack = std::max(0.0f, envelope[channel] - previousEnvelope[channel]);
            previousEnvelope[channel] = envelope[channel];
            const auto biasTarget = config.dynamicBias * config.memoryAmount * envelope[channel]
                                  * (input >= 0.0f ? 0.32f : -0.18f);
            biasMemory[channel] = biasTarget + biasCoefficient * (biasMemory[channel] - biasTarget);
            const auto recoveryTarget = std::clamp(1.0f - config.memoryAmount * envelope[channel] * 0.38f,
                                                   0.55f, 1.0f);
            recoveryGain[channel] = recoveryTarget + recoveryCoefficient
                                  * (recoveryGain[channel] - recoveryTarget);
            frequencyState[channel] += 0.08f * (input - frequencyState[channel]);
            const auto high = input - frequencyState[channel];
            const auto transientGain = 1.0f / (1.0f + 22.0f * config.attackReduction * attack);
            channels[channel][sample] = ((input + config.frequencySaturation * high * 0.65f)
                * stageDrive * recoveryGain[channel] * transientGain) + biasMemory[channel];
        }
    }
    selectedOversampler().process(channels, count, samples, [this](float input) noexcept
    {
        const auto biased = input + config.bias;
        const auto polarity = biased >= 0.0f ? 1.0f + config.asymmetry : 1.0f - config.asymmetry;
        return std::tanh(biased * polarity) - std::tanh(config.bias * polarity);
    });
    dcBlock.process(channels, count, samples);
    for (std::size_t sample = 0; sample < samples; ++sample)
    {
        const auto gain = trim.next();
        for (std::size_t channel = 0; channel < count; ++channel) channels[channel][sample] *= gain;
    }
}
float ResponsivePreampStage::biasState(std::size_t channel) const noexcept
{
    return channel < dsp::maximumChannels ? biasMemory[channel] : 0.0f;
}
std::size_t ResponsivePreampStage::latencySamples() const noexcept
{
    return config.oversamplingFactor == 1 ? 0 : 8;
}

void ToneStack::prepare(const dsp::ProcessSpec& newSpec) noexcept
{
    spec = newSpec; low.prepare(spec); middle.prepare(spec); high.prepare(spec); setParameters({}); reset();
}
void ToneStack::reset() noexcept { low.reset(); middle.reset(); high.reset(); }
void ToneStack::setParameters(const ToneStackParameters& next, std::size_t interpolationSamples) noexcept
{
    parameters = next; parameters.bass = clamp01(parameters.bass); parameters.mid = clamp01(parameters.mid);
    parameters.treble = clamp01(parameters.treble);
    double lowGain {}, midGain {}, highGain {};
    double lowFrequency = 120.0, midFrequency = 750.0, highFrequency = 3200.0, midQ = 0.8;
    if (parameters.type == ToneStackType::passiveCoupled)
    {
        lowGain = (parameters.bass - 0.5) * 22.0 - (parameters.mid - 0.5) * 5.0;
        midGain = (parameters.mid - 0.5) * 17.0 - (parameters.bass - 0.5) * 4.0
                - (parameters.treble - 0.5) * 3.0 - 3.0;
        highGain = (parameters.treble - 0.5) * 21.0 - (parameters.mid - 0.5) * 4.5;
        lowFrequency = 105.0 + 55.0 * parameters.mid;
        midFrequency = 560.0 + 520.0 * parameters.treble;
        highFrequency = 2700.0 + 1300.0 * parameters.bass;
        midQ = 0.62 + 0.5 * (1.0 - parameters.bass);
    }
    else if (parameters.type == ToneStackType::activeThreeBand)
    {
        lowGain = (parameters.bass - 0.5) * 30.0;
        midGain = (parameters.mid - 0.5) * 30.0;
        highGain = (parameters.treble - 0.5) * 30.0;
    }
    else
    {
        lowGain = (parameters.bass - 0.5) * 24.0;
        midGain = (parameters.mid - 0.5) * 24.0;
        highGain = (parameters.treble - 0.5) * 22.0;
        midFrequency = std::clamp<double>(parameters.midFrequencyHz, 120.0, 2400.0);
        midQ = std::clamp<double>(parameters.midQ, 0.25, 4.0);
        lowFrequency = 90.0; highFrequency = 2800.0;
    }
    generated[0] = dsp::BiquadCoefficients::make(dsp::FilterType::lowShelf, spec.sampleRate,
                                                  lowFrequency, 0.707, lowGain);
    generated[1] = dsp::BiquadCoefficients::make(dsp::FilterType::peaking, spec.sampleRate,
                                                  midFrequency, midQ, midGain);
    generated[2] = dsp::BiquadCoefficients::make(dsp::FilterType::highShelf, spec.sampleRate,
                                                  highFrequency, 0.707, highGain);
    low.setCoefficients(generated[0], interpolationSamples);
    middle.setCoefficients(generated[1], interpolationSamples);
    high.setCoefficients(generated[2], interpolationSamples);
}
void ToneStack::process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept
{
    low.process(channels, channelCount, samples); middle.process(channels, channelCount, samples);
    high.process(channels, channelCount, samples);
}

void PhaseInverter::prepare(const dsp::ProcessSpec& spec) noexcept
{
    sampleRate = spec.sampleRate;
    lowCut.prepare(spec); shaping.prepare(spec); setParameters(parameters, 0); reset();
}
void PhaseInverter::reset() noexcept { lowCut.reset(); shaping.reset(); feedbackState.fill(0.0f); }
void PhaseInverter::setParameters(const PhaseInverterParameters& next, std::size_t interpolationSamples) noexcept
{
    parameters = next;
    parameters.drive = std::clamp(parameters.drive, 0.1f, 8.0f);
    parameters.headroom = std::clamp(parameters.headroom, 0.15f, 1.5f);
    parameters.asymmetry = std::clamp(parameters.asymmetry, -0.8f, 0.8f);
    parameters.differentialImbalance = std::clamp(parameters.differentialImbalance, -0.4f, 0.4f);
    parameters.feedback = clamp01(parameters.feedback);
    lowCut.setCoefficients(dsp::BiquadCoefficients::make(dsp::FilterType::highPass, sampleRate, 35.0), interpolationSamples);
    shaping.setCoefficients(dsp::BiquadCoefficients::make(dsp::FilterType::peaking, sampleRate, 1800.0, 0.8, 1.5), interpolationSamples);
}
void PhaseInverter::process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept
{
    const auto count = std::min(channelCount, dsp::maximumChannels);
    lowCut.process(channels, count, samples); shaping.process(channels, count, samples);
    for (std::size_t channel = 0; channel < count; ++channel)
        for (std::size_t sample = 0; sample < samples; ++sample)
        {
            const auto input = channels[channel][sample] - parameters.feedback * 0.35f * feedbackState[channel];
            const auto imbalance = channel == 0 ? 1.0f + parameters.differentialImbalance
                                                : 1.0f - parameters.differentialImbalance;
            const auto polarity = input >= 0.0f ? 1.0f + parameters.asymmetry : 1.0f - parameters.asymmetry;
            const auto output = parameters.headroom * std::tanh(input * parameters.drive * imbalance
                                                              * polarity / parameters.headroom);
            feedbackState[channel] = output; channels[channel][sample] = output;
        }
}

void PowerAmp::prepare(const dsp::ProcessSpec& newSpec) noexcept
{
    spec = newSpec; presence.prepare(spec); resonance.prepare(spec);
    master.prepare(spec.sampleRate, 15.0, dsp::SmoothingMode::logarithmic);
    setParameters(parameters, 0); reset();
}
void PowerAmp::reset() noexcept
{
    presence.reset(); resonance.reset(); supply.fill(1.0f); energy.fill(0.0f); feedbackState.fill(0.0f);
    master.reset(dsp::dbToLinear(parameters.masterDb));
}
void PowerAmp::setParameters(const PowerAmpParameters& next, std::size_t interpolationSamples) noexcept
{
    parameters = next;
    parameters.saturation = clamp01(parameters.saturation); parameters.damping = clamp01(parameters.damping);
    parameters.sag = clamp01(parameters.sag); parameters.presence = clamp01(parameters.presence);
    parameters.resonance = clamp01(parameters.resonance); parameters.feedback = clamp01(parameters.feedback);
    parameters.biasCharacter = std::clamp(parameters.biasCharacter, -0.6f, 0.6f);
    master.setTarget(dsp::dbToLinear(std::clamp(parameters.masterDb, -60.0f, 18.0f)));
    sagAttackCoefficient = coefficient(spec.sampleRate, parameters.sagAttackMs);
    sagRecoveryCoefficient = coefficient(spec.sampleRate, parameters.sagRecoveryMs);
    presence.setCoefficients(dsp::BiquadCoefficients::make(dsp::FilterType::highShelf, spec.sampleRate,
        2600.0, 0.707, (parameters.presence - 0.5f) * 12.0f), interpolationSamples);
    resonance.setCoefficients(dsp::BiquadCoefficients::make(dsp::FilterType::lowShelf, spec.sampleRate,
        105.0, 0.707, (parameters.resonance - 0.5f) * 14.0f), interpolationSamples);
}
void PowerAmp::process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept
{
    const auto count = std::min(channelCount, dsp::maximumChannels);
    for (std::size_t sample = 0; sample < samples; ++sample)
    {
        const auto masterGain = master.next();
        for (std::size_t channel = 0; channel < count; ++channel)
        {
            const auto input = channels[channel][sample] * masterGain
                             - parameters.feedback * 0.28f * feedbackState[channel];
            const auto instantEnergy = input * input;
            energy[channel] = instantEnergy + 0.995f * (energy[channel] - instantEnergy);
            const auto targetSupply = std::clamp(1.0f - parameters.sag * std::sqrt(std::max(0.0f, energy[channel]))
                                                 * 0.72f, 0.32f, 1.0f);
            const auto supplyCoefficient = targetSupply < supply[channel]
                ? sagAttackCoefficient : sagRecoveryCoefficient;
            supply[channel] = targetSupply + supplyCoefficient * (supply[channel] - targetSupply);
            const auto headroom = supply[channel] * (0.62f + 0.38f * parameters.damping);
            const auto drive = 1.0f + parameters.saturation * 7.0f;
            const auto biased = input + parameters.biasCharacter * (1.0f - supply[channel]) * 0.2f;
            const auto output = headroom * std::tanh(biased * drive / std::max(0.12f, headroom));
            feedbackState[channel] = output;
            channels[channel][sample] = output;
        }
    }
    resonance.process(channels, count, samples); presence.process(channels, count, samples);
}
float PowerAmp::supplyState(std::size_t channel) const noexcept
{
    return channel < dsp::maximumChannels ? supply[channel] : 1.0f;
}

void CabinetSection::prepare(const dsp::ProcessSpec& newSpec)
{
    spec = newSpec; spec.channels = std::min(spec.channels, dsp::maximumChannels);
    first.prepare(maximumIrLength, spec.channels); second.prepare(maximumIrLength, spec.channels);
    lowCut.prepare(spec); highCut.prepare(spec);
    dryBuffer.assign(spec.maximumBlockSize * spec.channels, 0.0f);
    firstBuffer.assign(spec.maximumBlockSize * spec.channels, 0.0f);
    secondBuffer.assign(spec.maximumBlockSize * spec.channels, 0.0f);
    delay.assign((maximumAlignmentSamples + 1) * spec.channels, 0.0f);
    std::vector<float> irA(384), irB(384);
    for (std::size_t index = 0; index < irA.size(); ++index)
    {
        const auto time = static_cast<float>(index);
        irA[index] = (index == 0 ? 0.72f : 0.0f) + 0.16f * std::exp(-time / 68.0f)
                   * std::sin(0.31f * time);
        irB[index] = (index == 2 ? 0.62f : 0.0f) + 0.14f * std::exp(-time / 82.0f)
                   * std::sin(0.24f * time + 0.4f);
    }
    static_cast<void>(loadImpulseA(irA, {}, { "TubeForge 4x12 Edge", "Dynamic 57", 0.25f }));
    static_cast<void>(loadImpulseB(irB, {}, { "TubeForge 2x12 Center", "Ribbon 121", 0.65f }));
    setParameters(parameters, 0); reset();
}
void CabinetSection::reset() noexcept
{
    first.reset(); second.reset(); lowCut.reset(); highCut.reset();
    std::fill(delay.begin(), delay.end(), 0.0f); delayPosition.fill(0);
}
void CabinetSection::setParameters(const CabinetParameters& next, std::size_t interpolationSamples) noexcept
{
    parameters = next; parameters.blend = clamp01(parameters.blend);
    parameters.bassDiBlend = clamp01(parameters.bassDiBlend);
    parameters.delaySamplesB = std::min(parameters.delaySamplesB, maximumAlignmentSamples);
    lowCut.setCoefficients(dsp::BiquadCoefficients::make(dsp::FilterType::highPass, spec.sampleRate,
        parameters.lowCutHz, 0.707), interpolationSamples);
    highCut.setCoefficients(dsp::BiquadCoefficients::make(dsp::FilterType::lowPass, spec.sampleRate,
        parameters.highCutHz, 0.707), interpolationSamples);
}
bool CabinetSection::loadImpulseA(std::span<const float> left, std::span<const float> right,
                                  CabinetMetadata metadata)
{
    if (! first.loadImpulse(left, right)) return false;
    firstMetadata = std::move(metadata); return true;
}
bool CabinetSection::loadImpulseB(std::span<const float> left, std::span<const float> right,
                                  CabinetMetadata metadata)
{
    if (! second.loadImpulse(left, right)) return false;
    secondMetadata = std::move(metadata); return true;
}
void CabinetSection::process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept
{
    const auto count = std::min(channelCount, spec.channels);
    const auto processSamples = std::min(samples, spec.maximumBlockSize);
    if (parameters.bypass) return;
    std::array<float*, dsp::maximumChannels> firstPointers {}, secondPointers {};
    for (std::size_t channel = 0; channel < count; ++channel)
    {
        auto* dry = dryBuffer.data() + channel * spec.maximumBlockSize;
        firstPointers[channel] = firstBuffer.data() + channel * spec.maximumBlockSize;
        secondPointers[channel] = secondBuffer.data() + channel * spec.maximumBlockSize;
        std::copy_n(channels[channel], processSamples, dry);
        std::copy_n(channels[channel], processSamples, firstPointers[channel]);
        std::copy_n(channels[channel], processSamples, secondPointers[channel]);
    }
    first.process(firstPointers.data(), count, processSamples);
    second.process(secondPointers.data(), count, processSamples);
    const auto delaySize = maximumAlignmentSamples + 1;
    for (std::size_t sample = 0; sample < processSamples; ++sample)
        for (std::size_t channel = 0; channel < count; ++channel)
        {
            auto* channelDelay = delay.data() + channel * delaySize;
            channelDelay[delayPosition[channel]] = secondPointers[channel][sample];
            const auto readPosition = (delayPosition[channel] + delaySize - parameters.delaySamplesB) % delaySize;
            auto secondSample = channelDelay[readPosition];
            delayPosition[channel] = (delayPosition[channel] + 1) % delaySize;
            if (parameters.phaseInvertB) secondSample = -secondSample;
            const auto cabinet = firstPointers[channel][sample] * (1.0f - parameters.blend)
                               + secondSample * parameters.blend;
            const auto dry = dryBuffer[channel * spec.maximumBlockSize + sample];
            channels[channel][sample] = cabinet * (1.0f - parameters.bassDiBlend)
                                      + dry * parameters.bassDiBlend;
        }
    lowCut.process(channels, count, processSamples); highCut.process(channels, count, processSamples);
}

void AmpVoice::prepare(const dsp::ProcessSpec& newSpec)
{
    spec = newSpec; spec.channels = std::min(spec.channels, dsp::maximumChannels);
    calibrator.prepare(spec); gate.prepare(spec); preEq.prepare(spec);
    for (auto& stage : stages) stage.prepare(spec);
    toneStack.prepare(spec); phaseInverter.prepare(spec); powerAmp.prepare(spec); cabinet.prepare(spec);
    bassCrossover.prepare(spec); bassCompressor.prepare(spec);
    postLow.prepare(spec); postMid.prepare(spec); postHigh.prepare(spec);
    inputTrim.prepare(spec.sampleRate, 15.0, dsp::SmoothingMode::logarithmic);
    outputGain.prepare(spec.sampleRate, 20.0, dsp::SmoothingMode::logarithmic);
    cleanBlend.prepare(spec.sampleRate, 25.0);
    lowBuffer.assign(spec.maximumBlockSize * spec.channels, 0.0f);
    highBuffer.assign(spec.maximumBlockSize * spec.channels, 0.0f);
    dryBuffer.assign(spec.maximumBlockSize * spec.channels, 0.0f);
    configure(makeOriginalPreset(Topology::tightModern, Instrument::guitar)); reset();
}
void AmpVoice::reset() noexcept
{
    calibrator.reset(); gate.reset(); preEq.reset(); for (auto& stage : stages) stage.reset();
    toneStack.reset(); phaseInverter.reset(); powerAmp.reset(); cabinet.reset(); bassCrossover.reset();
    bassCompressor.reset(); postLow.reset(); postMid.reset(); postHigh.reset();
    inputEnergy.fill(0.0); outputEnergy.fill(0.0); matchGain.fill(1.0f);
    inputTrim.reset(dsp::dbToLinear(parameters.manualInputTrimDb));
    outputGain.reset(dsp::dbToLinear(parameters.outputGainDb)); cleanBlend.reset(parameters.bass.cleanBlend);
}
void AmpVoice::configure(const AmpPreset& preset) noexcept
{
    calibrator.setProfile(preset.calibration); setParameters(preset.parameters); reset();
}
void AmpVoice::setParameters(const AmpParameters& next) noexcept
{
    parameters = next; parameters.stageCount = std::clamp(parameters.stageCount, std::size_t { 2 }, std::size_t { 4 });
    inputTrim.setTarget(dsp::dbToLinear(std::clamp(parameters.manualInputTrimDb, -24.0f, 24.0f)));
    outputGain.setTarget(dsp::dbToLinear(std::clamp(parameters.outputGainDb, -60.0f, 18.0f)));
    cleanBlend.setTarget(clamp01(parameters.bass.cleanBlend));
    dsp::NoiseGateParameters gateParameters; gateParameters.thresholdDb = parameters.gateThresholdDb;
    gateParameters.rangeDb = -80.0f; gate.setParameters(gateParameters);
    preEq.setParameters(parameters.preEq);
    for (std::size_t index = 0; index < stages.size(); ++index)
    {
        auto config = parameters.stages[index];
        if (parameters.instrument == Instrument::bass) config.driveDb += parameters.bass.highDriveDb;
        stages[index].setConfig(config);
    }
    toneStack.setParameters(parameters.toneStack); phaseInverter.setParameters(parameters.phaseInverter);
    powerAmp.setParameters(parameters.powerAmp); cabinet.setParameters(parameters.cabinet);
    bassCrossover.setFrequency(parameters.bass.crossoverHz, 128);
    dsp::CompressorParameters compressor; compressor.thresholdDb = -28.0f;
    compressor.ratio = 1.0f + 7.0f * clamp01(parameters.bass.lowCompression);
    compressor.attackMs = 18.0; compressor.releaseMs = 150.0; bassCompressor.setParameters(compressor);
    postLow.setCoefficients(dsp::BiquadCoefficients::make(dsp::FilterType::lowShelf, spec.sampleRate,
        110.0, 0.707, parameters.postLowDb), 64);
    postMid.setCoefficients(dsp::BiquadCoefficients::make(dsp::FilterType::peaking, spec.sampleRate,
        900.0, 0.8, parameters.postMidDb), 64);
    postHigh.setCoefficients(dsp::BiquadCoefficients::make(dsp::FilterType::highShelf, spec.sampleRate,
        4200.0, 0.707, parameters.postHighDb), 64);
}
void AmpVoice::processDrivenPath(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept
{
    preEq.process(channels, channelCount, samples);
    for (std::size_t index = 0; index < parameters.stageCount; ++index)
        stages[index].process(channels, channelCount, samples);
    toneStack.process(channels, channelCount, samples); phaseInverter.process(channels, channelCount, samples);
    powerAmp.process(channels, channelCount, samples); cabinet.process(channels, channelCount, samples);
}
void AmpVoice::process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept
{
    const auto count = std::min(channelCount, spec.channels);
    const auto processSamples = std::min(samples, spec.maximumBlockSize);
    std::array<const float*, dsp::maximumChannels> readPointers {};
    for (std::size_t channel = 0; channel < count; ++channel) readPointers[channel] = channels[channel];
    calibrator.process(readPointers.data(), count, processSamples);
    for (std::size_t sample = 0; sample < processSamples; ++sample)
    {
        const auto trimGain = inputTrim.next();
        for (std::size_t channel = 0; channel < count; ++channel)
        {
            channels[channel][sample] *= trimGain;
            dryBuffer[channel * spec.maximumBlockSize + sample] = channels[channel][sample];
        }
    }
    gate.process(channels, count, processSamples);
    if (parameters.instrument == Instrument::bass)
    {
        std::array<const float*, dsp::maximumChannels> inputPointers {};
        std::array<float*, dsp::maximumChannels> lowPointers {}, highPointers {};
        for (std::size_t channel = 0; channel < count; ++channel)
        {
            inputPointers[channel] = channels[channel];
            lowPointers[channel] = lowBuffer.data() + channel * spec.maximumBlockSize;
            highPointers[channel] = highBuffer.data() + channel * spec.maximumBlockSize;
        }
        bassCrossover.process(inputPointers.data(), lowPointers.data(), highPointers.data(), count, processSamples);
        bassCompressor.process(lowPointers.data(), count, processSamples);
        if (parameters.bass.lowSaturation)
            for (std::size_t channel = 0; channel < count; ++channel)
                for (std::size_t sample = 0; sample < processSamples; ++sample)
                    lowPointers[channel][sample] = std::tanh(lowPointers[channel][sample] * 1.35f);
        if (count == 2 && parameters.bass.lowMono > 0.0f)
            for (std::size_t sample = 0; sample < processSamples; ++sample)
            {
                const auto mono = 0.5f * (lowPointers[0][sample] + lowPointers[1][sample]);
                for (std::size_t channel = 0; channel < 2; ++channel)
                    lowPointers[channel][sample] += (mono - lowPointers[channel][sample]) * clamp01(parameters.bass.lowMono);
            }
        processDrivenPath(highPointers.data(), count, processSamples);
        for (std::size_t sample = 0; sample < processSamples; ++sample)
        {
            const auto blend = cleanBlend.next();
            for (std::size_t channel = 0; channel < count; ++channel)
                channels[channel][sample] = lowPointers[channel][sample] * blend
                                          + highPointers[channel][sample] * (1.0f - 0.35f * blend);
        }
    }
    else
        processDrivenPath(channels, count, processSamples);
    postLow.process(channels, count, processSamples); postMid.process(channels, count, processSamples);
    postHigh.process(channels, count, processSamples);
    for (std::size_t sample = 0; sample < processSamples; ++sample)
    {
        const auto level = outputGain.next();
        for (std::size_t channel = 0; channel < count; ++channel)
        {
            const auto dry = dryBuffer[channel * spec.maximumBlockSize + sample];
            inputEnergy[channel] = dry * dry + 0.9995 * (inputEnergy[channel] - dry * dry);
            const auto processed = channels[channel][sample];
            outputEnergy[channel] = processed * processed + 0.9995 * (outputEnergy[channel] - processed * processed);
            if (parameters.loudnessMatch && outputEnergy[channel] > 1.0e-9)
            {
                const auto target = std::clamp(static_cast<float>(std::sqrt(
                    inputEnergy[channel] / outputEnergy[channel])), 0.25f, 4.0f);
                matchGain[channel] += 0.0002f * (target - matchGain[channel]);
            }
            else matchGain[channel] += 0.0002f * (1.0f - matchGain[channel]);
            channels[channel][sample] *= level * matchGain[channel];
        }
    }
}
std::size_t AmpVoice::latencySamples() const noexcept
{
    std::size_t latency {};
    for (std::size_t index = 0; index < parameters.stageCount; ++index) latency += stages[index].latencySamples();
    return latency;
}

void TraditionalAmpProcessor::prepare(const dsp::ProcessSpec& newSpec)
{
    spec = newSpec; crossfader.prepare(spec, voices.size());
    for (auto& voice : voices) voice.prepare(spec);
    presets[0] = makeOriginalPreset(Topology::tightModern, Instrument::guitar);
    presets[1] = presets[0];
    requestedParameters = presets[0].parameters;
    voices[0].configure(presets[0]); voices[1].configure(presets[1]); reset();
}
void TraditionalAmpProcessor::reset() noexcept
{
    for (auto& voice : voices) voice.reset(); crossfader.reset();
}
void TraditionalAmpProcessor::setParameters(const AmpParameters& parameters) noexcept
{
    const auto discreteChanged = parameters.instrument != requestedParameters.instrument
                              || parameters.topology != requestedParameters.topology;
    requestedParameters = parameters;
    if (discreteChanged)
    {
        transitionTarget = 1 - crossfader.mode();
        presets[transitionTarget].parameters = parameters;
        voices[transitionTarget].setParameters(parameters);
        voices[transitionTarget].reset();
        crossfader.requestMode(transitionTarget, 1024);
        transitionPending = true;
        return;
    }
    if (transitionPending)
    {
        presets[transitionTarget].parameters = parameters;
        voices[transitionTarget].setParameters(parameters);
        if (crossfader.mode() == transitionTarget && ! crossfader.isCrossfading())
            transitionPending = false;
        return;
    }
    for (auto& voice : voices) voice.setParameters(parameters);
    for (auto& preset : presets) preset.parameters = parameters;
}
void TraditionalAmpProcessor::setParametersImmediately(const AmpParameters& parameters) noexcept
{
    requestedParameters = parameters;
    for (std::size_t index = 0; index < voices.size(); ++index)
    {
        presets[index].parameters = parameters;
        voices[index].setParameters(parameters);
        voices[index].reset();
    }
    crossfader.reset();
    transitionTarget = 0;
    transitionPending = false;
}
void TraditionalAmpProcessor::loadPreset(const AmpPreset& preset, std::size_t crossfadeSamples)
{
    const auto inactive = 1 - crossfader.mode();
    presets[inactive] = preset;
    voices[inactive].configure(preset);
    crossfader.requestMode(inactive, crossfadeSamples);
    requestedParameters = preset.parameters;
    transitionTarget = inactive;
    transitionPending = true;
}
void TraditionalAmpProcessor::process(float* const* channels, std::size_t channelCount,
                                      std::size_t samples) noexcept
{
    crossfader.process(channels, channelCount, samples,
        [this](std::size_t voice, float* const* buffers, std::size_t count, std::size_t blockSamples) noexcept
        {
            voices[voice].process(buffers, count, blockSamples);
        });
}
CalibrationReading TraditionalAmpProcessor::calibrationReading() const noexcept
{
    return voices[crossfader.mode()].calibrationReading();
}
std::size_t TraditionalAmpProcessor::latencySamples() const noexcept
{
    return voices[crossfader.mode()].latencySamples();
}

std::vector<float> renderOffline(TraditionalAmpProcessor& processor, std::span<const float> monoInput,
                                 std::size_t blockSize)
{
    blockSize = std::max<std::size_t>(1, blockSize);
    std::vector<float> result(monoInput.size());
    std::vector<float> block(blockSize);
    for (std::size_t offset = 0; offset < monoInput.size(); offset += blockSize)
    {
        const auto count = std::min(blockSize, monoInput.size() - offset);
        std::fill(block.begin(), block.end(), 0.0f);
        std::copy_n(monoInput.data() + offset, count, block.data());
        float* channel[] { block.data() };
        processor.process(channel, 1, count);
        std::copy_n(block.data(), count, result.data() + offset);
    }
    return result;
}
} // namespace nts::amp
