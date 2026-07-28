#include <nts/ml/PackedTanhModel.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <cstring>
#include <vector>

int main(int argumentCount, char** arguments)
{
    if (argumentCount != 4)
    {
        std::cerr << "usage: nts_ml_runtime_cli model.bin input.f32 output.f32\n";
        return 2;
    }
    nts::ml::PackedTanhModel model;
    std::string error;
    if (! model.loadFile(std::filesystem::path(arguments[1]), error))
    {
        std::cerr << error << '\n';
        return 3;
    }
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
