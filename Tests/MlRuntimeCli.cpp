#include <nts/ml/NeuralModel.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <cstddef>
#include <cstring>
#include <span>
#include <vector>

int main(int argumentCount, char** arguments)
{
    if (argumentCount != 4)
    {
        std::cerr << "usage: nts_ml_runtime_cli model.bin input.f32 output.f32\n";
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
