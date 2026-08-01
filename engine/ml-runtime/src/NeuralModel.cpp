#include "nts/ml/NeuralModel.h"

#include <cstdint>
#include <cstring>

namespace nts::ml
{
namespace
{
constexpr std::size_t versionOffset = 4;

bool declaresWaveNet(std::span<const std::byte> bytes) noexcept
{
    if (bytes.size() < versionOffset + sizeof(std::uint32_t)
        || std::memcmp(bytes.data(), "NTSM", 4) != 0)
        return false;
    std::uint32_t version {};
    std::memcpy(&version, bytes.data() + versionOffset, sizeof(version));
    return version == 3;
}
}

bool NeuralModel::load(std::span<const std::byte> bytes, std::string& error)
{
    // Dispatch on the declared version, then let the chosen implementation reject anything it does
    // not recognise. A malformed v3 header must not silently fall through to the recurrent loader
    // and be reported as a dimension error against the wrong format.
    wavenetSelected = declaresWaveNet(bytes);
    return wavenetSelected ? wavenet.load(bytes, error) : recurrent.load(bytes, error);
}

void NeuralModel::reset() noexcept
{
    if (wavenetSelected) wavenet.reset(); else recurrent.reset();
}

bool NeuralModel::process(std::span<const float> input, std::span<float> output) noexcept
{
    return wavenetSelected ? wavenet.process(input, output) : recurrent.process(input, output);
}

bool NeuralModel::setControls(std::span<const float> values) noexcept
{
    return wavenetSelected ? wavenet.setControls(values) : recurrent.setControls(values);
}

bool NeuralModel::isLoaded() const noexcept
{
    return wavenetSelected ? wavenet.isLoaded() : recurrent.isLoaded();
}

int NeuralModel::sampleRate() const noexcept
{
    return wavenetSelected ? wavenet.sampleRate() : recurrent.sampleRate();
}

std::size_t NeuralModel::memoryBytes() const noexcept
{
    return wavenetSelected ? wavenet.memoryBytes() : recurrent.memoryBytes();
}

std::size_t NeuralModel::warmUpSamples() const noexcept
{
    // Recurrent models settle within their own state size; a WaveNet needs a full receptive field,
    // which for the NAM corpus captures is 6347 samples (132 ms at 48 kHz).
    return wavenetSelected ? wavenet.receptiveField() : recurrent.stateSize();
}
} // namespace nts::ml
