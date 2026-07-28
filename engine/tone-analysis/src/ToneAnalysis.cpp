#include <nts/tone/ToneAnalysis.h>

#include <nts/dsp/Analysis.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <numeric>
#include <numbers>

namespace nts::tone
{
namespace
{
float clamp01(float value) noexcept { return std::clamp(value, 0.0f, 1.0f); }
float db(float value) noexcept { return 20.0f * std::log10(std::max(1.0e-9f, value)); }

float percentile(std::vector<float> values, float fraction)
{
    if (values.empty()) return 0.0f;
    const auto index = static_cast<std::size_t>(std::clamp(fraction, 0.0f, 1.0f)
                                                * static_cast<float>(values.size() - 1));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(index), values.end());
    return values[index];
}

struct SpectrumSummary
{
    std::vector<float> power;
    std::size_t fftSize {};
    double sampleRate {};
};

SpectrumSummary averageSpectrum(std::span<const float> mono, double sampleRate, std::size_t requestedSize)
{
    SpectrumSummary result;
    nts::dsp::Fft fft;
    fft.prepare(requestedSize);
    result.fftSize = fft.size(); result.sampleRate = sampleRate;
    result.power.assign(result.fftSize / 2 + 1, 0.0f);
    std::vector<std::complex<float>> work(result.fftSize);
    std::vector<float> window(result.fftSize);
    for (std::size_t index = 0; index < window.size(); ++index)
        window[index] = 0.5f - 0.5f * std::cos(2.0f * std::numbers::pi_v<float>
                                               * static_cast<float>(index)
                                               / static_cast<float>(window.size() - 1));
    const auto availableFrames = mono.size() <= result.fftSize ? 1U
        : static_cast<unsigned>((mono.size() - result.fftSize) / std::max<std::size_t>(1, result.fftSize / 4) + 1);
    const auto frames = std::clamp<unsigned>(availableFrames, 1, 256);
    for (unsigned frame = 0; frame < frames; ++frame)
    {
        const auto maximumStart = mono.size() > result.fftSize ? mono.size() - result.fftSize : 0;
        const auto start = frames > 1 ? maximumStart * frame / (frames - 1) : 0;
        for (std::size_t index = 0; index < result.fftSize; ++index)
            work[index] = { index + start < mono.size() ? mono[index + start] * window[index] : 0.0f, 0.0f };
        fft.transform(work);
        for (std::size_t bin = 0; bin < result.power.size(); ++bin)
            result.power[bin] += std::norm(work[bin]);
    }
    const auto scale = 1.0f / static_cast<float>(frames);
    for (auto& value : result.power) value *= scale;
    return result;
}

float bandEnergy(const SpectrumSummary& spectrum, double low, double high) noexcept
{
    const auto lowBin = static_cast<std::size_t>(std::clamp(
        low * static_cast<double>(spectrum.fftSize) / spectrum.sampleRate, 0.0,
        static_cast<double>(spectrum.power.size() - 1)));
    const auto highBin = static_cast<std::size_t>(std::clamp(
        high * static_cast<double>(spectrum.fftSize) / spectrum.sampleRate,
        static_cast<double>(lowBin + 1), static_cast<double>(spectrum.power.size())));
    return std::accumulate(spectrum.power.begin() + static_cast<std::ptrdiff_t>(lowBin),
                           spectrum.power.begin() + static_cast<std::ptrdiff_t>(highBin), 0.0f);
}

float similarityToConfidence(float probability) noexcept
{
    return clamp01(0.5f + std::abs(probability - 0.5f));
}

juce::var descriptorVar(const Descriptor& descriptor)
{
    auto* object = new juce::DynamicObject();
    object->setProperty("value", descriptor.value);
    object->setProperty("uncertainty", descriptor.uncertainty);
    return juce::var(object);
}
} // namespace

ToneEncoder::ToneEncoder()
{
    for (std::size_t row = 0; row < toneEmbeddingDimensions; ++row)
        for (std::size_t column = 0; column < toneFeatureVectorDimensions; ++column)
        {
            const auto phase = static_cast<float>((row + 1) * (column + 3));
            projection[row * toneFeatureVectorDimensions + column]
                = 0.55f * std::sin(phase * 0.173f) + 0.45f * std::cos(phase * 0.071f);
        }
}

