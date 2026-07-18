#pragma once

#include <array>
#include <cmath>

namespace tubeforge::dsp
{
class RecurrentAmpModel
{
public:
    static constexpr std::size_t hiddenSize = 8;

    void reset() noexcept
    {
        hidden.fill(0.0f);
    }

    float process(float input, float driveGain) noexcept
    {
        const auto drivenInput = input * driveGain;
        std::array<float, hiddenSize> next {};

        for (std::size_t i = 0; i < hiddenSize; ++i)
        {
            const auto neighbour = hidden[(i + hiddenSize - 1) % hiddenSize];
            next[i] = std::tanh(inputWeights[i] * drivenInput
                                + recurrentWeights[i] * hidden[i]
                                + neighbourWeights[i] * neighbour
                                + biases[i]);
        }

        hidden = next;

        auto output = directWeight * drivenInput;
        for (std::size_t i = 0; i < hiddenSize; ++i)
            output += outputWeights[i] * hidden[i];

        return output * outputScale;
    }

private:
    // Provisional deterministic voicing. The model exporter will replace these
    // arrays with captured/trained weights without changing the audio callback.
    static constexpr std::array<float, hiddenSize> inputWeights {
        0.91f, -0.72f, 0.56f, -0.43f, 0.34f, 0.27f, -0.21f, 0.16f
    };
    static constexpr std::array<float, hiddenSize> recurrentWeights {
        0.31f, 0.42f, 0.27f, 0.38f, 0.22f, 0.35f, 0.29f, 0.25f
    };
    static constexpr std::array<float, hiddenSize> neighbourWeights {
        0.08f, -0.07f, 0.06f, -0.05f, 0.04f, -0.03f, 0.025f, -0.02f
    };
    static constexpr std::array<float, hiddenSize> biases {
        0.012f, -0.009f, 0.007f, -0.005f, 0.004f, -0.003f, 0.002f, -0.001f
    };
    static constexpr std::array<float, hiddenSize> outputWeights {
        0.62f, -0.44f, 0.31f, -0.23f, 0.18f, 0.14f, -0.10f, 0.07f
    };
    static constexpr float directWeight = 0.22f;
    static constexpr float outputScale = 0.72f;

    std::array<float, hiddenSize> hidden {};
};
} // namespace tubeforge::dsp
