#pragma once

#include <juce_core/juce_core.h>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

/** Reading Neural Amp Modeler `.nam` captures and packing them as NTSM v3.

    This is a second implementation of `ml/nts_ml/nam/reader.py` and
    `ml/nts_ml/export/packed.py:pack_wavenet`, and the duplication is deliberate: the plug-in has
    to be able to open a capture a user just downloaded, and shelling out to a Python interpreter
    to do it is not a dependency a plug-in gets to have.

    Duplication that is allowed to drift is worse than no duplication at all, so it is not
    allowed to drift. `nts_nam_parity` renders both converters over the real corpus and requires
    the resulting `model.bin` **bytes to be identical**. That is a stronger gate than comparing
    audio, and it is the reason the existing correctness chain still holds end to end: the Python
    renderer is checked against upstream `neural-amp-modeler`, the C++ runtime is checked against
    the Python renderer, and this converter is checked byte for byte against the Python converter.
*/
namespace nts::nam
{
/** What the capture's author says the hardware is.

    Read from `metadata.gear_type`, which every one of the 354 corpus captures carries. It is a
    label rather than a rule -- see `CaptureLibrary` -- but it is a reliable one: the corpus uses
    exactly `amp`, `full-rig` and `pedal`, with no missing or unexpected values.
*/
enum class GearKind
{
    unknown,
    amp,
    fullRig,
    pedal
};

[[nodiscard]] GearKind gearKindFrom(std::string_view gearType) noexcept;
/// A display label: "Amp", "Full rig", "Pedal", "Unknown".
[[nodiscard]] const char* gearKindName(GearKind kind) noexcept;

/** The two independently trained models inside a corpus capture.

    Selected by `max_value` in the file: the smaller is roughly a third of the cost of the larger.
*/
enum class Tier
{
    lite,
    standard
};

[[nodiscard]] const char* tierName(Tier tier) noexcept;

/** Provenance. Every field is optional in the format, so every field has a defined empty value. */
struct CaptureMetadata
{
    std::string name, modeledBy, gearMake, gearModel, gearType, toneType;
    GearKind gear { GearKind::unknown };
    std::optional<double> loudness, gain, validationEsr;

    /// The attribution an artifact carries. Converting grants no redistribution rights; this
    /// records who made the capture so the question stays answerable.
    [[nodiscard]] std::string licenseText() const;
    /** The label to show for one capture.

        The file stem wins whenever there is one, because within a pack that is the only thing
        that tells two captures apart: all thirty JCM800 captures carry the metadata name
        "Marshall JCM 800 2203 2008" and differ only in their filenames, which is where the
        control settings live. `metadata.name` and the make/model are the fallbacks, and they
        stay visible in the row's detail line either way.
    */
    [[nodiscard]] std::string displayName(std::string_view fileStem) const;
};

struct LayerSpec
{
    int kernelSize {}, dilation {};
    std::string activation;
    double negativeSlope { 0.01 };
};

/** One WaveNet submodel: geometry, weights, and the scalars the head needs. */
struct WaveNetSpec
{
    int channels {}, inputSize { 1 }, conditionSize { 1 }, headKernelSize {};
    bool headBias { true };
    double headScale { 1.0 };
    int sampleRate {};
    std::vector<LayerSpec> layers;
    std::vector<float> weights;

    /** The number of floats this geometry implies.

        Derived from the module layout rather than read from the file, so a capture whose weight
        vector disagrees with its own declared geometry is refused instead of being packed into
        something the runtime would then misinterpret.
    */
    [[nodiscard]] std::size_t weightCount() const noexcept;
    /// History a freshly reset model needs before its output means anything.
    [[nodiscard]] std::size_t receptiveField() const noexcept;
};

struct Capture
{
    std::string version;
    int sampleRate {};
    CaptureMetadata metadata;
    std::optional<WaveNetSpec> lite, standard;

    /// The requested tier, or the only one present. Null only if the capture holds neither.
    [[nodiscard]] const WaveNetSpec* spec(Tier tier) const noexcept;
    [[nodiscard]] bool hasTier(Tier tier) const noexcept;
};

/** Parses `.nam` JSON, refusing by name anything this reader does not model.

    The v0.7 format can express conditioning features -- FiLM modulation, gated activations,
    grouped convolutions, an active head 1x1 -- that neither this reader nor the runtime
    implements. None of the 354 corpus captures use any of them. Refusing loudly is the point: a
    reader that quietly ignored an active FiLM block would produce plausible, wrong audio, and
    nothing downstream would catch it.
*/
[[nodiscard]] bool readCapture(const juce::String& json, Capture& capture, std::string& error);

/** Packs a submodel as NTSM v3.

    Byte-identical to `nts_ml.export.packed.pack_wavenet` for the same capture, which is what
    `nts_nam_parity` asserts. The weight payload is copied through in the order the capture file
    already uses -- the converter never reinterprets weights it does not need to understand.
*/
[[nodiscard]] bool packWaveNet(const WaveNetSpec& spec, std::vector<std::byte>& bytes,
                               std::string& error);
} // namespace nts::nam
