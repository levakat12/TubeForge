#pragma once

// The measurements `nts_pedal_compare` reports, kept apart from the tool so they can be tested
// against signals with known answers.
//
// Everything reads one Hann-windowed, zero-padded spectrum. The padding is not for resolution; it
// is what stops a tone that falls between two bins reading a couple of dB low, which would be a
// bias in exactly the comparison this exists to make.

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

namespace pedalmeasure
{
inline constexpr std::size_t bandCount = 20;
/// Quieter than anything a 24-bit signal can hold, so a missing harmonic reads as absent.
inline constexpr double silenceDb = -200.0;

struct Spectrum
{
    /// Power per bin, normalised so the one-sided sum is the signal's mean square.
    std::vector<double> power;
    double binHz {};
    /// How many bins the padding gives each unpadded one.
    double padding { 1.0 };
    /// The width of one bin of the signal itself, which is what the window's lobes scale with.
    double resolutionHz {};
};

namespace detail
{
inline void fft(std::vector<std::complex<double>>& data)
{
    const auto size = data.size();
    for (std::size_t index = 1, other = 0; index < size; ++index)
    {
        auto bit = size >> 1;
        for (; other & bit; bit >>= 1) other ^= bit;
        other ^= bit;
        if (index < other) std::swap(data[index], data[other]);
    }
    for (std::size_t length = 2; length <= size; length <<= 1)
    {
        const auto step = std::polar(1.0, -2.0 * std::numbers::pi / static_cast<double>(length));
        for (std::size_t start = 0; start < size; start += length)
        {
            std::complex<double> twiddle { 1.0, 0.0 };
            for (std::size_t offset = 0; offset < length / 2; ++offset)
            {
                const auto even = data[start + offset];
                const auto odd = data[start + offset + length / 2] * twiddle;
                data[start + offset] = even + odd;
                data[start + offset + length / 2] = even - odd;
                twiddle *= step;
            }
        }
    }
}

inline Spectrum spectrumOf(std::span<const float> signal, double sampleRate)
{
    std::size_t size = 2;
    while (size < signal.size() * 2) size <<= 1;
    std::vector<std::complex<double>> data(size);
    double windowEnergy = 0.0;
    const auto length = static_cast<double>(signal.size());
    for (std::size_t index = 0; index < signal.size(); ++index)
    {
        const auto window = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(index) / length);
        data[index] = window * static_cast<double>(signal[index]);
        windowEnergy += window * window;
    }
    fft(data);
    Spectrum spectrum;
    spectrum.binHz = sampleRate / static_cast<double>(size);
    spectrum.padding = static_cast<double>(size) / std::max(length, 1.0);
    spectrum.resolutionHz = sampleRate / std::max(length, 1.0);
    spectrum.power.resize(size / 2 + 1);
    const auto scale = 1.0 / (static_cast<double>(size) * std::max(windowEnergy, 1e-30));
    for (std::size_t bin = 0; bin < spectrum.power.size(); ++bin)
        spectrum.power[bin] = std::norm(data[bin]) * scale * (bin == 0 || bin == size / 2 ? 1.0 : 2.0);
    return spectrum;
}

inline double toDb(double power) { return 10.0 * std::log10(std::max(power, 1e-30)); }

/// The loudest bin within a Hann main lobe of `frequency`, or zero if there is none.
inline double peakPowerNear(const Spectrum& spectrum, double frequency)
{
    const auto centre = static_cast<long>(std::lround(frequency / spectrum.binHz));
    const auto reach = static_cast<long>(std::lround(3.0 * spectrum.resolutionHz / spectrum.binHz));
    double peak = 0.0;
    for (auto bin = centre - reach; bin <= centre + reach; ++bin)
        if (bin >= 0 && static_cast<std::size_t>(bin) < spectrum.power.size())
            peak = std::max(peak, spectrum.power[static_cast<std::size_t>(bin)]);
    return peak;
}
} // namespace detail

/// Centre of each of the twenty bands, 100 Hz to 8 kHz in third-octave steps (ten per decade).
inline double bandCentreHz(std::size_t band)
{
    return 100.0 * std::pow(10.0, static_cast<double>(band) / 10.0);
}