bool ToneEncoder::loadProjection(std::span<const float> weights, std::string version, std::string& error)
{
    if (weights.size() != projection.size()) { error = "Tone encoder projection has the wrong dimensions"; return false; }
    if (version.empty()) { error = "Tone encoder version cannot be empty"; return false; }
    if (!std::all_of(weights.begin(), weights.end(), [](float value) { return std::isfinite(value); }))
    { error = "Tone encoder projection contains a non-finite weight"; return false; }
    std::copy(weights.begin(), weights.end(), projection.begin());
    embeddingVersion = std::move(version); error.clear(); return true;
}

ToneEmbedding ToneEncoder::encode(std::span<const float, toneFeatureVectorDimensions> features) const noexcept
{
    ToneEmbedding result; result.version = embeddingVersion;
    float energy {};
    for (std::size_t row = 0; row < toneEmbeddingDimensions; ++row)
    {
        float value {};
        const auto* weights = projection.data() + row * toneFeatureVectorDimensions;
        for (std::size_t column = 0; column < toneFeatureVectorDimensions; ++column)
            value += weights[column] * features[column];
        result.values[row] = std::tanh(value * 0.13f);
        energy += result.values[row] * result.values[row];
    }
    const auto inverseNorm = 1.0f / std::sqrt(std::max(energy, 1.0e-12f));
    for (auto& value : result.values) value *= inverseNorm;
    return result;
}

