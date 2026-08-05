// Converts one `.nam` capture to NTSM v3 and writes the bytes, so `ml/tests/nam_parity_driver.py`
// can compare them with what `nts-nam-import` produces for the same file.
//
// This exists because the plug-in carries a second implementation of the reader and packer -- it
// has to, since shelling out to a Python interpreter is not a dependency a plug-in gets to have.
// Duplication that is allowed to drift is worse than no duplication, so the parity driver holds
// the two byte for byte, and this is the seam it drives.

#include <nts/nam/Capture.h>

#include <juce_core/juce_core.h>

#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    if (argc < 4)
    {
        std::fprintf(stderr, "usage: nts_nam_cli <capture.nam> <lite|standard> <output.bin>\n");
        return 2;
    }
    // Braced: `juce::File source(juce::CharPointer_UTF8(argv[1]))` parses as a function
    // declaration, and the errors it causes point everywhere except here.
    const juce::File source { juce::String(juce::CharPointer_UTF8(argv[1])) };
    const std::string tierName { argv[2] };
    const juce::File destination { juce::String(juce::CharPointer_UTF8(argv[3])) };

    if (! source.existsAsFile())
    {
        std::fprintf(stderr, "no such capture: %s\n", argv[1]);
        return 2;
    }
    if (tierName != "lite" && tierName != "standard")
    {
        std::fprintf(stderr, "tier must be 'lite' or 'standard'\n");
        return 2;
    }

    nts::nam::Capture capture;
    std::string error;
    if (! nts::nam::readCapture(source.loadFileAsString(), capture, error))
    {
        std::fprintf(stderr, "read failed: %s\n", error.c_str());
        return 1;
    }
    const auto tier = tierName == "lite" ? nts::nam::Tier::lite : nts::nam::Tier::standard;
    // Deliberately strict here where the library is lenient: the parity driver asks for a tier
    // and must be told if it did not get it, rather than silently comparing the other one.
    if (! capture.hasTier(tier))
    {
        std::fprintf(stderr, "capture has no '%s' tier\n", tierName.c_str());
        return 1;
    }

    std::vector<std::byte> packed;
    if (! nts::nam::packWaveNet(*capture.spec(tier), packed, error))
    {
        std::fprintf(stderr, "pack failed: %s\n", error.c_str());
        return 1;
    }
    if (! destination.replaceWithData(packed.data(), packed.size()))
    {
        std::fprintf(stderr, "could not write %s\n", argv[3]);
        return 1;
    }
    // Printed so a failing parity run says what was compared without re-reading the artifact.
    std::printf("%s %s %zu bytes\n", source.getFileName().toRawUTF8(), tierName.c_str(), packed.size());
    return 0;
}