inline std::vector<double> thirdOctaveLevelsDb(std::span<const float> signal, double sampleRate)
{
    const auto spectrum = detail::spectrumOf(signal, sampleRate);
    std::vector<double> levels(bandCount, silenceDb);
    for (std::size_t band = 0; band < bandCount; ++band)
    {
        const auto low = bandCentreHz(band) * std::pow(10.0, -0.05);
        const auto high = bandCentreHz(band) * std::pow(10.0, 0.05);
        double total = 0.0;
        for (std::size_t bin = 0; bin < spectrum.power.size(); ++bin)
        {
            const auto frequency = static_cast<double>(bin) * spectrum.binHz;
            if (frequency >= low && frequency < high) total += spectrum.power[bin];
        }
        // Padding covers each frequency several times over, so the sum is divided back down.
        levels[band] = detail::toDb(total / spectrum.padding);
    }
    return levels;
}

inline std::array<double, 5> harmonicsDb(std::span<const float> signal, double sampleRate,
                                         double fundamentalHz)
{
    const auto spectrum = detail::spectrumOf(signal, sampleRate);
    const auto reference = detail::toDb(detail::peakPowerNear(spectrum, fundamentalHz));
    std::array<double, 5> harmonics {};
    for (std::size_t index = 0; index < harmonics.size(); ++index)
    {
        const auto frequency = fundamentalHz * static_cast<double>(index + 1);
        harmonics[index] = frequency >= sampleRate / 2.0
                               ? silenceDb
                               : std::max(silenceDb, detail::toDb(detail::peakPowerNear(spectrum, frequency))
                                                         - reference);
    }
    return harmonics;
}

/// Harmonics 2 up to Nyquist against the fundamental, as a percentage.
inline double thdPercent(std::span<const float> signal, double sampleRate, double fundamentalHz)
{
    const auto spectrum = detail::spectrumOf(signal, sampleRate);
    const auto reference = detail::peakPowerNear(spectrum, fundamentalHz);
    if (reference <= 0.0) return 0.0;
    double harmonicPower = 0.0;
    for (int order = 2; fundamentalHz * order < sampleRate / 2.0; ++order)
        harmonicPower += detail::peakPowerNear(spectrum, fundamentalHz * order);
    return 100.0 * std::sqrt(harmonicPower / reference);
}

/** The highest bin that is not on a harmonic of the fundamental, relative to the fundamental.

    Aliasing folds a harmonic back to a frequency that is not a multiple of the fundamental, so
    this is where it shows. Below 40 Hz is left out so a DC offset is not mistaken for it.
*/
inline double aliasFloorDb(std::span<const float> signal, double sampleRate, double fundamentalHz)
{
    const auto spectrum = detail::spectrumOf(signal, sampleRate);
    const auto reference = detail::toDb(detail::peakPowerNear(spectrum, fundamentalHz));
    // Wide enough that the window's skirts around a harmonic are not read as something else.
    const auto exclusion = 20.0 * spectrum.resolutionHz;
    double floorPower = 0.0;
    for (std::size_t bin = 0; bin < spectrum.power.size(); ++bin)
    {
        const auto frequency = static_cast<double>(bin) * spectrum.binHz;
        if (frequency < 40.0) continue;
        const auto order = std::round(frequency / fundamentalHz);
        if (std::abs(frequency - order * fundamentalHz) <= exclusion) continue;
        floorPower = std::max(floorPower, spectrum.power[bin]);
    }
    return std::max(silenceDb, detail::toDb(floorPower) - reference);
}

/// The largest range, across the lists, of any one band: how far apart the captures are at worst.
inline double spreadDb(const std::vector<std::vector<double>>& bandLevels)
{
    double spread = 0.0;
    if (bandLevels.empty()) return spread;
    for (std::size_t band = 0; band < bandLevels.front().size(); ++band)
    {
        auto low = bandLevels.front()[band], high = low;
        for (const auto& levels : bandLevels)
        {
            low = std::min(low, levels[band]);
            high = std::max(high, levels[band]);
        }
        spread = std::max(spread, high - low);
    }
    return spread;
}
} // namespace pedalmeasure
