#include <nts/reconstruction/StemRefinement.h>

#include <nts/dsp/Analysis.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <numeric>
#include <span>
#include <vector>

namespace nts::reconstruction
{
namespace
{
/** Below this the local window sum is too small to divide by, so the original
    sample is kept instead of being amplified. Only the first and last few tens of
    samples of a channel are affected; the steady-state Hann sum is 1.5. */
constexpr float minimumWindowSum = 0.05f;

/** One stem's working state for the channel currently being refined. */
struct StemChannel
{
    std::vector<float>* samples {};
    std::vector<std::complex<float>> spectrum;
    std::vector<float> magnitude;
    std::vector<float> weight;
    std::vector<float> mask;
    std::vector<float> accumulator;
    std::vector<float> frame;
};

double channelEnergy(const std::vector<float>& samples) noexcept
{
    return std::inner_product(samples.begin(), samples.end(), samples.begin(), 0.0);
}
} // namespace

StemRefinementReport refineStems(StemSet& stems, const StereoAudio& mixture,
                                 const StemRefinementOptions& options,
                                 const ProgressCallback& progress, std::stop_token stopToken)
{
    StemRefinementReport report;
    const auto sampleCount = mixture.samples();
    if (! stems.success)
    {
        report.warnings.emplace_back("stem set was not produced successfully; refinement skipped");
        return report;
    }
    if (sampleCount == 0 || options.iterations < 1)
    {
        report.warnings.emplace_back("refinement disabled or mixture is empty");
        return report;
    }
    if (options.fftSize < 64 || options.hopSize == 0 || options.hopSize > options.fftSize)
    {
        report.warnings.emplace_back("invalid refinement window configuration");
        return report;
    }

    const std::array<StereoAudio*, 6> candidates { &stems.vocals, &stems.drums, &stems.bass,
                                                   &stems.other, &stems.guitar, &stems.piano };
    std::vector<StereoAudio*> sources;
    for (auto* stem : candidates)
        if (stem->left.size() == sampleCount || stem->right.size() == sampleCount)
            sources.push_back(stem);
    if (sources.size() < 2)
    {
        report.warnings.emplace_back("fewer than two aligned stems; nothing to re-partition");
        return report;
    }

    nts::dsp::Stft stft;
    stft.prepare(options.fftSize, options.hopSize, nts::dsp::WindowType::hann);
    const auto fftSize = stft.size();
    const auto hop = stft.hop();
    const auto halfBins = fftSize / 2 + 1;
    const auto exponent = std::clamp(options.maskExponent, 1.0f, 4.0f);
    const auto floorValue = std::clamp(options.maskFloor, 0.0f, 0.5f);

    // The default exponent of 2 is both the MMSE-optimal choice and the only one
    // that needs no transcendental in the hot loop, which matters when this runs
    // over twenty minutes of audio at a 1024-sample hop.
    const auto raise = [exponent](float value) noexcept
    {
        if (exponent == 2.0f) return value * value;
        if (exponent == 1.0f) return value;
        return std::pow(value, exponent);
    };

    std::vector<StemChannel> working(sources.size());
    std::vector<float> normalization(fftSize, 0.0f);
    std::vector<std::complex<float>> mixtureSpectrum(fftSize);
    std::vector<float> mixtureMagnitude(halfBins, 0.0f);
    std::vector<float> stemMagnitudeSum(halfBins, 0.0f);
    std::vector<float> denominator(halfBins, 0.0f);

    double mixtureMagnitudeTotal {}, residualMagnitudeTotal {};
    double energyBefore {}, energyAfter {};
    const auto framesPerChannel = (sampleCount + hop - 1) / hop;
    std::size_t processedFrames {}, refinedChannels {};

    for (std::size_t channel = 0; channel < 2; ++channel)
    {
        const auto& mixtureSamples = channel == 0 || mixture.right.empty() ? mixture.left : mixture.right;
        if (mixtureSamples.size() < sampleCount) continue;

        std::size_t active {};
        for (std::size_t index = 0; index < sources.size(); ++index)
        {
            auto& samples = channel == 0 ? sources[index]->left : sources[index]->right;
            auto& state = working[index];
            state.samples = samples.size() == sampleCount ? &samples : nullptr;
            if (state.samples == nullptr) continue;
            ++active;
            state.spectrum.assign(fftSize, {});
            state.magnitude.assign(halfBins, 0.0f);
            state.weight.assign(halfBins, 0.0f);
            state.mask.assign(halfBins, 0.0f);
            state.accumulator.assign(fftSize, 0.0f);
            state.frame.assign(fftSize, 0.0f);
        }
        // Only once the channel is known to be refinable, so a skipped channel
        // cannot contribute a "before" energy that never gets an "after".
        if (active < 2) continue;
        for (const auto& state : working)
            if (state.samples != nullptr) energyBefore += channelEnergy(*state.samples);
        ++refinedChannels;
        std::fill(normalization.begin(), normalization.end(), 0.0f);

        for (std::size_t start = 0; start < sampleCount; start += hop)
        {
            if (stopToken.stop_requested())
            {
                report.warnings.emplace_back("refinement cancelled");
                return report;
            }
            const auto available = std::min(fftSize, sampleCount - start);

            stft.analyze(std::span<const float> { mixtureSamples.data() + start, available }, mixtureSpectrum);
            for (std::size_t bin = 0; bin < halfBins; ++bin)
                mixtureMagnitude[bin] = std::abs(mixtureSpectrum[bin]);

            // Reads stay at or above `start`; the flush below only ever writes
            // samples strictly before it, so a stem is never re-analysed from
            // material this pass has already replaced.
            for (auto& state : working)
            {
                if (state.samples == nullptr) continue;
                stft.analyze(std::span<const float> { state.samples->data() + start, available }, state.spectrum);
                for (std::size_t bin = 0; bin < halfBins; ++bin)
                    state.magnitude[bin] = std::abs(state.spectrum[bin]);
            }

            for (int iteration = 0; iteration < options.iterations; ++iteration)
            {
                std::fill(stemMagnitudeSum.begin(), stemMagnitudeSum.end(), 0.0f);
                std::fill(denominator.begin(), denominator.end(), 0.0f);
                for (auto& state : working)
                {
                    if (state.samples == nullptr) continue;
                    for (std::size_t bin = 0; bin < halfBins; ++bin)
                    {
                        state.weight[bin] = raise(state.magnitude[bin]);
                        stemMagnitudeSum[bin] += state.magnitude[bin];
                        denominator[bin] += state.weight[bin];
                    }
                }
                if (options.modelResidual)
                    for (std::size_t bin = 0; bin < halfBins; ++bin)
                    {
                        const auto residual = std::max(0.0f, mixtureMagnitude[bin] - stemMagnitudeSum[bin]);
                        denominator[bin] += raise(residual);
                        if (iteration == 0 && channel == 0)
                        {
                            mixtureMagnitudeTotal += mixtureMagnitude[bin];
                            residualMagnitudeTotal += residual;
                        }
                    }
                for (auto& state : working)
                {
                    if (state.samples == nullptr) continue;
                    for (std::size_t bin = 0; bin < halfBins; ++bin)
                    {
                        const auto mask = denominator[bin] > 1.0e-20f
                            ? std::clamp(state.weight[bin] / denominator[bin], floorValue, 1.0f) : 0.0f;
                        state.mask[bin] = mask;
                        state.magnitude[bin] = mask * mixtureMagnitude[bin];
                    }
                }
            }

            for (auto& state : working)
            {
                if (state.samples == nullptr) continue;
                // The mask is built from magnitudes, so it is symmetric; mirroring it
                // onto the upper half keeps the masked spectrum Hermitian and the
                // inverse transform real.
                for (std::size_t bin = 0; bin < halfBins; ++bin)
                {
                    const auto mask = state.mask[bin];
                    state.spectrum[bin] = mixtureSpectrum[bin] * mask;
                    const auto mirror = fftSize - bin;
                    if (mirror < fftSize && mirror != bin)
                        state.spectrum[mirror] = mixtureSpectrum[mirror] * mask;
                }
                stft.synthesize(state.spectrum, state.frame);
                for (std::size_t index = 0; index < fftSize; ++index)
                    state.accumulator[index] += state.frame[index];
            }
            const auto window = stft.window();
            for (std::size_t index = 0; index < fftSize; ++index)
                normalization[index] += window[index] * window[index];

            for (std::size_t offset = 0; offset < hop && start + offset < sampleCount; ++offset)
            {
                if (normalization[offset] < minimumWindowSum) continue;
                const auto scale = 1.0f / normalization[offset];
                for (auto& state : working)
                    if (state.samples != nullptr)
                        (*state.samples)[start + offset] = state.accumulator[offset] * scale;
            }

            std::move(normalization.begin() + static_cast<std::ptrdiff_t>(hop), normalization.end(),
                      normalization.begin());
            std::fill(normalization.end() - static_cast<std::ptrdiff_t>(hop), normalization.end(), 0.0f);
            for (auto& state : working)
            {
                if (state.samples == nullptr) continue;
                std::move(state.accumulator.begin() + static_cast<std::ptrdiff_t>(hop),
                          state.accumulator.end(), state.accumulator.begin());
                std::fill(state.accumulator.end() - static_cast<std::ptrdiff_t>(hop),
                          state.accumulator.end(), 0.0f);
            }

            if (progress && ++processedFrames % 64 == 0)
            {
                const auto fraction = static_cast<float>(processedFrames)
                    / static_cast<float>(std::max<std::size_t>(1, framesPerChannel * 2));
                if (! progress({ std::min(1.0f, fraction), "refining stem masks" }))
                {
                    report.warnings.emplace_back("refinement cancelled");
                    return report;
                }
            }
        }

        for (const auto& state : working)
            if (state.samples != nullptr) energyAfter += channelEnergy(*state.samples);
    }

    if (refinedChannels == 0)
    {
        report.warnings.emplace_back("no channel had two aligned stems; refinement skipped");
        return report;
    }
    report.applied = true;
    report.stemsRefined = sources.size();
    report.residualShare = mixtureMagnitudeTotal > 0.0
        ? static_cast<float>(residualMagnitudeTotal / mixtureMagnitudeTotal) : 0.0f;
    report.energyChangeDb = energyBefore > 0.0
        ? static_cast<float>(10.0 * std::log10(std::max(energyAfter, 1.0e-18) / energyBefore)) : 0.0f;
    if (report.residualShare > 0.35f)
        report.warnings.emplace_back("a large share of the mixture is unexplained by the supplied stems");
    if (progress) progress({ 1.0f, "stem refinement complete" });
    return report;
}
} // namespace nts::reconstruction