ToneAnalysisResult ToneAnalyzer::analyze(const AnalysisInput& input) const
{
    ToneAnalysisResult result;
    if (input.sampleRate < 8000.0 || input.sampleRate > 384000.0)
    { result.error = "Unsupported analysis sample rate"; return result; }
    if (input.left.empty()) { result.error = "No audio samples were supplied"; return result; }
    const auto sampleCount = std::min(input.left.size(), static_cast<std::size_t>(input.sampleRate * 60.0));
    if (sampleCount < static_cast<std::size_t>(input.sampleRate * 0.25))
    { result.error = "Clip is shorter than 250 ms"; return result; }

    std::vector<float> mono(sampleCount), side(sampleCount);
    double mean {};
    for (std::size_t index = 0; index < sampleCount; ++index)
    {
        const auto right = input.right.size() > index ? input.right[index] : input.left[index];
        mono[index] = 0.5f * (input.left[index] + right);
        side[index] = 0.5f * (input.left[index] - right);
        mean += mono[index];
    }
    mean /= static_cast<double>(sampleCount);
    double energy {}, sideEnergy {}, leftEnergy {}, rightEnergy {}, cross {}, positivePeak {}, negativePeak {};
    std::size_t crossings {};
    for (std::size_t index = 0; index < sampleCount; ++index)
    {
        mono[index] -= static_cast<float>(mean);
        energy += static_cast<double>(mono[index]) * mono[index];
        sideEnergy += static_cast<double>(side[index]) * side[index];
        leftEnergy += static_cast<double>(input.left[index]) * input.left[index];
        const auto right = input.right.size() > index ? input.right[index] : input.left[index];
        rightEnergy += static_cast<double>(right) * right;
        cross += static_cast<double>(input.left[index]) * right;
        positivePeak = std::max(positivePeak, static_cast<double>(mono[index]));
        negativePeak = std::max(negativePeak, static_cast<double>(-mono[index]));
        if (index > 0 && std::signbit(mono[index]) != std::signbit(mono[index - 1])) ++crossings;
    }
    result.features.rms = static_cast<float>(std::sqrt(energy / sampleCount));
    result.features.peak = static_cast<float>(std::max(positivePeak, negativePeak));
    result.features.durationSeconds = static_cast<float>(sampleCount / input.sampleRate);
    result.features.zeroCrossingRate = static_cast<float>(crossings * input.sampleRate / sampleCount);
    if (result.features.rms < 1.0e-5f || result.features.peak < 5.0e-5f)
    { result.error = "Silence or near-silence cannot produce a tone profile"; return result; }

    const auto normalization = 0.125f / result.features.rms;
    for (auto& sample : mono) sample = std::clamp(sample * normalization, -4.0f, 4.0f);
    const auto shortSpectrum = averageSpectrum(mono, input.sampleRate, 512);
    const auto spectrum = averageSpectrum(mono, input.sampleRate, 4096);
    const auto longSpectrum = averageSpectrum(mono, input.sampleRate, 8192);
    const auto nyquist = input.sampleRate * 0.5;
    const auto total = std::max(1.0e-12f, bandEnergy(spectrum, 20.0, nyquist));
    const auto shortTotal = std::max(1.0e-12f, bandEnergy(shortSpectrum, 20.0, nyquist));
    const auto longTotal = std::max(1.0e-12f, bandEnergy(longSpectrum, 20.0, nyquist));
    auto& spectral = result.features.spectral;
    spectral.lowFrequencyExtension = clamp01(bandEnergy(longSpectrum, 25.0, 120.0) / longTotal * 5.0f);
    spectral.lowMidBuildup = clamp01(bandEnergy(spectrum, 120.0, 350.0) / total * 4.0f);
    spectral.midEmphasis = clamp01(bandEnergy(spectrum, 350.0, 1200.0) / total * 2.8f);
    spectral.upperMidAttack = clamp01(bandEnergy(shortSpectrum, 1200.0, 4200.0) / shortTotal * 2.2f);
    const auto highRatio = bandEnergy(spectrum, 4200.0, std::min(16000.0, nyquist)) / total;
    spectral.highFrequencyRolloff = clamp01(1.0f - highRatio * 4.0f);

    double weightedFrequency {}, powerSum {};
    double logFrequencyMean {}, logPowerMean {}; std::size_t regressionCount {};
    float logMagnitudeSum {}, arithmeticMagnitude {};
    for (std::size_t bin = 1; bin < spectrum.power.size(); ++bin)
    {
        const auto frequency = static_cast<double>(bin) * input.sampleRate / spectrum.fftSize;
        if (frequency < 30.0 || frequency > std::min(18000.0, nyquist)) continue;
        const auto power = std::max(spectrum.power[bin], 1.0e-20f);
        weightedFrequency += frequency * power; powerSum += power;
        const auto x = std::log2(frequency / 1000.0); const auto y = 10.0 * std::log10(power);
        logFrequencyMean += x; logPowerMean += y; ++regressionCount;
        logMagnitudeSum += std::log(power); arithmeticMagnitude += power;
    }
    spectral.spectralCentroidHz = static_cast<float>(weightedFrequency / std::max(powerSum, 1.0e-12));
    logFrequencyMean /= std::max<std::size_t>(1, regressionCount);
    logPowerMean /= std::max<std::size_t>(1, regressionCount);
    double slopeNumerator {}, slopeDenominator {};
    for (std::size_t bin = 1; bin < spectrum.power.size(); ++bin)
    {
        const auto frequency = static_cast<double>(bin) * input.sampleRate / spectrum.fftSize;
        if (frequency < 30.0 || frequency > std::min(18000.0, nyquist)) continue;
        const auto x = std::log2(frequency / 1000.0); const auto y = 10.0 * std::log10(std::max(spectrum.power[bin], 1.0e-20f));
        slopeNumerator += (x - logFrequencyMean) * (y - logPowerMean);
        slopeDenominator += (x - logFrequencyMean) * (x - logFrequencyMean);
    }
    spectral.spectralSlopeDbPerOctave = static_cast<float>(slopeNumerator / std::max(slopeDenominator, 1.0e-9));
    spectral.spectralFlatness = clamp01(std::exp(logMagnitudeSum / std::max<std::size_t>(1, regressionCount))
                                               / std::max(1.0e-20f, arithmeticMagnitude / std::max<std::size_t>(1, regressionCount)));
    float peakProminence {};
    for (std::size_t bin = 2; bin + 2 < spectrum.power.size(); ++bin)
        if (spectrum.power[bin] > spectrum.power[bin - 1] && spectrum.power[bin] > spectrum.power[bin + 1])
            peakProminence = std::max(peakProminence, spectrum.power[bin]
                / std::max(1.0e-20f, 0.5f * (spectrum.power[bin - 2] + spectrum.power[bin + 2])));
    spectral.resonantPeakStrength = clamp01(std::log10(std::max(1.0f, peakProminence)) / 2.0f);

    const auto frameSize = std::max<std::size_t>(32, static_cast<std::size_t>(input.sampleRate * 0.02));
    const auto hop = std::max<std::size_t>(1, frameSize / 2);
    std::vector<float> frameDb, frameRms;
    for (std::size_t start = 0; start < mono.size(); start += hop)
    {
        double frameEnergy {}; const auto count = std::min(frameSize, mono.size() - start);
        for (std::size_t i = 0; i < count; ++i) frameEnergy += static_cast<double>(mono[start + i]) * mono[start + i];
        const auto value = static_cast<float>(std::sqrt(frameEnergy / std::max<std::size_t>(1, count)));
        frameRms.push_back(value); frameDb.push_back(db(value));
    }
    auto& dynamic = result.features.dynamic;
    dynamic.crestFactorDb = db(result.features.peak / std::max(result.features.rms, 1.0e-9f));
    float changes {};
    for (std::size_t i = 1; i < frameRms.size(); ++i) changes += std::max(0.0f, frameRms[i] - frameRms[i - 1]);
    dynamic.transientPreservation = clamp01(changes / std::max(0.001f, std::accumulate(frameRms.begin(), frameRms.end(), 0.0f)) * 12.0f
                                                + dynamic.crestFactorDb / 36.0f);
    const auto range = percentile(frameDb, 0.95f) - percentile(frameDb, 0.10f);
    dynamic.loudnessRangeDb = std::clamp(range, 0.0f, 40.0f);
    dynamic.compressionAmount = clamp01(1.0f - range / 24.0f);
    dynamic.attackMilliseconds = 2.0f + (1.0f - dynamic.transientPreservation) * 38.0f;
    dynamic.sustain = clamp01(percentile(frameRms, 0.50f) / std::max(percentile(frameRms, 0.95f), 1.0e-6f));
    dynamic.releaseMilliseconds = 45.0f + dynamic.sustain * 650.0f;
    dynamic.gainReductionEstimateDb = dynamic.compressionAmount * 12.0f;
    dynamic.integratedLoudnessDb = db(result.features.rms) - 0.691f;

    auto peakBin = std::size_t { 1 };
    const auto minimumFundamentalBin = static_cast<std::size_t>(35.0 * spectrum.fftSize / input.sampleRate);
    const auto maximumFundamentalBin = std::min(spectrum.power.size() - 1,
        static_cast<std::size_t>(1000.0 * spectrum.fftSize / input.sampleRate));
    for (std::size_t bin = std::max<std::size_t>(1, minimumFundamentalBin); bin <= maximumFundamentalBin; ++bin)
        if (spectrum.power[bin] > spectrum.power[peakBin]) peakBin = bin;
    const auto fundamentalHz = static_cast<float>(peakBin * input.sampleRate / spectrum.fftSize);
    auto& nonlinear = result.features.nonlinear;
    float harmonicTotal {};
    for (std::size_t harmonic = 1; harmonic <= nonlinear.harmonicDistribution.size(); ++harmonic)
    {
        const auto bin = peakBin * harmonic;
        if (bin >= spectrum.power.size()) break;
        const auto first = bin > 1 ? bin - 1 : bin;
        const auto last = std::min(bin + 2, spectrum.power.size());
        nonlinear.harmonicDistribution[harmonic - 1] = std::accumulate(
            spectrum.power.begin() + static_cast<std::ptrdiff_t>(first),
            spectrum.power.begin() + static_cast<std::ptrdiff_t>(last), 0.0f);
        harmonicTotal += nonlinear.harmonicDistribution[harmonic - 1];
    }
    for (auto& value : nonlinear.harmonicDistribution) value /= std::max(harmonicTotal, 1.0e-12f);
    float odd {}, even {};
    for (std::size_t i = 1; i < nonlinear.harmonicDistribution.size(); ++i)
        ((i + 1) % 2 == 0 ? even : odd) += nonlinear.harmonicDistribution[i];
    nonlinear.oddEvenBalance = std::clamp((odd - even) / std::max(odd + even, 1.0e-6f), -1.0f, 1.0f);
    nonlinear.intermodulationIndicator = clamp01(spectral.spectralFlatness * 0.65f + (1.0f - harmonicTotal / total) * 0.35f);
    const auto nearClip = static_cast<float>(std::count_if(mono.begin(), mono.end(), [](float value) { return std::abs(value) > 0.97f; }))
                        / static_cast<float>(mono.size());
    const auto crestSaturation = clamp01((3.2f - dynamic.crestFactorDb) / 2.8f);
    const auto harmonicSaturation = clamp01((1.0f - nonlinear.harmonicDistribution[0]) * 3.5f);
    nonlinear.saturationOnset = clamp01(crestSaturation * 0.42f + harmonicSaturation * 0.42f
                                         + dynamic.compressionAmount * 0.10f + nearClip * 4.0f
                                         + highRatio * 0.30f);
    nonlinear.clippingAsymmetry = std::clamp(static_cast<float>((positivePeak - negativePeak)
        / std::max(positivePeak + negativePeak, 1.0e-9)), -1.0f, 1.0f);
    nonlinear.levelDependentSpectralChange = clamp01(nonlinear.saturationOnset * 0.55f
                                                       + spectral.spectralFlatness * 0.45f);

    auto& spatial = result.features.spatial;
    spatial.stereoWidth = clamp01(static_cast<float>(std::sqrt(sideEnergy / std::max(energy, 1.0e-12))));
    spatial.channelCorrelation = static_cast<float>(cross / std::sqrt(std::max(leftEnergy * rightEnergy, 1.0e-12)));
    spatial.panning = std::clamp(static_cast<float>((rightEnergy - leftEnergy)
        / std::max(rightEnergy + leftEnergy, 1.0e-12)), -1.0f, 1.0f);
    spatial.doubleTrackingLikelihood = clamp01(spatial.stereoWidth * (1.0f - std::abs(spatial.panning))
                                                * (1.0f - std::max(0.0f, spatial.channelCorrelation)));
    spatial.roomReverbEstimate = clamp01(dynamic.sustain * 0.45f + spatial.stereoWidth * 0.35f
                                          + (1.0f - dynamic.transientPreservation) * 0.20f);
    spatial.modulation = clamp01(spatial.stereoWidth * 0.45f + (1.0f - std::abs(spatial.channelCorrelation)) * 0.25f);
    spatial.delayPresence = clamp01(spatial.roomReverbEstimate * spatial.stereoWidth * 0.85f);

    auto& context = result.report.context;
    const auto bassProbability = clamp01(0.15f + spectral.lowFrequencyExtension * 0.62f
        + clamp01((900.0f - spectral.spectralCentroidHz) / 900.0f) * 0.30f);
    context.instrument = bassProbability >= 0.5f ? Instrument::bass : Instrument::guitar;
    context.instrumentConfidence = similarityToConfidence(bassProbability);
    const auto pickProbability = clamp01(dynamic.transientPreservation * 0.60f + spectral.upperMidAttack * 0.40f);
    context.technique = pickProbability >= 0.52f ? Technique::pick : Technique::fingerstyle;
    context.techniqueConfidence = similarityToConfidence(pickProbability);
    context.approximateRegisterHz = fundamentalHz;

    auto& report = result.report;
    const auto gain = clamp01(nonlinear.saturationOnset * 0.52f + dynamic.compressionAmount * 0.25f
                              + spectral.spectralFlatness * 0.23f);
    report.gain.value = gain;
    report.brightness.value = clamp01(spectral.upperMidAttack * 0.35f + highRatio * 2.0f
                                      + clamp01(spectral.spectralCentroidHz / 5000.0f) * 0.25f);
    report.tightness.value = clamp01(dynamic.transientPreservation * 0.55f
                                     + (1.0f - spectral.lowMidBuildup) * 0.30f
                                     + (1.0f - spatial.roomReverbEstimate) * 0.15f);
    report.compression.value = dynamic.compressionAmount;
    report.cleanLowBlendEstimate.value = clamp01(spectral.lowFrequencyExtension
        * (1.0f - nonlinear.saturationOnset) * 1.2f);
    report.cabinetDarkness.value = 1.0f - report.brightness.value;
    report.roomAmount.value = spatial.roomReverbEstimate;
    context.gainCategory = gain < 0.16f ? GainCategory::clean : gain < 0.34f ? GainCategory::edgeOfBreakup
        : gain < 0.56f ? GainCategory::crunch : gain < 0.82f ? GainCategory::highGain : GainCategory::fuzz;
    context.cleanDistortedConfidence = similarityToConfidence(gain);

    auto& confidence = report.confidence;
    const auto leakageRisk = clamp01(spectral.spectralFlatness * 0.35f + dynamic.transientPreservation * 0.25f
                                      + spatial.stereoWidth * 0.20f + nonlinear.intermodulationIndicator * 0.20f);
    confidence.signalToLeakage = 1.0f - leakageRisk * 0.55f;
    confidence.classifierCertainty = (context.instrumentConfidence + context.techniqueConfidence
                                      + context.cleanDistortedConfidence) / 3.0f;
    confidence.duration = clamp01(result.features.durationSeconds / 5.0f);
    const auto activeBands = static_cast<float>((spectral.lowFrequencyExtension > 0.05f)
        + (spectral.lowMidBuildup > 0.05f) + (spectral.midEmphasis > 0.05f)
        + (spectral.upperMidAttack > 0.03f) + (highRatio > 0.005f));
    confidence.spectralCoverage = clamp01(activeBands / 5.0f);
    confidence.polyphony = clamp01(0.45f + harmonicTotal / total * 1.5f);
    confidence.separationQuality = clamp01(input.sourceSeparationQuality);
    confidence.modelDomainProximity = clamp01(1.0f - std::max(0.0f, spectral.spectralFlatness - 0.75f)
                                              - std::max(0.0f, spatial.stereoWidth - 0.95f));
    confidence.aggregate = clamp01(confidence.signalToLeakage * 0.20f
        + confidence.classifierCertainty * 0.20f + confidence.duration * 0.16f
        + confidence.spectralCoverage * 0.14f + confidence.polyphony * 0.08f
        + confidence.separationQuality * 0.12f + confidence.modelDomainProximity * 0.10f);
    const auto uncertainty = clamp01(1.0f - confidence.aggregate);
    for (auto* descriptor : { &report.gain, &report.brightness, &report.tightness, &report.compression,
                              &report.cleanLowBlendEstimate, &report.cabinetDarkness, &report.roomAmount })
        descriptor->uncertainty = uncertainty;
    if (result.features.durationSeconds < 2.0f) report.warnings.emplace_back("short clip reduces confidence");
    if (leakageRisk > 0.68f) report.warnings.emplace_back("possible drums, vocals, or source leakage");
    if (input.sourceSeparationQuality < 0.7f) report.warnings.emplace_back("source-separation artifacts may bias descriptors");
    if (confidence.spectralCoverage < 0.6f) report.warnings.emplace_back("limited spectral coverage");
    if (confidence.modelDomainProximity < 0.65f) report.warnings.emplace_back("audio is outside the calibrated model domain");

    const auto featureVector = makeFeatureVector(result.features, report);
    result.embedding = toneEncoder.encode(featureVector);
    result.success = true;
    return result;
}

