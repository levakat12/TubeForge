#pragma once

#include <complex>
#include <cstddef>
#include <span>
#include <vector>

namespace nts::dsp
{
[[nodiscard]] std::size_t nextPowerOfTwo(std::size_t value) noexcept;

class Fft
{
public:
    void prepare(std::size_t size);
    void transform(std::span<std::complex<float>> data, bool inverse = false) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return fftSize; }

private:
    std::size_t fftSize {};
    std::vector<std::size_t> bitReversal;
    std::vector<std::complex<float>> twiddles;
};

enum class WindowType { rectangular, hann, hamming, blackman };

class Stft
{
public:
    void prepare(std::size_t fftSize, std::size_t hopSize, WindowType window);
    void analyze(std::span<const float> input,
                 std::span<std::complex<float>> complexOutput) noexcept;
    void synthesize(std::span<const std::complex<float>> spectrum,
                    std::span<float> output) noexcept;
    [[nodiscard]] std::vector<float> reconstructOffline(std::span<const float> input);
    [[nodiscard]] std::size_t size() const noexcept { return transform.size(); }
    [[nodiscard]] std::size_t hop() const noexcept { return hopSize; }
    [[nodiscard]] std::span<const float> window() const noexcept { return windowValues; }

private:
    Fft transform;
    std::size_t hopSize {};
    std::vector<float> windowValues;
    std::vector<std::complex<float>> work;
};
} // namespace nts::dsp
