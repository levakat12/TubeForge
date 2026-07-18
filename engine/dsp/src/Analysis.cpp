#include "nts/dsp/Analysis.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace nts::dsp
{
std::size_t nextPowerOfTwo(std::size_t value) noexcept
{
    if (value <= 1) return 1;
    --value;
    for (std::size_t shift = 1; shift < sizeof(value) * 8; shift <<= 1)
        value |= value >> shift;
    return value + 1;
}

void Fft::prepare(std::size_t size)
{
    fftSize = nextPowerOfTwo(std::max<std::size_t>(2, size));
    bitReversal.resize(fftSize);
    std::size_t bits {};
    while ((std::size_t { 1 } << bits) < fftSize) ++bits;
    for (std::size_t index = 0; index < fftSize; ++index)
    {
        std::size_t reversed {};
        for (std::size_t bit = 0; bit < bits; ++bit)
            reversed = (reversed << 1) | ((index >> bit) & 1U);
        bitReversal[index] = reversed;
    }
    twiddles.resize(fftSize / 2);
    for (std::size_t index = 0; index < twiddles.size(); ++index)
    {
        const auto angle = -2.0f * std::numbers::pi_v<float> * static_cast<float>(index)
                         / static_cast<float>(fftSize);
        twiddles[index] = { std::cos(angle), std::sin(angle) };
    }
}

void Fft::transform(std::span<std::complex<float>> data, bool inverse) const noexcept
{
    if (data.size() < fftSize || fftSize == 0) return;
    for (std::size_t index = 0; index < fftSize; ++index)
        if (index < bitReversal[index])
            std::swap(data[index], data[bitReversal[index]]);

    for (std::size_t length = 2; length <= fftSize; length <<= 1)
    {
        const auto half = length / 2;
        const auto stride = fftSize / length;
        for (std::size_t start = 0; start < fftSize; start += length)
        {
            for (std::size_t index = 0; index < half; ++index)
            {
                const auto twiddle = inverse ? std::conj(twiddles[index * stride])
                                             : twiddles[index * stride];
                const auto even = data[start + index];
                const auto odd = data[start + index + half] * twiddle;
                data[start + index] = even + odd;
                data[start + index + half] = even - odd;
            }
        }
    }
    if (inverse)
    {
        const auto scale = 1.0f / static_cast<float>(fftSize);
        for (std::size_t index = 0; index < fftSize; ++index)
            data[index] *= scale;
    }
}

void Stft::prepare(std::size_t fftSize, std::size_t newHopSize, WindowType windowType)
{
    transform.prepare(fftSize);
    hopSize = std::clamp(newHopSize, std::size_t { 1 }, transform.size());
    windowValues.resize(transform.size());
    work.resize(transform.size());
    const auto denominator = static_cast<float>(transform.size() - 1);
    for (std::size_t index = 0; index < transform.size(); ++index)
    {
        const auto phase = 2.0f * std::numbers::pi_v<float> * static_cast<float>(index) / denominator;
        switch (windowType)
        {
            case WindowType::rectangular: windowValues[index] = 1.0f; break;
            case WindowType::hann: windowValues[index] = 0.5f - 0.5f * std::cos(phase); break;
            case WindowType::hamming: windowValues[index] = 0.54f - 0.46f * std::cos(phase); break;
            case WindowType::blackman:
                windowValues[index] = 0.42f - 0.5f * std::cos(phase) + 0.08f * std::cos(2.0f * phase);
                break;
        }
    }
}

void Stft::analyze(std::span<const float> input,
                   std::span<std::complex<float>> complexOutput) noexcept
{
    if (complexOutput.size() < transform.size()) return;
    for (std::size_t index = 0; index < transform.size(); ++index)
        complexOutput[index] = { index < input.size() ? input[index] * windowValues[index] : 0.0f, 0.0f };
    transform.transform(complexOutput.first(transform.size()));
}

void Stft::synthesize(std::span<const std::complex<float>> spectrum,
                      std::span<float> output) noexcept
{
    if (spectrum.size() < transform.size() || output.size() < transform.size()) return;
    std::copy_n(spectrum.begin(), transform.size(), work.begin());
    transform.transform(work, true);
    for (std::size_t index = 0; index < transform.size(); ++index)
        output[index] = work[index].real() * windowValues[index];
}

std::vector<float> Stft::reconstructOffline(std::span<const float> input)
{
    if (transform.size() == 0) return {};
    const auto paddedSize = input.size() + transform.size();
    std::vector<float> output(paddedSize, 0.0f);
    std::vector<float> normalization(paddedSize, 0.0f);
    std::vector<float> frame(transform.size());
    std::vector<std::complex<float>> spectrum(transform.size());
    for (std::size_t start = 0; start < input.size(); start += hopSize)
    {
        const auto count = std::min(transform.size(), input.size() - start);
        analyze(input.subspan(start, count), spectrum);
        synthesize(spectrum, frame);
        for (std::size_t index = 0; index < transform.size(); ++index)
        {
            output[start + index] += frame[index];
            normalization[start + index] += windowValues[index] * windowValues[index];
        }
    }
    for (std::size_t index = 0; index < output.size(); ++index)
        if (normalization[index] > 1.0e-8f)
            output[index] /= normalization[index];
    output.resize(input.size());
    return output;
}
} // namespace nts::dsp