std::array<float, toneFeatureVectorDimensions> makeFeatureVector(
    const ToneFeatures& features, const ToneReport& report) noexcept
{
    std::array<float, toneFeatureVectorDimensions> vector {};
    const std::array core {
        features.spectral.lowFrequencyExtension * 0.35f, features.spectral.lowMidBuildup * 0.35f,
        features.spectral.midEmphasis * 0.45f, features.spectral.upperMidAttack * 0.55f,
        features.spectral.highFrequencyRolloff, features.spectral.resonantPeakStrength,
        clamp01(features.spectral.spectralCentroidHz / 8000.0f) * 0.20f,
        clamp01((features.spectral.spectralSlopeDbPerOctave + 24.0f) / 48.0f) * 0.55f,
        features.spectral.spectralFlatness, clamp01(features.dynamic.crestFactorDb / 30.0f),
        features.dynamic.transientPreservation, features.dynamic.compressionAmount,
        clamp01(features.dynamic.attackMilliseconds / 50.0f),
        clamp01(features.dynamic.releaseMilliseconds / 800.0f), features.dynamic.sustain,
        clamp01(features.dynamic.loudnessRangeDb / 30.0f), features.nonlinear.oddEvenBalance * 0.5f + 0.5f,
        features.nonlinear.intermodulationIndicator * 1.2f, features.nonlinear.saturationOnset * 1.5f,
        features.nonlinear.clippingAsymmetry * 0.5f + 0.5f,
        features.nonlinear.levelDependentSpectralChange * 1.35f, features.spatial.stereoWidth,
        features.spatial.roomReverbEstimate, features.spatial.doubleTrackingLikelihood,
        features.spatial.panning * 0.5f + 0.5f, features.spatial.modulation,
        features.spatial.delayPresence, report.gain.value * 1.5f, report.brightness.value * 0.7f,
        report.tightness.value, report.compression.value, report.cleanLowBlendEstimate.value,
        report.cabinetDarkness.value, report.roomAmount.value
    };
    std::copy(core.begin(), core.end(), vector.begin());
    std::transform(features.nonlinear.harmonicDistribution.begin(), features.nonlinear.harmonicDistribution.end(),
                   vector.begin() + static_cast<std::ptrdiff_t>(core.size()),
                   [](float value) { return value * 1.5f; });
    for (std::size_t index = core.size() + features.nonlinear.harmonicDistribution.size(); index < vector.size(); ++index)
    {
        const auto source = vector[(index * 7 + 3) % (core.size() + features.nonlinear.harmonicDistribution.size())];
        const auto other = vector[(index * 11 + 5) % (core.size() + features.nonlinear.harmonicDistribution.size())];
        vector[index] = std::sqrt(std::max(0.0f, source * other));
    }
    return vector;
}

