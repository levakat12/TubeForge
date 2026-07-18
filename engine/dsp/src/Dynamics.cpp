#include "nts/dsp/Dynamics.h"

#include <algorithm>
#include <cmath>

namespace nts::dsp
{
namespace
{
constexpr auto pi = 3.14159265358979323846;
float timeCoefficient(double sampleRate, double milliseconds) noexcept
{
    return static_cast<float>(std::exp(-1.0 / (sampleRate * std::max(0.01, milliseconds) * 0.001)));
}
}

void NoiseGate::prepare(const ProcessSpec& spec) noexcept
{
    sampleRate = std::max(1.0, spec.sampleRate);
    thresholdSmoother.prepare(sampleRate, 5.0);
    rangeSmoother.prepare(sampleRate, 5.0);
    hysteresisSmoother.prepare(sampleRate, 5.0);
    thresholdSmoother.reset(parameters.thresholdDb);
    rangeSmoother.reset(parameters.rangeDb);
    hysteresisSmoother.reset(parameters.hysteresisDb);
    updateCoefficients();
    reset();
}
void NoiseGate::reset() noexcept
{
    sidechainLow.fill(0.0f); envelope = 0.0f; gain = 0.0f; holdRemaining = 0;
    thresholdSmoother.reset(parameters.thresholdDb);
    rangeSmoother.reset(parameters.rangeDb);
    hysteresisSmoother.reset(parameters.hysteresisDb);
    currentState = GateState::closed;
}
void NoiseGate::setParameters(const NoiseGateParameters& next) noexcept
{
    parameters = next;
    parameters.thresholdDb = std::clamp(parameters.thresholdDb, -120.0f, 0.0f);
    parameters.rangeDb = std::clamp(parameters.rangeDb, -120.0f, 0.0f);
    parameters.hysteresisDb = std::clamp(parameters.hysteresisDb, 0.0f, 24.0f);
    thresholdSmoother.setTarget(parameters.thresholdDb);
    rangeSmoother.setTarget(parameters.rangeDb);
    hysteresisSmoother.setTarget(parameters.hysteresisDb);
    updateCoefficients();
}
void NoiseGate::updateCoefficients() noexcept
{
    attackCoefficient = timeCoefficient(sampleRate, parameters.attackMs);
    releaseCoefficient = timeCoefficient(sampleRate, parameters.releaseMs);
    holdSamples = static_cast<std::size_t>(sampleRate * std::max(0.0, parameters.holdMs) * 0.001);
    const auto cutoff = clampFrequency(parameters.sidechainHighPassHz, sampleRate);
    highPassCoefficient = static_cast<float>(1.0 - std::exp(-2.0 * pi * cutoff / sampleRate));
}
void NoiseGate::process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept
{
    const auto count = std::min(channelCount, maximumChannels);
    for (std::size_t sample = 0; sample < samples; ++sample)
    {
        const auto thresholdDb = thresholdSmoother.next();
        const auto hysteresisDb = hysteresisSmoother.next();
        const auto openThreshold = dbToLinear(thresholdDb);
        const auto closeThreshold = dbToLinear(thresholdDb - hysteresisDb);
        const auto closedGain = dbToLinear(rangeSmoother.next());
        float detector {};
        for (std::size_t channel = 0; channel < count; ++channel)
        {
            const auto input = channels[channel][sample];
            sidechainLow[channel] += highPassCoefficient * (input - sidechainLow[channel]);
            detector = std::max(detector, std::abs(input - sidechainLow[channel]));
        }
        const auto envCoefficient = detector > envelope ? attackCoefficient : releaseCoefficient;
        envelope = detector + envCoefficient * (envelope - detector);

        if (envelope >= openThreshold)
        {
            currentState = gain < 0.999f ? GateState::opening : GateState::open;
            holdRemaining = holdSamples;
        }
        else if (envelope < closeThreshold)
        {
            if (holdRemaining > 0)
            {
                --holdRemaining;
                currentState = GateState::holding;
            }
            else
                currentState = gain > closedGain + 1.0e-5f ? GateState::closing : GateState::closed;
        }

        const auto target = currentState == GateState::closed || currentState == GateState::closing
            ? closedGain : 1.0f;
        const auto coefficient = target > gain ? attackCoefficient : releaseCoefficient;
        gain = target + coefficient * (gain - target);
        for (std::size_t channel = 0; channel < count; ++channel)
            channels[channel][sample] *= gain;
    }
    envelope = suppressDenormal(envelope); gain = suppressDenormal(gain);
}

void Compressor::prepare(const ProcessSpec& spec) noexcept
{
    sampleRate = std::max(1.0, spec.sampleRate);
    thresholdSmoother.prepare(sampleRate, 10.0);
    ratioSmoother.prepare(sampleRate, 10.0, SmoothingMode::logarithmic);
    kneeSmoother.prepare(sampleRate, 10.0);
    makeupSmoother.prepare(sampleRate, 10.0);
    thresholdSmoother.reset(parameters.thresholdDb);
    ratioSmoother.reset(parameters.ratio);
    kneeSmoother.reset(parameters.kneeDb);
    makeupSmoother.reset(parameters.makeupDb);
    updateCoefficients(); reset();
}
void Compressor::reset() noexcept
{
    sidechainLow.fill(0.0f); detectorEnvelope.fill(0.0f); smoothedGainDb.fill(0.0f);
    thresholdSmoother.reset(parameters.thresholdDb);
    ratioSmoother.reset(parameters.ratio);
    kneeSmoother.reset(parameters.kneeDb);
    makeupSmoother.reset(parameters.makeupDb);
    lastGainReductionDb = 0.0f;
}
void Compressor::setParameters(const CompressorParameters& next) noexcept
{
    parameters = next;
    parameters.thresholdDb = std::clamp(parameters.thresholdDb, -100.0f, 12.0f);
    parameters.ratio = std::clamp(parameters.ratio, 1.0f, 100.0f);
    parameters.kneeDb = std::clamp(parameters.kneeDb, 0.0f, 48.0f);
    thresholdSmoother.setTarget(parameters.thresholdDb);
    ratioSmoother.setTarget(parameters.ratio);
    kneeSmoother.setTarget(parameters.kneeDb);
    makeupSmoother.setTarget(parameters.makeupDb);
    updateCoefficients();
}
void Compressor::updateCoefficients() noexcept
{
    attackCoefficient = timeCoefficient(sampleRate, parameters.attackMs);
    releaseCoefficient = timeCoefficient(sampleRate, parameters.releaseMs);
    const auto cutoff = std::max(0.0, parameters.sidechainHighPassHz);
    sidechainCoefficient = cutoff <= 0.0 ? 0.0f
        : static_cast<float>(1.0 - std::exp(-2.0 * pi * clampFrequency(cutoff, sampleRate) / sampleRate));
}
float Compressor::gainComputer(float levelDb, float thresholdDb, float ratio,
                               float kneeDb) noexcept
{
    const auto over = levelDb - thresholdDb;
    const auto slope = 1.0f / ratio - 1.0f;
    if (kneeDb <= 0.0f)
        return over > 0.0f ? slope * over : 0.0f;
    const auto halfKnee = kneeDb * 0.5f;
    if (over <= -halfKnee) return 0.0f;
    if (over >= halfKnee) return slope * over;
    const auto position = over + halfKnee;
    return slope * position * position / (2.0f * kneeDb);
}
void Compressor::process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept
{
    const auto count = std::min(channelCount, maximumChannels);
    for (std::size_t sample = 0; sample < samples; ++sample)
    {
        const auto thresholdDb = thresholdSmoother.next();
        const auto ratio = ratioSmoother.next();
        const auto kneeDb = kneeSmoother.next();
        const auto makeup = dbToLinear(makeupSmoother.next());
        std::array<float, maximumChannels> levels {};
        float linked {};
        for (std::size_t channel = 0; channel < count; ++channel)
        {
            const auto input = channels[channel][sample];
            sidechainLow[channel] += sidechainCoefficient * (input - sidechainLow[channel]);
            const auto sidechain = sidechainCoefficient > 0.0f ? input - sidechainLow[channel] : input;
            const auto detected = parameters.detector == DetectorMode::rms ? sidechain * sidechain : std::abs(sidechain);
            const auto coefficient = detected > detectorEnvelope[channel] ? attackCoefficient : releaseCoefficient;
            detectorEnvelope[channel] = detected + coefficient * (detectorEnvelope[channel] - detected);
            levels[channel] = parameters.detector == DetectorMode::rms
                ? std::sqrt(std::max(0.0f, detectorEnvelope[channel])) : detectorEnvelope[channel];
            linked = std::max(linked, levels[channel]);
        }
        for (std::size_t channel = 0; channel < count; ++channel)
        {
            const auto level = parameters.stereoLink ? linked : levels[channel];
            const auto targetDb = gainComputer(linearToDb(level), thresholdDb, ratio, kneeDb);
            const auto coefficient = targetDb < smoothedGainDb[channel] ? attackCoefficient : releaseCoefficient;
            smoothedGainDb[channel] = targetDb + coefficient * (smoothedGainDb[channel] - targetDb);
            channels[channel][sample] *= dbToLinear(smoothedGainDb[channel]) * makeup;
            lastGainReductionDb = std::max(lastGainReductionDb * 0.999f, -smoothedGainDb[channel]);
        }
    }
}

void PeakLimiter::prepare(const ProcessSpec& newSpec, double maximumLookaheadMs)
{
    spec = newSpec;
    delayCapacity = std::max<std::size_t>(1, static_cast<std::size_t>(spec.sampleRate * maximumLookaheadMs * 0.001) + 1);
    delay.assign(delayCapacity * maximumChannels, 0.0f);
    ceilingSmoother.prepare(std::max(1.0, spec.sampleRate), 5.0);
    ceilingSmoother.reset(parameters.ceilingDb);
    setParameters(parameters); reset();
}
void PeakLimiter::reset() noexcept
{
    std::fill(delay.begin(), delay.end(), 0.0f); writePosition = 0; gain = 1.0f;
    ceilingSmoother.reset(parameters.ceilingDb);
}
void PeakLimiter::setParameters(const LimiterParameters& next) noexcept
{
    parameters = next;
    parameters.ceilingDb = std::clamp(parameters.ceilingDb, -24.0f, 0.0f);
    ceilingSmoother.setTarget(parameters.ceilingDb);
    lookaheadSamples = std::min(delayCapacity > 0 ? delayCapacity - 1 : std::size_t {},
                                static_cast<std::size_t>(spec.sampleRate * std::max(0.0, parameters.lookaheadMs) * 0.001));
    releaseCoefficient = timeCoefficient(std::max(1.0, spec.sampleRate), parameters.releaseMs);
}
void PeakLimiter::process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept
{
    const auto count = std::min(channelCount, maximumChannels);
    for (std::size_t sample = 0; sample < samples; ++sample)
    {
        const auto ceiling = dbToLinear(ceilingSmoother.next());
        float peak {};
        for (std::size_t channel = 0; channel < count; ++channel)
        {
            const auto input = channels[channel][sample];
            delay[channel * delayCapacity + writePosition] = input;
            peak = std::max(peak, std::abs(input));
        }
        const auto target = peak > ceiling ? ceiling / std::max(peak, 1.0e-12f) : 1.0f;
        gain = target < gain ? target : 1.0f + releaseCoefficient * (gain - 1.0f);
        const auto readPosition = (writePosition + delayCapacity - lookaheadSamples) % delayCapacity;
        for (std::size_t channel = 0; channel < count; ++channel)
            channels[channel][sample] = std::clamp(delay[channel * delayCapacity + readPosition] * gain,
                                                   -ceiling, ceiling);
        writePosition = (writePosition + 1) % delayCapacity;
    }
}
} // namespace nts::dsp
