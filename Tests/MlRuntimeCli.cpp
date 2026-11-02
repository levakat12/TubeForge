#include <nts/ml/NeuralModel.h>
#include <nts/ml/PackedFeatureRegressor.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace
{
std::vector<char> readAll(const char* path)
{
    std::ifstream stream(path, std::ios::binary);
    if (! stream.is_open()) return {};
    return { std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>() };
}

/** Drives `PackedFeatureRegressor` over a matrix of feature rows.

    A separate entry point from the audio path below rather than a branch inside it, because the
    two disagree about what a float in the input file *is*: there it is a sample and the output has
    one per input, here it is one of `inputCount` features and the output has `outputCount` per
    row. Sharing the loop would mean a length check that could not tell a truncated audio vector
    from a feature matrix with the wrong width.
*/
int runRegressor(const char* modelPath, const char* inputPath, const char* outputPath)
{
    const auto modelBytes = readAll(modelPath);
    nts::ml::PackedFeatureRegressor model;
    std::string error;
    if (! model.load({ reinterpret_cast<const std::byte*>(modelBytes.data()), modelBytes.size() },
                     error))
    {
        std::cerr << error << '\n';
        return 3;
    }
    const auto raw = readAll(inputPath);
    if (raw.empty() || raw.size() % sizeof(float) != 0) { std::cerr << "Invalid feature matrix\n"; return 4; }
    std::vector<float> features(raw.size() / sizeof(float));
    std::memcpy(features.data(), raw.data(), raw.size());
    if (features.size() % model.inputCount() != 0)
    {
        std::cerr << "Feature matrix is not a whole number of rows\n";
        return 4;
    }
    const auto rows = features.size() / model.inputCount();
    std::vector<float> results(rows * model.outputCount());
    for (std::size_t row = 0; row < rows; ++row)
        if (! model.predict(std::span(features).subspan(row * model.inputCount(), model.inputCount()),
                            std::span(results).subspan(row * model.outputCount(), model.outputCount())))
            return 5;
    std::ofstream outputStream(outputPath, std::ios::binary);
    outputStream.write(reinterpret_cast<const char*>(results.data()),
                       static_cast<std::streamsize>(results.size() * sizeof(float)));
    return outputStream ? 0 : 6;
}
} // namespace

int main(int argumentCount, char** arguments)
{
    if (argumentCount == 5 && std::strcmp(arguments[1], "--regressor") == 0)
        return runRegressor(arguments[2], arguments[3], arguments[4]);
    if (argumentCount != 4)
    {
        std::cerr << "usage: nts_ml_runtime_cli model.bin input.f32 output.f32\n"
                     "       nts_ml_runtime_cli --regressor model.bin features.f32 output.f32\n";
        return 2;
    }
    // Reads whatever the packed header declares, so the parity driver can drive every architecture
    // the runtime supports through one executable.
    std::ifstream modelStream(arguments[1], std::ios::binary);
    if (! modelStream.is_open())
    {
        std::cerr << "Unable to open packed model\n";
        return 3;
    }
    std::vector<char> modelBytes((std::istreambuf_iterator<char>(modelStream)),
                                 std::istreambuf_iterator<char>());
    nts::ml::NeuralModel model;
    std::string error;
    if (! model.load(std::span(reinterpret_cast<const std::byte*>(modelBytes.data()), modelBytes.size()),
                     error))
    {
        std::cerr << error << '\n';
        return 3;
    }
    model.reset();
    std::ifstream inputStream(arguments[2], std::ios::binary);
    if (! inputStream.is_open())
    {
        std::cerr << "Unable to open float32 input vector\n";
        return 4;
    }
    std::vector<char> raw((std::istreambuf_iterator<char>(inputStream)), std::istreambuf_iterator<char>());
    if (raw.size() % sizeof(float) != 0)
    {
        std::cerr << "Invalid float32 input vector\n";
        return 4;
    }
    std::vector<float> input(raw.size() / sizeof(float));
    std::memcpy(input.data(), raw.data(), raw.size());
    std::vector<float> output(input.size());
    if (! model.process(input, output))
        return 5;
    std::ofstream outputStream(arguments[3], std::ios::binary);
    outputStream.write(reinterpret_cast<const char*>(output.data()),
                       static_cast<std::streamsize>(output.size() * sizeof(float)));
    return outputStream ? 0 : 6;
}