std::string serializeToneReport(const ToneAnalysisResult& result, bool pretty)
{
    auto* root = new juce::DynamicObject();
    root->setProperty("analysisVersion", juce::String(result.analysisVersion));
    root->setProperty("embeddingVersion", juce::String(result.embedding.version));
    root->setProperty("instrument", juce::String(toString(result.report.context.instrument).data()));
    root->setProperty("instrumentConfidence", result.report.context.instrumentConfidence);
    root->setProperty("technique", juce::String(toString(result.report.context.technique).data()));
    root->setProperty("techniqueConfidence", result.report.context.techniqueConfidence);
    root->setProperty("gainCategory", juce::String(toString(result.report.context.gainCategory).data()));
    root->setProperty("gain", descriptorVar(result.report.gain));
    root->setProperty("compression", descriptorVar(result.report.compression));
    root->setProperty("brightness", descriptorVar(result.report.brightness));
    root->setProperty("tightness", descriptorVar(result.report.tightness));
    root->setProperty("cleanLowBlendEstimate", descriptorVar(result.report.cleanLowBlendEstimate));
    root->setProperty("cabinetDarkness", descriptorVar(result.report.cabinetDarkness));
    root->setProperty("roomAmount", descriptorVar(result.report.roomAmount));
    root->setProperty("confidence", result.report.confidence.aggregate);
    root->setProperty("durationSeconds", result.features.durationSeconds);
    juce::Array<juce::var> warnings;
    for (const auto& warning : result.report.warnings) warnings.add(juce::String(warning));
    root->setProperty("warnings", warnings);
    return juce::JSON::toString(juce::var(root), pretty).toStdString();
}

std::string_view toString(Instrument value) noexcept
{
    switch (value) { case Instrument::guitar: return "guitar"; case Instrument::bass: return "bass"; default: return "unknown"; }
}
std::string_view toString(Technique value) noexcept
{
    switch (value) { case Technique::pick: return "pick"; case Technique::fingerstyle: return "fingerstyle"; default: return "unknown"; }
}
std::string_view toString(GainCategory value) noexcept
{
    switch (value)
    {
        case GainCategory::clean: return "clean"; case GainCategory::edgeOfBreakup: return "edge-of-breakup";
        case GainCategory::crunch: return "crunch"; case GainCategory::highGain: return "high-gain";
        case GainCategory::fuzz: return "fuzz";
    }
    return "clean";
}
std::string_view toString(SourceType value) noexcept
{
    switch (value)
    {
        case SourceType::isolatedStem: return "isolated-stem"; case SourceType::pairedCapture: return "paired-capture";
        case SourceType::pluginRender: return "plugin-render"; case SourceType::physicalAmp: return "physical-amp";
        case SourceType::userRecording: return "user-recording";
    }
    return "user-recording";
}
} // namespace nts::tone
