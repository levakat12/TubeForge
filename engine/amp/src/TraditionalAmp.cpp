#include "nts/amp/TraditionalAmp.h"

#include <nts/dsp/Nonlinear.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <numbers>
#include <sstream>

namespace nts::amp
{
namespace
{
template <typename Value>
Value clamp01(Value value) noexcept { return std::clamp(value, Value {}, Value { 1 }); }

float coefficient(double sampleRate, double milliseconds) noexcept
{
    return static_cast<float>(std::exp(-1.0 / (std::max(1.0, sampleRate)
        * std::max(0.01, milliseconds) * 0.001)));
}

dsp::OversamplingFactor factorFromInt(int factor) noexcept
{
    if (factor >= 8) return dsp::OversamplingFactor::x8;
    if (factor >= 4) return dsp::OversamplingFactor::x4;
    if (factor >= 2) return dsp::OversamplingFactor::x2;
    return dsp::OversamplingFactor::x1;
}

std::optional<double> jsonNumber(std::string_view json, std::string_view key)
{
    const auto marker = std::string("\"") + std::string(key) + "\"";
    auto position = json.find(marker);
    if (position == std::string_view::npos) return std::nullopt;
    position = json.find(':', position + marker.size());
    if (position == std::string_view::npos) return std::nullopt;
    ++position;
    while (position < json.size() && (json[position] == ' ' || json[position] == '\n' || json[position] == '\r')) ++position;
    const auto start = position;
    while (position < json.size() && (std::isdigit(static_cast<unsigned char>(json[position]))
           || json[position] == '-' || json[position] == '+' || json[position] == '.'
           || json[position] == 'e' || json[position] == 'E')) ++position;
    double value {};
    const auto text = json.substr(start, position - start);
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc {} ? std::optional<double> { value } : std::nullopt;
}

std::optional<std::string> jsonString(std::string_view json, std::string_view key)
{
    const auto marker = std::string("\"") + std::string(key) + "\"";
    auto position = json.find(marker);
    if (position == std::string_view::npos) return std::nullopt;
    position = json.find(':', position + marker.size());
    if (position == std::string_view::npos) return std::nullopt;
    position = json.find('"', position + 1);
    if (position == std::string_view::npos) return std::nullopt;
    const auto end = json.find('"', position + 1);
    if (end == std::string_view::npos) return std::nullopt;
    return std::string(json.substr(position + 1, end - position - 1));
}

bool jsonBool(std::string_view json, std::string_view key, bool fallback)
{
    const auto marker = std::string("\"") + std::string(key) + "\"";
    auto position = json.find(marker);
    if (position == std::string_view::npos) return fallback;
    position = json.find(':', position + marker.size());
    if (position == std::string_view::npos) return fallback;
    const auto tail = json.substr(position + 1);
    return tail.find("true") < tail.find("false");
}

float numberOr(std::string_view json, std::string_view key, float fallback)
{
    const auto value = jsonNumber(json, key);
    return value ? static_cast<float>(*value) : fallback;
}
} // namespace

void InputCalibrator::prepare(const dsp::ProcessSpec& spec) noexcept
{
    sampleRate = std::max(1.0, spec.sampleRate);
    reset();
}

void InputCalibrator::reset() noexcept
{
    smoothedEnergy = 0.0;
    lastReading = {};
}

void InputCalibrator::process(const float* const* channels, std::size_t channelCount,
                              std::size_t samples) noexcept
{
    const auto count = std::min(channelCount, dsp::maximumChannels);
    float peak {};
    double energy {};
    for (std::size_t sample = 0; sample < samples; ++sample)
        for (std::size_t channel = 0; channel < count; ++channel)
        {
            const auto value = channels[channel][sample];
            peak = std::max(peak, std::abs(value));
            energy += static_cast<double>(value) * value;
        }
    const auto mean = energy / static_cast<double>(std::max<std::size_t>(1, samples * count));
    const auto smoothing = std::exp(-static_cast<double>(samples) / (sampleRate * 0.4));
    smoothedEnergy = mean + smoothing * (smoothedEnergy - mean);
    lastReading.peakDb = dsp::linearToDb(std::max(peak, 1.0e-6f));
    lastReading.rmsDb = dsp::linearToDb(static_cast<float>(std::sqrt(std::max(1.0e-12, smoothedEnergy))));
    const auto targetRms = 0.5f * (calibration.targetRmsLowDb + calibration.targetRmsHighDb);
    const auto rmsTrim = targetRms - lastReading.rmsDb;
    const auto peakTrim = calibration.targetPeakDb - lastReading.peakDb;
    lastReading.suggestedTrimDb = std::clamp(std::min(rmsTrim, peakTrim + 3.0f), -24.0f, 24.0f);
}

namespace
{
/** The two names each voicing has, indexed by `Topology`.

    One table rather than a chain of ternaries at each of the four places that used to need
    one. The static_assert is the guard that matters: an entry appended to the enum without an
    entry here would otherwise read past the end at run time.
*/
struct TopologyLabels
{
    std::string_view key;
    std::string_view name;
};

constexpr std::array<TopologyLabels, topologyCount> topologyLabels { {
    { "tightModern", "Tight Modern" },
    { "vintageBloom", "Vintage Bloom" },
    { "americanClean", "American Clean" },
    { "britishCrunch", "British Crunch" },
    { "classAChime", "Class-A Chime" },
    { "saggingRectifier", "Sagging Rectifier" },
    { "studioDirect", "Studio Direct" },
    { "valveFlagship", "Valve Flagship" },
    { "cathodeVintage", "Cathode Vintage" },
    { "hybridMosfet", "Hybrid MOSFET" },
    { "shortPathGrind", "Short-Path Grind" },
    { "solidStateBiAmp", "Solid-State Bi-Amp" },
    { "cmosModern", "CMOS Modern" }
} };
static_assert(topologyLabels.size() == topologyCount,
              "every Topology needs a preset key and a display name");

/// The first bass-native voicing. Everything from here on reports `TopologyAffinity::bass`.
constexpr auto firstBassNativeTopology = Topology::valveFlagship;

const TopologyLabels& labelsFor(Topology topology) noexcept
{
    const auto index = static_cast<std::size_t>(topology);
    return topologyLabels[index < topologyLabels.size() ? index : 0];
}
} // namespace

std::string_view topologyKey(Topology topology) noexcept { return labelsFor(topology).key; }

std::string_view topologyName(Topology topology) noexcept { return labelsFor(topology).name; }

namespace
{
/** A stored waveshape index, clamped to a curve that exists.

    `dsp::Waveshape` is serialised as its integer value, so a preset written by a later build --
    or a hand-edited one -- can name a curve this build has never heard of. Casting that straight
    to the enum makes every `switch` on it fall through to its `return input` default, which is a
    silently *linear* gain stage: no distortion, no error, and an amplifier that sounds broken in
    a way nothing reports. Falling back on the valve curve is the same policy `topologyFromKey`
    already applies to an unknown voicing, for the same reason.
*/
[[nodiscard]] dsp::Waveshape shapeFromIndex(float stored) noexcept
{
    constexpr auto highest = static_cast<int>(dsp::Waveshape::ledClip);
    const auto index = static_cast<int>(stored);
    return index >= 0 && index <= highest ? static_cast<dsp::Waveshape>(index)
                                          : dsp::Waveshape::hyperbolicTangent;
}
} // namespace

TopologyAffinity topologyAffinity(Topology topology) noexcept
{
    return static_cast<std::size_t>(topology) >= static_cast<std::size_t>(firstBassNativeTopology)
        ? TopologyAffinity::bass : TopologyAffinity::either;
}

std::optional<Topology> topologyFromKey(std::string_view key) noexcept
{
    for (std::size_t index = 0; index < topologyLabels.size(); ++index)
        if (topologyLabels[index].key == key) return static_cast<Topology>(index);
    return std::nullopt;
}

namespace
{
/** The switch table: which amplifier carries which switch, and what each one does.

    Every figure is measured from the circuit being modelled rather than chosen to taste, which is
    the whole reason these are worth having as switches instead of leaving a player to dial them:

    - **Ultra Lo** lifts 2 dB at 40 Hz *and cuts 10 dB at 500*, and the cut is the dominant term.
      That is why the switch reads as "more bass" without adding any low-end power -- it was
      designed to give the illusion of weight without pushing the output stage, and a version of
      it with only the 40 Hz lift would be a different and much less useful control.
    - **Ultra Hi** is a 6 dB lift at 5 kHz.
    - **Deep** is a broad 5 dB at 30 Hz, **Bright** a broad 5 dB at 5-7 kHz.

    A switch replaces whatever the voicing had on the filter it uses rather than adding to it: on
    the hardware these are networks that are in circuit or out of it, not trims stacked on a
    voicing's own tilt.
*/
struct PanelSwitchLabels { std::string_view key, name; };

constexpr std::array<PanelSwitchLabels, panelSwitchCount> panelSwitchLabels { {
    { "none", "Standard" },
    { "ultraLo", "Ultra Lo" },
    { "ultraHi", "Ultra Hi" },
    { "deep", "Deep" },
    { "bright", "Bright" }
} };
static_assert(panelSwitchLabels.size() == panelSwitchCount,
              "every PanelSwitch needs a key and a display name");
} // namespace

std::string_view panelSwitchKey(PanelSwitch value) noexcept
{
    const auto index = static_cast<std::size_t>(value);
    return panelSwitchLabels[index < panelSwitchLabels.size() ? index : 0].key;
}

std::string_view panelSwitchName(PanelSwitch value) noexcept
{
    const auto index = static_cast<std::size_t>(value);
    return panelSwitchLabels[index < panelSwitchLabels.size() ? index : 0].name;
}

bool panelSwitchAppliesTo(Topology topology, PanelSwitch value) noexcept
{
    switch (value)
    {
        case PanelSwitch::none:    return true;
        case PanelSwitch::ultraLo:
        case PanelSwitch::ultraHi: return topology == Topology::valveFlagship;
        case PanelSwitch::deep:
        case PanelSwitch::bright:  return topology == Topology::hybridMosfet;
    }
    return false;
}

void applyPanelSwitch(AmpParameters& parameters, PanelSwitch value) noexcept
{
    // A switch the voicing does not have is ignored rather than applied. A preset or an automation
    // lane naming one is the case that matters: honouring it would build an amplifier that never
    // existed, and rejecting the whole preset would lose everything else in it.
    if (! panelSwitchAppliesTo(parameters.topology, value)) return;
    auto& preEq = parameters.preEq;
    switch (value)
    {
        case PanelSwitch::none: break;

        case PanelSwitch::ultraLo:
            preEq.lowShelfEnabled = true; preEq.lowShelfHz = 40.0f; preEq.lowShelfDb = 2.0f;
            preEq.midEmphasisEnabled = true; preEq.midEmphasisHz = 500.0f; preEq.midEmphasisDb = -10.0f;
            break;

        case PanelSwitch::ultraHi:
            preEq.highShelfEnabled = true; preEq.highShelfHz = 5000.0f; preEq.highShelfDb = 6.0f;
            break;

        case PanelSwitch::deep:
            preEq.lowShelfEnabled = true; preEq.lowShelfHz = 30.0f; preEq.lowShelfDb = 5.0f;
            break;

        case PanelSwitch::bright:
            preEq.highShelfEnabled = true; preEq.highShelfHz = 6000.0f; preEq.highShelfDb = 5.0f;
            break;
    }
}

AmpPreset makeOriginalPreset(Topology topology, Instrument instrument)
{
    AmpPreset preset;
    preset.parameters.topology = topology;
    preset.parameters.instrument = instrument;
    preset.parameters.pickup = PickupProfile::passive;
    preset.calibration = instrument == Instrument::guitar
        ? CalibrationProfile { -24.0f, -18.0f, -12.0f, PickupProfile::passive }
        : CalibrationProfile { -26.0f, -19.0f, -10.0f, PickupProfile::passive };
    auto& p = preset.parameters;
    const auto guitar = instrument == Instrument::guitar;
    // Every voicing but Studio Direct takes the passive stack on guitar and the bass
    // semi-parametric on bass. Studio Direct is the exception on purpose -- see its case.
    const auto passiveStack = guitar ? ToneStackType::passiveCoupled
                                     : ToneStackType::bassSemiParametric;
    /* The bass path, set **before** the switch so a voicing can overrule it.

       It used to be assigned after, which was right while every voicing shared one bass path and
       silently wrong the moment one did not: a `case` that set `p.bass` had it overwritten on the
       way out, and the result compiled, ran, and sounded like a plausible amplifier. The seven
       original voicings touch none of these three fields, so ordering them first is bit-identical
       for all of them.
    */
    if (! guitar)
    {
        p.bass = { 170.0f, 0.62f, 0.52f, 5.0f, 1.0f, false };
        p.cabinet.bassDiBlend = 0.25f;
        p.outputGainDb = -5.0f;
    }
    switch (topology)
    {
    case Topology::tightModern:
        preset.name = guitar ? "Original Tight Guitar" : "Original Tight Bass";
        p.stageCount = guitar ? 4 : 3;
        p.preEq = { guitar ? 95.0f : 48.0f, 16500.0f, 0.72f,
                    2.5f, true, -1.5f, true, 1.5f };
        for (std::size_t index = 0; index < p.stages.size(); ++index)
        {
            p.stages[index] = { 11.0f + static_cast<float>(index) * 2.5f,
                index % 2 == 0 ? 0.04f : -0.025f, 0.12f + 0.03f * static_cast<float>(index),
                guitar ? 90.0f : 45.0f, 12500.0f - 800.0f * index,
                -7.0f, 4, 0.28f, 0.28f, 0.22f, 0.32f };
        }
        p.toneStack = { passiveStack, 0.5f, 0.56f, 0.58f, guitar ? 850.0f : 650.0f, 0.85f };
        p.phaseInverter = { 1.7f, 0.78f, 0.14f, 0.06f, 0.3f };
        p.powerAmp = { -4.0f, 0.48f, 0.7f, 0.3f, 0.03f, 0.58f, 0.55f, 0.4f, 28.0f, 360.0f };
        break;

    case Topology::vintageBloom:
        preset.name = guitar ? "Original Vintage Bloom" : "Original Bass Bloom";
        p.stageCount = 2;
        p.preEq = { guitar ? 62.0f : 38.0f, 19000.0f, 0.25f,
                    -1.0f, false, 0.0f, true, -1.0f };
        p.stages[0] = { 14.0f, -0.06f, 0.18f, 55.0f, 15000.0f, -5.0f, 4, 0.4f, 0.18f, 0.12f, 0.5f };
        p.stages[1] = { 10.0f, 0.08f, 0.24f, 48.0f, 13000.0f, -4.0f, 4, 0.45f, 0.22f, 0.1f, 0.58f };
        p.toneStack = { passiveStack, 0.62f, 0.45f, 0.52f, 520.0f, 0.7f };
        p.phaseInverter = { 1.35f, 0.9f, 0.2f, 0.09f, 0.18f };
        p.powerAmp = { -1.5f, 0.62f, 0.42f, 0.62f, -0.08f, 0.48f, 0.65f, 0.22f, 55.0f, 650.0f };
        break;

    case Topology::americanClean:
        /* Headroom is the character. Two low-gain stages and a power section that refuses to
           move: high damping and heavy global feedback hold the output stiff, and the
           saturation term is the lowest of any voicing here. The scooped mid and the +3.5 dB
           pick emphasis are the bright-cap-and-scoop shape a blackface circuit has before
           anything is turned up. */
        preset.name = guitar ? "Original American Clean" : "Original Clean Bass";
        p.stageCount = 2;
        p.preEq = { guitar ? 78.0f : 42.0f, 18000.0f, 0.45f,
                    3.5f, true, -2.0f, true, -2.0f };
        p.stages[0] = { 6.0f, 0.02f, 0.06f, 75.0f, 14000.0f, -3.0f, 2, 0.18f, 0.1f, 0.08f, 0.15f };
        p.stages[1] = { 7.5f, -0.02f, 0.08f, 70.0f, 13000.0f, -3.5f, 2, 0.2f, 0.12f, 0.06f, 0.18f };
        p.toneStack = { passiveStack, 0.55f, 0.38f, 0.62f, guitar ? 620.0f : 480.0f, 0.6f };
        p.phaseInverter = { 1.1f, 1.15f, 0.06f, 0.03f, 0.35f };
        p.powerAmp = { -2.0f, 0.28f, 0.85f, 0.22f, 0.02f, 0.62f, 0.45f, 0.55f, 45.0f, 300.0f };
        break;

    case Topology::britishCrunch:
        /* Not Vintage Bloom with more gain. The distortion is meant to come from the power
           section rather than from a stack of preamp stages, so this runs the highest power
           saturation of the seven against the *lowest* global feedback and damping -- an
           output stage with nothing holding it down. The mid emphasis and the mid-forward
           stack are the other half: this is the voicing that cuts rather than scoops. */
        preset.name = guitar ? "Original British Crunch" : "Original British Bass";
        p.stageCount = 3;
        p.preEq = { guitar ? 70.0f : 40.0f, 17500.0f, 0.4f,
                    1.5f, false, 0.0f, true, 2.0f };
        p.stages[0] = { 10.0f, 0.05f, 0.14f, 68.0f, 13500.0f, -6.0f, 4, 0.3f, 0.24f, 0.16f, 0.34f };
        p.stages[1] = { 12.5f, -0.04f, 0.18f, 64.0f, 12800.0f, -6.0f, 4, 0.32f, 0.26f, 0.14f, 0.38f };
        p.stages[2] = { 13.0f, 0.06f, 0.22f, 60.0f, 12000.0f, -5.5f, 4, 0.34f, 0.28f, 0.12f, 0.42f };
        p.toneStack = { passiveStack, 0.45f, 0.62f, 0.6f, guitar ? 720.0f : 560.0f, 0.9f };
        p.phaseInverter = { 1.9f, 0.7f, 0.22f, 0.11f, 0.12f };
        p.powerAmp = { -2.5f, 0.68f, 0.35f, 0.45f, -0.04f, 0.55f, 0.6f, 0.12f, 40.0f, 480.0f };
        break;

    case Topology::classAChime:
        /* Cathode-biased and running no global feedback worth the name: `feedback` at 0.03 and
           `damping` at 0.15 are the defining numbers, not the EQ. Positive `biasCharacter` is
           the cathode bias, the high cut is open to 20 kHz, and presence sits high against a
           low resonance -- bright and airy rather than big. */
        preset.name = guitar ? "Original Class-A Chime" : "Original Class-A Bass";
        p.stageCount = 2;
        p.preEq = { guitar ? 85.0f : 46.0f, 20000.0f, 0.35f,
                    4.0f, false, 0.0f, false, 0.0f };
        p.stages[0] = { 9.0f, 0.07f, 0.2f, 80.0f, 15500.0f, -4.5f, 4, 0.36f, 0.16f, 0.1f, 0.3f };
        p.stages[1] = { 11.0f, -0.05f, 0.26f, 76.0f, 14500.0f, -4.0f, 4, 0.4f, 0.2f, 0.08f, 0.36f };
        p.toneStack = { passiveStack, 0.35f, 0.5f, 0.72f, guitar ? 900.0f : 700.0f, 0.7f };
        p.phaseInverter = { 1.4f, 0.65f, 0.28f, 0.18f, 0.05f };
        p.powerAmp = { -1.0f, 0.72f, 0.15f, 0.7f, 0.14f, 0.7f, 0.3f, 0.03f, 30.0f, 520.0f };
        break;

    case Topology::saggingRectifier:
        /* The loose counterpart to Tight Modern: same four stages, opposite power section.
           Sag is the highest here at 0.72 with a 70 ms attack and a 900 ms recovery, so the
           supply is still coming back when the next chord lands. The stages carry high
           `memoryAmount` and `attackReduction` for the blocking distortion that goes with it.
           The low cut is *higher* than Tight Modern's, not lower -- the bloom is meant to come
           from the supply, and letting the bottom octave through as well only makes it mud. */
        preset.name = guitar ? "Original Sagging Rectifier" : "Original Sagging Bass";
        p.stageCount = 4;
        p.preEq = { guitar ? 105.0f : 52.0f, 15500.0f, 0.55f,
                    1.0f, true, -1.0f, true, -2.5f };
        for (std::size_t index = 0; index < p.stages.size(); ++index)
        {
            p.stages[index] = { 12.0f + static_cast<float>(index) * 2.7f,
                index % 2 == 0 ? -0.05f : 0.03f, 0.16f + 0.035f * static_cast<float>(index),
                guitar ? 100.0f : 50.0f, 12800.0f - 700.0f * index,
                -7.0f, 4, 0.34f, 0.3f, 0.35f, 0.5f };
        }
        p.toneStack = { passiveStack, 0.6f, 0.3f, 0.62f, guitar ? 500.0f : 420.0f, 1.0f };
        p.phaseInverter = { 1.6f, 0.72f, 0.16f, 0.07f, 0.24f };
        p.powerAmp = { -5.0f, 0.55f, 0.5f, 0.72f, -0.1f, 0.5f, 0.72f, 0.3f, 70.0f, 900.0f };
        break;

    case Topology::studioDirect:
        /* An active front end rather than a valve one, and the only voicing that takes
           `activeThreeBand` on both instruments -- a flat, non-interacting three-band is what
           an active preamp has, and a passive stack here would contradict the whole idea.
           Everything else is set to get out of the way: minimal drive and asymmetry, almost no
           supply sag, and the stiffest output stage of the seven. This is the DI voicing. */
        preset.name = guitar ? "Original Studio Direct" : "Original Studio Bass";
        p.stageCount = 2;
        p.preEq = { guitar ? 60.0f : 30.0f, 20000.0f, 0.5f,
                    0.0f, false, 0.0f, false, 0.0f };
        p.stages[0] = { 4.0f, 0.0f, 0.02f, 40.0f, 18000.0f, -2.0f, 2, 0.05f, 0.05f, 0.02f, 0.05f };
        p.stages[1] = { 5.0f, 0.0f, 0.03f, 36.0f, 17000.0f, -2.0f, 2, 0.06f, 0.05f, 0.02f, 0.06f };
        p.toneStack = { ToneStackType::activeThreeBand, 0.5f, 0.5f, 0.5f,
                        guitar ? 800.0f : 600.0f, 0.9f };
        p.phaseInverter = { 1.0f, 1.4f, 0.02f, 0.01f, 0.4f };
        p.powerAmp = { -1.0f, 0.12f, 0.9f, 0.05f, 0.0f, 0.5f, 0.5f, 0.6f, 20.0f, 200.0f };
        break;

    /* The bass-native block. What separates these from the seven above is not that their numbers
       are lower -- it is that they were reasoned from bass circuits rather than retuned from
       guitar ones, and the difference shows up in one place above all: **the preamp stages keep
       their bottom octave.** Every guitar voicing cuts at 38 Hz or higher even in its bass
       reading, because on a guitar the bottom octave is nothing but mud ahead of a distorting
       stage. On a bass it is the note, and a voicing that filters it out before the valves is a
       guitar amplifier being polite about it.

       Each still answers for guitar, because a host can write any (instrument, topology) pair and
       must not get an uninitialised preset back. Those readings raise the cuts the way the seven
       above lower theirs, and are not expected to be anybody's favourite amplifier.
    */
    case Topology::valveFlagship:
        /* The reference all-tube bass head. Stage low cuts at 32-38 Hz are the whole point (see
           above), and `frequencySaturation` at 0.34 is what makes that retained low end saturate
           differently from the midrange rather than merely being present.

           `feedback` at 0.32 is deliberate and contested: the circuit this is reasoned from is
           often described as having no global feedback loop, and it does have one -- from the
           output transformer secondary back to the phase inverter cathode, which is what sets its
           damping factor. A near-zero here gives a flabby output stage that contradicts the tight
           roar the same amplifier is prized for.

           The phase inverter runs hotter than its `britishCrunch` counterpart because it is
           standing in for two tiers: the real circuit puts a driver stage between the inverter and
           the output valves, and there is nowhere else for that gain to live.
        */
        preset.name = guitar ? "Original Valve Flagship" : "Original Valve Flagship Bass";
        p.stageCount = 3;
        p.preEq = { guitar ? 72.0f : 30.0f, 14000.0f, 0.35f, 1.0f, false, 0.0f, true, 2.5f };
        p.stages[0] = { 9.0f, 0.05f, 0.14f, guitar ? 68.0f : 32.0f, 11000.0f,
                        -6.0f, 4, 0.30f, 0.30f, 0.20f, 0.35f };
        p.stages[1] = { 12.0f, -0.04f, 0.17f, guitar ? 70.0f : 34.0f, 10000.0f,
                        -6.0f, 4, 0.32f, 0.32f, 0.22f, 0.38f };
        p.stages[2] = { 14.0f, 0.06f, 0.20f, guitar ? 74.0f : 38.0f, 9000.0f,
                        -6.0f, 4, 0.34f, 0.34f, 0.24f, 0.42f };
        p.toneStack = { passiveStack, 0.55f, 0.60f, 0.50f, guitar ? 850.0f : 700.0f, 0.70f };
        p.phaseInverter = { 1.75f, 0.90f, 0.15f, 0.07f, 0.22f };
        p.powerAmp = { -3.5f, 0.50f, 0.55f, 0.55f, -0.06f, 0.45f, 0.70f, 0.32f, 45.0f, 620.0f };
        if (! guitar)
        {
            // `cleanBlend` is low on purpose. This amplifier's identity is that *everything* goes
            // through the valves; a high clean blend turns it into a DI with a valve garnish.
            p.bass = { 140.0f, 0.35f, 0.40f, 4.0f, 1.0f, true, 2.6f, 0.0f, 0.0f,
                       dsp::Waveshape::hyperbolicTangent, true };
            p.cabinet.bassDiBlend = 0.15f;
        }
        break;

    case Topology::cathodeVintage:
        /* The small cathode-biased combo, and the one voicing here whose character is entirely
           dynamic rather than spectral: `memoryAmount` 0.55 against `sag` 0.70 means it is
           compressing on every note and never fully recovered. Positive `biasCharacter` is the
           cathode bias, which is where its even harmonics come from.

           Two details are worth defending because they look like mistakes. The 9 kHz `highCutHz`
           is real -- this amplifier has no top end, and that is precisely why it sits under a
           vocal instead of fighting one. And it takes `activeThreeBand` on **both** instruments,
           where every other valve voicing takes the passive stack: the circuit has a Baxandall
           tone control with no mid knob at all, so `mid` is pinned and its Q kept low rather than
           modelling a control that does not exist.

           The phase inverter is a split-load type, which runs out of drive long before a
           long-tailed pair would and clips lopsidedly when it does -- hence the lowest `headroom`
           of any voicing in this file and the high `differentialImbalance`.
        */
        preset.name = guitar ? "Original Cathode Vintage" : "Original Cathode Vintage Bass";
        p.stageCount = 2;
        p.preEq = { guitar ? 68.0f : 35.0f, 9000.0f, 0.20f, -1.5f, true, 2.0f, false, 0.0f };
        p.stages[0] = { 11.0f, -0.07f, 0.22f, guitar ? 62.0f : 30.0f, 7000.0f,
                        -4.0f, 4, 0.45f, 0.20f, 0.10f, 0.52f };
        p.stages[1] = { 13.0f, 0.06f, 0.28f, guitar ? 62.0f : 30.0f, 6000.0f,
                        -4.0f, 4, 0.48f, 0.22f, 0.08f, 0.58f };
        p.toneStack = { ToneStackType::activeThreeBand, 0.60f, 0.50f, 0.35f,
                        guitar ? 650.0f : 500.0f, 0.50f };
        p.phaseInverter = { 1.3f, 0.55f, 0.24f, 0.18f, 0.14f };
        p.powerAmp = { -1.0f, 0.66f, 0.30f, 0.70f, 0.14f, 0.30f, 0.60f, 0.10f, 60.0f, 780.0f };
        if (! guitar)
        {
            p.bass = { 200.0f, 0.25f, 0.55f, 3.0f, 1.0f, true, 2.6f, 0.0f, 0.0f,
                       dsp::Waveshape::hyperbolicTangent, true };
            p.cabinet.bassDiBlend = 0.10f;
            p.outputGainDb = -4.0f;
        }
        break;

    case Topology::hybridMosfet:
        /* Valve preamp, power section that refuses to move. `saturation` 0.08 against `damping`
           0.95 and `feedback` 0.65 is the whole voicing: three valve stages contribute asymmetry
           and a little compression, and then nothing downstream of them ever clips.

           This is the voicing to reach for when the amplifier is meant to disappear -- slap, funk,
           modern session playing -- and it is deliberately the home for a scooped preset rather
           than a scooped voicing. A tone shape is not a circuit and does not deserve its own index.
        */
        preset.name = guitar ? "Original Hybrid MOSFET" : "Original Hybrid MOSFET Bass";
        p.stageCount = 3;
        p.preEq = { guitar ? 64.0f : 28.0f, 19000.0f, 0.40f, 0.5f, false, 0.0f, true, -1.0f };
        p.stages[0] = { 6.0f, 0.03f, 0.09f, guitar ? 58.0f : 26.0f, 16000.0f,
                        -3.0f, 4, 0.20f, 0.12f, 0.06f, 0.16f };
        p.stages[1] = { 7.0f, -0.02f, 0.11f, guitar ? 58.0f : 26.0f, 15000.0f,
                        -3.0f, 4, 0.20f, 0.12f, 0.06f, 0.18f };
        p.stages[2] = { 8.0f, 0.03f, 0.13f, guitar ? 58.0f : 26.0f, 14000.0f,
                        -3.0f, 4, 0.22f, 0.12f, 0.06f, 0.20f };
        p.toneStack = { passiveStack, 0.55f, 0.48f, 0.55f, guitar ? 750.0f : 600.0f, 0.65f };
        p.phaseInverter = { 1.05f, 1.35f, 0.05f, 0.02f, 0.42f };
        p.powerAmp = { -1.5f, 0.08f, 0.95f, 0.03f, 0.01f, 0.55f, 0.50f, 0.65f, 18.0f, 160.0f };
        if (! guitar)
        {
            p.bass = { 120.0f, 0.50f, 0.30f, 2.0f, 1.0f, false, 2.6f, 0.0f, 0.0f,
                       dsp::Waveshape::hyperbolicTangent, true };
            p.cabinet.bassDiBlend = 0.35f;
        }
        break;

    case Topology::shortPathGrind:
        /* Two stages and out. The exception that proves the rule at the head of this block: it
           carries the **highest** preamp low cut here, not the lowest, because the circuit it is
           reasoned from genuinely has no bottom octave -- and that absence is the character rather
           than a defect. Cutting the fundamental is also what keeps the grind from turning to mud.

           The only bass-native voicing that takes `passiveCoupled` on bass as well as guitar: the
           circuit really does have a passive interacting stack, so the semi-parametric would be
           modelling a control the amplifier does not have.
        */
        preset.name = guitar ? "Original Short-Path Grind" : "Original Short-Path Grind Bass";
        p.stageCount = 2;
        p.preEq = { guitar ? 88.0f : 55.0f, 12000.0f, 0.45f, 1.5f, false, 0.0f, true, 3.5f };
        p.stages[0] = { 14.0f, -0.09f, 0.30f, guitar ? 92.0f : 60.0f, 10000.0f,
                        -6.0f, 4, 0.42f, 0.35f, 0.28f, 0.50f };
        p.stages[1] = { 17.0f, 0.07f, 0.34f, guitar ? 88.0f : 55.0f, 9000.0f,
                        -6.0f, 4, 0.44f, 0.36f, 0.30f, 0.54f };
        p.toneStack = { ToneStackType::passiveCoupled, 0.50f, 0.62f, 0.48f,
                        guitar ? 620.0f : 480.0f, 0.85f };
        p.phaseInverter = { 1.85f, 0.60f, 0.30f, 0.16f, 0.08f };
        p.powerAmp = { -3.0f, 0.75f, 0.28f, 0.62f, -0.10f, 0.50f, 0.55f, 0.07f, 38.0f, 560.0f };
        if (! guitar)
        {
            p.bass = { 220.0f, 0.30f, 0.45f, 6.0f, 1.0f, true, 2.6f, 0.0f, 0.0f,
                       dsp::Waveshape::hyperbolicTangent, true };
            p.cabinet.bassDiBlend = 0.10f;
            p.outputGainDb = -4.0f;
        }
        break;

    case Topology::solidStateBiAmp:
        /* The first voicing here that is not a valve amplifier at all, and the thing that makes it
           one is not its EQ -- it is `memoryAmount` at 0.03 across both stages and `sag` at 0.02.
           Every valve voicing in this file runs 0.16 or more. A transistor has no supply memory:
           it does not slump under a chord and come back, it delivers the same volt until it runs
           out and then delivers no more. Take that away and the tightest EQ in the world still
           sounds like a valve amp being played carefully.

           `hardClip` on both stages and on the output stage is the other half. `damping` at 0.92
           against `feedback` 0.70 is the high damping factor a solid-state power section actually
           advertises, and the 8x oversampling is what makes a clamped curve survive being sampled
           (see the fold-back table in nts_amp_tests -- 8x alone, with no ADAA, is what carries it).

           **The crossover at 500 Hz is the voicing.** Every other bass path in this file splits at
           120-220 Hz, where the job is to keep a low B out of the distortion. Splitting at 500
           does something categorically different: the whole body of every note below the fifth
           fret passes through the clean 300-watt side untouched, and only the attack transient and
           the harmonics reach the driven one. That is why a bi-amp clanks without ever sounding
           fuzzy, and it is unreachable from any other voicing here.
        */
        preset.name = guitar ? "Original Solid-State Bi-Amp" : "Original Solid-State Bi-Amp Bass";
        p.stageCount = 2;
        p.preEq = { guitar ? 70.0f : 40.0f, 18000.0f, 0.70f, 4.0f, true, -1.0f, true, -2.5f };
        p.stages[0] = { 7.0f, 0.0f, 0.03f, guitar ? 72.0f : 42.0f, 17000.0f,
                        -3.0f, 8, 0.02f, 0.05f, 0.02f, 0.03f };
        p.stages[1] = { 9.0f, 0.0f, 0.03f, guitar ? 72.0f : 42.0f, 16000.0f,
                        -3.0f, 8, 0.02f, 0.05f, 0.02f, 0.03f };
        for (std::size_t index = 0; index < 2; ++index) p.stages[index].shape = dsp::Waveshape::hardClip;
        // An active four-band is what this circuit has; a passive interacting stack would be
        // modelling component values that are not in it.
        p.toneStack = { ToneStackType::activeThreeBand, 0.55f, 0.40f, 0.62f,
                        guitar ? 900.0f : 800.0f, 1.10f };
        // There is no phase inverter in a solid-state amplifier. These values are it getting out
        // of the way: unity drive, headroom above anything that will reach it, no asymmetry.
        p.phaseInverter = { 1.0f, 1.40f, 0.01f, 0.0f, 0.45f };
        p.powerAmp = { -2.0f, 0.35f, 0.92f, 0.02f, 0.0f, 0.60f, 0.40f, 0.70f, 15.0f, 120.0f };
        p.powerAmp.shape = dsp::Waveshape::hardClip;
        if (! guitar)
        {
            // Low band clean and level, high band driven and trimmed back to sit under it.
            p.bass = { 500.0f, 0.50f, 0.30f, 8.0f, 1.0f, false, 2.6f, 0.0f, -2.0f,
                       dsp::Waveshape::hardClip, true };
            p.cabinet.bassDiBlend = 0.30f;
        }
        break;

    case Topology::cmosModern:
        /* One hard clipping stage, not a cascade -- and that is the whole architecture.

           The circuit this is reasoned from deliberately overdrives a *single* CMOS stage where
           the designs it was reacting against cascade several, and the difference is audible: a
           cascade rounds progressively and ends up sounding like a very angry valve, while one
           stage driven to +24 dB into a clamp gives the flat-topped, metallic edge the voicing
           exists for. The second stage here is make-up and filtering, and stays linear.

           **Two numbers do the work, and neither is the gain.** The stage low cut sits at 150 Hz,
           so the distortion engine never sees the fundamental at all; and `dryBlend` at 0.40 sums
           that untouched fundamental back over the top. Without the blend this voicing has no low
           end whatever -- which is why it could not ship until Track D, and why it is the one
           voicing in this file that is not an approximation of anything without it.

           The power section is Class D and is modelled as what that is: `saturation` 0.06, `sag`
           0, `damping` 0.95. It does not contribute character and is not supposed to.
        */
        preset.name = guitar ? "Original CMOS Modern" : "Original CMOS Modern Bass";
        p.stageCount = 2;
        p.preEq = { guitar ? 75.0f : 45.0f, 20000.0f, 0.85f, 5.0f, true, -2.0f, true, 3.0f };
        p.stages[0] = { 24.0f, 0.02f, 0.06f, guitar ? 180.0f : 150.0f, 9000.0f,
                        -12.0f, 8, 0.05f, 0.10f, 0.03f, 0.05f };
        p.stages[0].shape = dsp::Waveshape::hardClip;
        // Linear make-up: the clipping happened upstream, and a second curve here would only
        // round the edge the first one was chosen to produce.
        p.stages[1] = { 4.0f, 0.0f, 0.02f, guitar ? 150.0f : 120.0f, 8000.0f,
                        -3.0f, 4, 0.03f, 0.05f, 0.02f, 0.03f };
        p.toneStack = { ToneStackType::activeThreeBand, 0.60f, 0.42f, 0.60f,
                        guitar ? 1000.0f : 900.0f, 1.30f };
        p.phaseInverter = { 1.0f, 1.45f, 0.02f, 0.0f, 0.50f };
        p.powerAmp = { -1.0f, 0.06f, 0.95f, 0.0f, 0.0f, 0.55f, 0.45f, 0.75f, 12.0f, 100.0f };
        // The fundamental, brought back untouched. Latency-aligned; see AmpVoice::dryDelay.
        p.dryBlend = 0.40f;
        p.outputGainDb = -6.0f;
        if (! guitar)
        {
            p.bass = { 150.0f, 0.50f, 0.60f, 0.0f, 1.0f, false, 2.6f, 0.0f, 0.0f,
                       dsp::Waveshape::hyperbolicTangent, true };
            p.cabinet.bassDiBlend = 0.45f;
        }
        break;
    }
    return preset;
}

std::string serializePresetLegacy(const AmpPreset& preset, bool pretty)
{
    const auto separator = pretty ? "\n  " : "";
    std::ostringstream stream;
    stream << std::setprecision(8) << '{' << separator
           << "\"schemaVersion\":" << preset.schemaVersion << ',' << separator
           << "\"name\":\"" << preset.name << "\"," << separator
           << "\"instrument\":\"" << (preset.parameters.instrument == Instrument::guitar ? "guitar" : "bass") << "\"," << separator
           << "\"topology\":\"" << topologyKey(preset.parameters.topology) << "\"," << separator
           << "\"pickup\":\"" << (preset.parameters.pickup == PickupProfile::passive ? "passive" : "active") << "\"," << separator
           << "\"inputCalibrationDb\":" << preset.parameters.manualInputTrimDb << ',' << separator
           << "\"targetRmsLowDb\":" << preset.calibration.targetRmsLowDb << ',' << separator
           << "\"targetRmsHighDb\":" << preset.calibration.targetRmsHighDb << ',' << separator
           << "\"targetPeakDb\":" << preset.calibration.targetPeakDb << ',' << separator
           << "\"lowCutHz\":" << preset.parameters.preEq.lowCutHz << ',' << separator
           << "\"highCutHz\":" << preset.parameters.preEq.highCutHz << ',' << separator
           << "\"tightness\":" << preset.parameters.preEq.tightness << ',' << separator
           << "\"pickEmphasisDb\":" << preset.parameters.preEq.pickEmphasisDb << ',' << separator
           << "\"stageCount\":" << preset.parameters.stageCount << ',' << separator;
    for (std::size_t stage = 0; stage < preset.parameters.stages.size(); ++stage)
    {
        const auto& config = preset.parameters.stages[stage];
        stream << "\"stage" << stage << "DriveDb\":" << config.driveDb << ',' << separator
               << "\"stage" << stage << "Bias\":" << config.bias << ',' << separator
               << "\"stage" << stage << "Asymmetry\":" << config.asymmetry << ',' << separator
               << "\"stage" << stage << "Oversampling\":" << config.oversamplingFactor << ',' << separator;
    }
    stream << "\"bass\":" << preset.parameters.toneStack.bass << ',' << separator
           << "\"mid\":" << preset.parameters.toneStack.mid << ',' << separator
           << "\"treble\":" << preset.parameters.toneStack.treble << ',' << separator
           << "\"presence\":" << preset.parameters.powerAmp.presence << ',' << separator
           << "\"resonance\":" << preset.parameters.powerAmp.resonance << ',' << separator
           << "\"masterDb\":" << preset.parameters.powerAmp.masterDb << ',' << separator
           << "\"sag\":" << preset.parameters.powerAmp.sag << ',' << separator
           << "\"feedback\":" << preset.parameters.powerAmp.feedback << ',' << separator
           << "\"crossoverHz\":" << preset.parameters.bass.crossoverHz << ',' << separator
           << "\"cleanBlend\":" << preset.parameters.bass.cleanBlend << ',' << separator
           << "\"cabinetBlend\":" << preset.parameters.cabinet.blend << ',' << separator
           << "\"cabinetWidth\":" << preset.parameters.cabinet.width << ',' << separator
           << "\"cabinetBypass\":" << (preset.parameters.cabinet.bypass ? "true" : "false") << ',' << separator
           << "\"cabinetAlignment\":" << preset.parameters.cabinet.slots[1].delaySamples << ',' << separator
           << "\"postLowDb\":" << preset.parameters.postLowDb << ',' << separator
           << "\"postMidDb\":" << preset.parameters.postMidDb << ',' << separator
           << "\"postHighDb\":" << preset.parameters.postHighDb << ',' << separator
           << "\"outputGainDb\":" << preset.parameters.outputGainDb << (pretty ? "\n" : "") << '}';
    return stream.str();
}

std::string serializePreset(const AmpPreset& preset, bool pretty)
{
    const auto newline = pretty ? "\n" : "";
    const auto indent = pretty ? "  " : "";
    const auto& p = preset.parameters;
    std::ostringstream stream;
    stream << std::setprecision(8) << '{' << newline
           << indent << "\"schemaVersion\":" << preset.schemaVersion << ',' << newline
           << indent << "\"name\":\"" << preset.name << "\"," << newline
           << indent << "\"instrument\":\"" << (p.instrument == Instrument::guitar ? "guitar" : "bass") << "\"," << newline
           << indent << "\"topology\":\"" << topologyKey(p.topology) << "\"," << newline
           << indent << "\"pickup\":\"" << (p.pickup == PickupProfile::passive ? "passive" : "active") << "\"," << newline
           << indent << "\"inputCalibration\":{"
           << "\"inputCalibrationDb\":" << p.manualInputTrimDb
           << ",\"targetRmsLowDb\":" << preset.calibration.targetRmsLowDb
           << ",\"targetRmsHighDb\":" << preset.calibration.targetRmsHighDb
           << ",\"targetPeakDb\":" << preset.calibration.targetPeakDb << "}," << newline
           << indent << "\"gate\":{"
           << "\"enabled\":" << (p.gateEnabled ? "true" : "false")
           << ",\"thresholdDb\":" << p.gateThresholdDb
           << ",\"depthDb\":" << p.gateDepthDb
           << ",\"attackMs\":" << p.gateAttackMs
           << ",\"holdMs\":" << p.gateHoldMs
           << ",\"releaseMs\":" << p.gateReleaseMs << "}," << newline
           << indent << "\"preEq\":{"
           << "\"lowCutHz\":" << p.preEq.lowCutHz << ",\"highCutHz\":" << p.preEq.highCutHz
           << ",\"tightness\":" << p.preEq.tightness << ",\"pickEmphasisDb\":" << p.preEq.pickEmphasisDb
           << ",\"lowShelfEnabled\":" << (p.preEq.lowShelfEnabled ? "true" : "false")
           << ",\"lowShelfDb\":" << p.preEq.lowShelfDb
           << ",\"midEmphasisEnabled\":" << (p.preEq.midEmphasisEnabled ? "true" : "false")
           << ",\"midEmphasisDb\":" << p.preEq.midEmphasisDb
           << ",\"lowShelfHz\":" << p.preEq.lowShelfHz
           << ",\"midEmphasisHz\":" << p.preEq.midEmphasisHz
           << ",\"highShelfEnabled\":" << (p.preEq.highShelfEnabled ? "true" : "false")
           << ",\"highShelfDb\":" << p.preEq.highShelfDb
           << ",\"highShelfHz\":" << p.preEq.highShelfHz << "}," << newline
           << indent << "\"preampStages\":{\"stageCount\":" << p.stageCount;
    for (std::size_t stage = 0; stage < p.stages.size(); ++stage)
    {
        const auto& config = p.stages[stage];
        const auto prefix = std::string("stage") + std::to_string(stage);
        stream << ",\"" << prefix << "DriveDb\":" << config.driveDb
               << ",\"" << prefix << "Bias\":" << config.bias
               << ",\"" << prefix << "Asymmetry\":" << config.asymmetry
               << ",\"" << prefix << "LowCutHz\":" << config.lowCutHz
               << ",\"" << prefix << "HighCutHz\":" << config.highCutHz
               << ",\"" << prefix << "OutputTrimDb\":" << config.outputTrimDb
               << ",\"" << prefix << "Oversampling\":" << config.oversamplingFactor
               << ",\"" << prefix << "DynamicBias\":" << config.dynamicBias
               << ",\"" << prefix << "FrequencySaturation\":" << config.frequencySaturation
               << ",\"" << prefix << "AttackReduction\":" << config.attackReduction
               << ",\"" << prefix << "MemoryAmount\":" << config.memoryAmount
               << ",\"" << prefix << "Shape\":" << static_cast<int>(config.shape)
               << ",\"" << prefix << "Antialiased\":" << (config.antialiasedSaturation ? "true" : "false");
    }
    stream << "}," << newline
           << indent << "\"toneStack\":{\"toneStackType\":" << static_cast<int>(p.toneStack.type)
           << ",\"bass\":" << p.toneStack.bass << ",\"mid\":" << p.toneStack.mid
           << ",\"treble\":" << p.toneStack.treble
           << ",\"midFrequencyHz\":" << p.toneStack.midFrequencyHz << ",\"midQ\":" << p.toneStack.midQ << "}," << newline
           << indent << "\"phaseInverter\":{\"piDrive\":" << p.phaseInverter.drive
           << ",\"piHeadroom\":" << p.phaseInverter.headroom
           << ",\"piAsymmetry\":" << p.phaseInverter.asymmetry
           << ",\"differentialImbalance\":" << p.phaseInverter.differentialImbalance
           << ",\"piFeedback\":" << p.phaseInverter.feedback << "}," << newline
           << indent << "\"powerAmp\":{\"masterDb\":" << p.powerAmp.masterDb
           << ",\"saturation\":" << p.powerAmp.saturation << ",\"damping\":" << p.powerAmp.damping
           << ",\"sag\":" << p.powerAmp.sag << ",\"biasCharacter\":" << p.powerAmp.biasCharacter
           << ",\"presence\":" << p.powerAmp.presence << ",\"resonance\":" << p.powerAmp.resonance
           << ",\"feedback\":" << p.powerAmp.feedback << ",\"sagAttackMs\":" << p.powerAmp.sagAttackMs
           << ",\"sagRecoveryMs\":" << p.powerAmp.sagRecoveryMs
           << ",\"powerShape\":" << static_cast<int>(p.powerAmp.shape) << "}," << newline
           << indent << "\"cabinet\":{\"cabinetBlend\":" << p.cabinet.blend
           << ",\"cabinetWidth\":" << p.cabinet.width
           << ",\"phaseInvertB\":" << (p.cabinet.slots[1].phaseInvert ? "true" : "false")
           << ",\"cabinetAlignment\":" << p.cabinet.slots[1].delaySamples
           << ",\"cabinetLowCutHz\":" << p.cabinet.lowCutHz
           << ",\"cabinetHighCutHz\":" << p.cabinet.highCutHz
           << ",\"cabinetBypass\":" << (p.cabinet.bypass ? "true" : "false")
           << ",\"bassDiBlend\":" << p.cabinet.bassDiBlend
           /* The per-slot block, appended. Every key here is optional on the way back in and
              defaults to the value the field had before it existed, so a preset written by an
              older build deserialises to exactly the cabinet it described. The two historical
              keys above keep their names for the same reason -- `phaseInvertB` and
              `cabinetAlignment` are slot B's phase and delay, and renaming them in the file
              would orphan every preset anyone has saved. */
           << ",\"cabinetOutputTrimDb\":" << p.cabinet.outputTrimDb
           << ",\"phaseInvertA\":" << (p.cabinet.slots[0].phaseInvert ? "true" : "false")
           << ",\"cabinetAlignmentA\":" << p.cabinet.slots[0].delaySamples
           << ",\"cabinetLevelADb\":" << p.cabinet.slots[0].levelDb
           << ",\"cabinetLevelBDb\":" << p.cabinet.slots[1].levelDb
           << ",\"cabinetPanA\":" << p.cabinet.slots[0].pan
           << ",\"cabinetPanB\":" << p.cabinet.slots[1].pan
           << ",\"cabinetMuteA\":" << (p.cabinet.slots[0].mute ? "true" : "false")
           << ",\"cabinetMuteB\":" << (p.cabinet.slots[1].mute ? "true" : "false") << "}," << newline
           << indent << "\"bassPath\":{\"crossoverHz\":" << p.bass.crossoverHz
           << ",\"cleanBlend\":" << p.bass.cleanBlend << ",\"lowCompression\":" << p.bass.lowCompression
           << ",\"highDriveDb\":" << p.bass.highDriveDb << ",\"lowMono\":" << p.bass.lowMono
           << ",\"lowSaturation\":" << (p.bass.lowSaturation ? "true" : "false")
           << ",\"lowDriveDb\":" << p.bass.lowDriveDb
           << ",\"lowLevelDb\":" << p.bass.lowLevelDb
           << ",\"highLevelDb\":" << p.bass.highLevelDb
           << ",\"lowShape\":" << static_cast<int>(p.bass.lowShape)
           << ",\"alignBands\":" << (p.bass.alignBands ? "true" : "false") << "}," << newline
           << indent << "\"postEq\":{\"postLowDb\":" << p.postLowDb << ",\"postMidDb\":" << p.postMidDb
           << ",\"postHighDb\":" << p.postHighDb << "}," << newline
           << indent << "\"dryBlend\":" << p.dryBlend << ',' << newline
           << indent << "\"outputGainDb\":" << p.outputGainDb << ',' << newline
           << indent << "\"loudnessMatch\":" << (p.loudnessMatch ? "true" : "false") << newline << '}';
    return stream.str();
}

std::optional<AmpPreset> deserializePreset(std::string_view json)
{
    const auto version = jsonNumber(json, "schemaVersion");
    const auto name = jsonString(json, "name");
    if (! version || static_cast<int>(*version) != 1 || ! name) return std::nullopt;
    const auto instrumentName = jsonString(json, "instrument").value_or("guitar");
    // An unrecognised name falls back rather than failing the load: a preset written by a
    // later build naming a voicing this one has never heard of still carries usable values for
    // every other field, and rejecting the file outright would lose all of them.
    const auto topology = topologyFromKey(jsonString(json, "topology").value_or("tightModern"))
                              .value_or(Topology::tightModern);
    auto preset = makeOriginalPreset(topology,
                                     instrumentName == "bass" ? Instrument::bass : Instrument::guitar);
    preset.name = *name;
    auto& p = preset.parameters;
    p.pickup = jsonString(json, "pickup").value_or("passive") == "active" ? PickupProfile::active : PickupProfile::passive;
    p.manualInputTrimDb = numberOr(json, "inputCalibrationDb", p.manualInputTrimDb);
    preset.calibration.targetRmsLowDb = numberOr(json, "targetRmsLowDb", preset.calibration.targetRmsLowDb);
    preset.calibration.targetRmsHighDb = numberOr(json, "targetRmsHighDb", preset.calibration.targetRmsHighDb);
    preset.calibration.targetPeakDb = numberOr(json, "targetPeakDb", preset.calibration.targetPeakDb);
    // Absent in presets written before the gate was controllable, so each falls
    // back to the constructed default and old files keep their previous sound.
    p.gateEnabled = jsonBool(json, "enabled", p.gateEnabled);
    p.gateThresholdDb = numberOr(json, "thresholdDb", p.gateThresholdDb);
    p.gateDepthDb = numberOr(json, "depthDb", p.gateDepthDb);
    p.gateAttackMs = numberOr(json, "attackMs", p.gateAttackMs);
    p.gateHoldMs = numberOr(json, "holdMs", p.gateHoldMs);
    p.gateReleaseMs = numberOr(json, "releaseMs", p.gateReleaseMs);
    p.preEq.lowCutHz = numberOr(json, "lowCutHz", p.preEq.lowCutHz);
    p.preEq.highCutHz = numberOr(json, "highCutHz", p.preEq.highCutHz);
    p.preEq.tightness = numberOr(json, "tightness", p.preEq.tightness);
    p.preEq.pickEmphasisDb = numberOr(json, "pickEmphasisDb", p.preEq.pickEmphasisDb);
    p.preEq.lowShelfEnabled = jsonBool(json, "lowShelfEnabled", p.preEq.lowShelfEnabled);
    p.preEq.lowShelfDb = numberOr(json, "lowShelfDb", p.preEq.lowShelfDb);
    p.preEq.midEmphasisEnabled = jsonBool(json, "midEmphasisEnabled", p.preEq.midEmphasisEnabled);
    p.preEq.midEmphasisDb = numberOr(json, "midEmphasisDb", p.preEq.midEmphasisDb);
    // Each default is the fixed value the filter used to be built at, so a preset written
    // before the frequencies were parameters is configured with exactly the same filters.
    p.preEq.lowShelfHz = numberOr(json, "lowShelfHz", p.preEq.lowShelfHz);
    p.preEq.midEmphasisHz = numberOr(json, "midEmphasisHz", p.preEq.midEmphasisHz);
    p.preEq.highShelfEnabled = jsonBool(json, "highShelfEnabled", p.preEq.highShelfEnabled);
    p.preEq.highShelfDb = numberOr(json, "highShelfDb", p.preEq.highShelfDb);
    p.preEq.highShelfHz = numberOr(json, "highShelfHz", p.preEq.highShelfHz);
    p.stageCount = static_cast<std::size_t>(std::clamp(numberOr(json, "stageCount", static_cast<float>(p.stageCount)), 2.0f, 4.0f));
    for (std::size_t stage = 0; stage < p.stages.size(); ++stage)
    {
        p.stages[stage].driveDb = numberOr(json, "stage" + std::to_string(stage) + "DriveDb", p.stages[stage].driveDb);
        p.stages[stage].bias = numberOr(json, "stage" + std::to_string(stage) + "Bias", p.stages[stage].bias);
        p.stages[stage].asymmetry = numberOr(json, "stage" + std::to_string(stage) + "Asymmetry", p.stages[stage].asymmetry);
        p.stages[stage].lowCutHz = numberOr(json, "stage" + std::to_string(stage) + "LowCutHz", p.stages[stage].lowCutHz);
        p.stages[stage].highCutHz = numberOr(json, "stage" + std::to_string(stage) + "HighCutHz", p.stages[stage].highCutHz);
        p.stages[stage].outputTrimDb = numberOr(json, "stage" + std::to_string(stage) + "OutputTrimDb", p.stages[stage].outputTrimDb);
        p.stages[stage].oversamplingFactor = static_cast<int>(numberOr(json, "stage" + std::to_string(stage) + "Oversampling", static_cast<float>(p.stages[stage].oversamplingFactor)));
        p.stages[stage].dynamicBias = numberOr(json, "stage" + std::to_string(stage) + "DynamicBias", p.stages[stage].dynamicBias);
        p.stages[stage].frequencySaturation = numberOr(json, "stage" + std::to_string(stage) + "FrequencySaturation", p.stages[stage].frequencySaturation);
        p.stages[stage].attackReduction = numberOr(json, "stage" + std::to_string(stage) + "AttackReduction", p.stages[stage].attackReduction);
        p.stages[stage].memoryAmount = numberOr(json, "stage" + std::to_string(stage) + "MemoryAmount", p.stages[stage].memoryAmount);
        // Absent in every preset saved before the field existed, and the fallback is the value
        // the preset was written with: a stage that never named a curve was a valve.
        p.stages[stage].shape = shapeFromIndex(numberOr(json, "stage" + std::to_string(stage) + "Shape",
                                                        static_cast<float>(p.stages[stage].shape)));
        p.stages[stage].antialiasedSaturation = jsonBool(json, "stage" + std::to_string(stage) + "Antialiased",
                                                         p.stages[stage].antialiasedSaturation);
    }
    p.toneStack.type = static_cast<ToneStackType>(std::clamp(static_cast<int>(numberOr(json, "toneStackType", static_cast<float>(p.toneStack.type))), 0, 2));
    p.toneStack.bass = numberOr(json, "bass", p.toneStack.bass);
    p.toneStack.mid = numberOr(json, "mid", p.toneStack.mid);
    p.toneStack.treble = numberOr(json, "treble", p.toneStack.treble);
    p.toneStack.midFrequencyHz = numberOr(json, "midFrequencyHz", p.toneStack.midFrequencyHz);
    p.toneStack.midQ = numberOr(json, "midQ", p.toneStack.midQ);
    p.phaseInverter.drive = numberOr(json, "piDrive", p.phaseInverter.drive);
    p.phaseInverter.headroom = numberOr(json, "piHeadroom", p.phaseInverter.headroom);
    p.phaseInverter.asymmetry = numberOr(json, "piAsymmetry", p.phaseInverter.asymmetry);
    p.phaseInverter.differentialImbalance = numberOr(json, "differentialImbalance", p.phaseInverter.differentialImbalance);
    p.phaseInverter.feedback = numberOr(json, "piFeedback", p.phaseInverter.feedback);
    p.powerAmp.presence = numberOr(json, "presence", p.powerAmp.presence);
    p.powerAmp.resonance = numberOr(json, "resonance", p.powerAmp.resonance);
    p.powerAmp.masterDb = numberOr(json, "masterDb", p.powerAmp.masterDb);
    p.powerAmp.sag = numberOr(json, "sag", p.powerAmp.sag);
    p.powerAmp.feedback = numberOr(json, "feedback", p.powerAmp.feedback);
    p.powerAmp.saturation = numberOr(json, "saturation", p.powerAmp.saturation);
    p.powerAmp.damping = numberOr(json, "damping", p.powerAmp.damping);
    p.powerAmp.biasCharacter = numberOr(json, "biasCharacter", p.powerAmp.biasCharacter);
    p.powerAmp.sagAttackMs = numberOr(json, "sagAttackMs", p.powerAmp.sagAttackMs);
    p.powerAmp.sagRecoveryMs = numberOr(json, "sagRecoveryMs", p.powerAmp.sagRecoveryMs);
    p.powerAmp.shape = shapeFromIndex(numberOr(json, "powerShape", static_cast<float>(p.powerAmp.shape)));
    p.bass.crossoverHz = numberOr(json, "crossoverHz", p.bass.crossoverHz);
    p.bass.cleanBlend = numberOr(json, "cleanBlend", p.bass.cleanBlend);
    p.cabinet.blend = numberOr(json, "cabinetBlend", p.cabinet.blend);
    // Absent in presets saved before the control existed, and its default of 0 is the behaviour
    // they were saved with, so an old preset reads back exactly as it sounded.
    p.cabinet.width = numberOr(json, "cabinetWidth", p.cabinet.width);
    p.cabinet.bypass = jsonBool(json, "cabinetBypass", p.cabinet.bypass);
    p.cabinet.slots[1].phaseInvert = jsonBool(json, "phaseInvertB", p.cabinet.slots[1].phaseInvert);
    p.cabinet.slots[1].delaySamples = static_cast<std::size_t>(numberOr(json, "cabinetAlignment", 0.0f));
    p.cabinet.lowCutHz = numberOr(json, "cabinetLowCutHz", p.cabinet.lowCutHz);
    p.cabinet.highCutHz = numberOr(json, "cabinetHighCutHz", p.cabinet.highCutHz);
    p.cabinet.bassDiBlend = numberOr(json, "bassDiBlend", p.cabinet.bassDiBlend);
    // Every one of these falls back to the member's own default, which is the value the field
    // effectively had before it existed -- so a preset from an older build restores the cabinet
    // it actually described rather than a re-voiced approximation of it.
    p.cabinet.outputTrimDb = numberOr(json, "cabinetOutputTrimDb", p.cabinet.outputTrimDb);
    p.cabinet.slots[0].phaseInvert = jsonBool(json, "phaseInvertA", p.cabinet.slots[0].phaseInvert);
    p.cabinet.slots[0].delaySamples = static_cast<std::size_t>(
        numberOr(json, "cabinetAlignmentA", static_cast<float>(p.cabinet.slots[0].delaySamples)));
    p.cabinet.slots[0].levelDb = numberOr(json, "cabinetLevelADb", p.cabinet.slots[0].levelDb);
    p.cabinet.slots[1].levelDb = numberOr(json, "cabinetLevelBDb", p.cabinet.slots[1].levelDb);
    p.cabinet.slots[0].pan = numberOr(json, "cabinetPanA", p.cabinet.slots[0].pan);
    p.cabinet.slots[1].pan = numberOr(json, "cabinetPanB", p.cabinet.slots[1].pan);
    p.cabinet.slots[0].mute = jsonBool(json, "cabinetMuteA", p.cabinet.slots[0].mute);
    p.cabinet.slots[1].mute = jsonBool(json, "cabinetMuteB", p.cabinet.slots[1].mute);
    p.postLowDb = numberOr(json, "postLowDb", p.postLowDb);
    p.postMidDb = numberOr(json, "postMidDb", p.postMidDb);
    p.postHighDb = numberOr(json, "postHighDb", p.postHighDb);
    // Absent before the control existed, and zero is fully wet -- which is what those presets were.
    p.dryBlend = numberOr(json, "dryBlend", p.dryBlend);
    p.bass.lowCompression = numberOr(json, "lowCompression", p.bass.lowCompression);
    p.bass.highDriveDb = numberOr(json, "highDriveDb", p.bass.highDriveDb);
    p.bass.lowMono = numberOr(json, "lowMono", p.bass.lowMono);
    p.bass.lowSaturation = jsonBool(json, "lowSaturation", p.bass.lowSaturation);
    // Each default is the value the path behaved as before the field existed, so an older preset
    // reads back as the amplifier it was saved from rather than as a re-balanced one.
    p.bass.lowDriveDb = numberOr(json, "lowDriveDb", p.bass.lowDriveDb);
    p.bass.lowLevelDb = numberOr(json, "lowLevelDb", p.bass.lowLevelDb);
    p.bass.highLevelDb = numberOr(json, "highLevelDb", p.bass.highLevelDb);
    p.bass.lowShape = shapeFromIndex(numberOr(json, "lowShape", static_cast<float>(p.bass.lowShape)));
    p.bass.alignBands = jsonBool(json, "alignBands", p.bass.alignBands);
    p.outputGainDb = numberOr(json, "outputGainDb", p.outputGainDb);
    p.loudnessMatch = jsonBool(json, "loudnessMatch", p.loudnessMatch);
    return preset;
}

void PreEq::prepare(const dsp::ProcessSpec& newSpec) noexcept
{
    spec = newSpec;
    lowCut.prepare(spec); highCut.prepare(spec); pick.prepare(spec); lowShelf.prepare(spec); mid.prepare(spec);
    highShelf.prepare(spec);
    setParameters({}); reset();
}
void PreEq::reset() noexcept
{
    lowCut.reset(); highCut.reset(); pick.reset(); lowShelf.reset(); mid.reset(); highShelf.reset();
}
void PreEq::setParameters(const PreEqParameters& p, std::size_t interpolationSamples) noexcept
{
    const auto tightCut = 35.0 + 310.0 * clamp01(p.tightness) * clamp01(p.tightness);
    lowCut.setCoefficients(dsp::BiquadCoefficients::make(dsp::FilterType::highPass, spec.sampleRate,
        std::max<double>(p.lowCutHz, tightCut), 0.707), interpolationSamples);
    highCut.setCoefficients(dsp::BiquadCoefficients::make(dsp::FilterType::lowPass, spec.sampleRate,
        p.highCutHz, 0.707), interpolationSamples);
    pick.setCoefficients(dsp::BiquadCoefficients::make(dsp::FilterType::peaking, spec.sampleRate,
        2800.0, 1.1, p.pickEmphasisDb), interpolationSamples);
    // Frequencies come from the parameters now. The defaults are the literals that used to be
    // here, so a voicing that names neither is configured with exactly the filters it always had.
    lowShelf.setCoefficients(dsp::BiquadCoefficients::make(dsp::FilterType::lowShelf, spec.sampleRate,
        std::clamp<double>(p.lowShelfHz, 20.0, 400.0), 0.707, p.lowShelfDb), interpolationSamples);
    mid.setCoefficients(dsp::BiquadCoefficients::make(dsp::FilterType::peaking, spec.sampleRate,
        std::clamp<double>(p.midEmphasisHz, 120.0, 4000.0), 0.9, p.midEmphasisDb), interpolationSamples);
    highShelf.setCoefficients(dsp::BiquadCoefficients::make(dsp::FilterType::highShelf, spec.sampleRate,
        std::clamp<double>(p.highShelfHz, 1500.0, 12000.0), 0.707, p.highShelfDb), interpolationSamples);
    useLowShelf = p.lowShelfEnabled; useMid = p.midEmphasisEnabled;
    useHighShelf = p.highShelfEnabled;
}
void PreEq::process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept
{
    lowCut.process(channels, channelCount, samples); highCut.process(channels, channelCount, samples);
    pick.process(channels, channelCount, samples);
    if (useLowShelf) lowShelf.process(channels, channelCount, samples);
    if (useMid) mid.process(channels, channelCount, samples);
    if (useHighShelf) highShelf.process(channels, channelCount, samples);
}

void ResponsivePreampStage::prepare(const dsp::ProcessSpec& newSpec)
{
    spec = newSpec;
    lowCut.prepare(spec); highCut.prepare(spec); dcBlock.prepare(spec);
    const std::array factors { dsp::OversamplingFactor::x1, dsp::OversamplingFactor::x2,
                               dsp::OversamplingFactor::x4, dsp::OversamplingFactor::x8 };
    // Sized for 8x, which is the largest any of the four can ask for.
    oversamplerWork.assign(dsp::Oversampler::workFloatsFor(spec, 8), 0.0f);
    for (std::size_t index = 0; index < oversamplers.size(); ++index)
        oversamplers[index].prepare(spec, factors[index], &oversamplerWork);
    drive.prepare(spec.sampleRate, 8.0, dsp::SmoothingMode::logarithmic);
    trim.prepare(spec.sampleRate, 8.0, dsp::SmoothingMode::logarithmic);
    setConfig(config, 0); reset();
}
void ResponsivePreampStage::reset() noexcept
{
    lowCut.reset(); highCut.reset(); dcBlock.reset();
    for (auto& oversampler : oversamplers) oversampler.reset();
    envelope.fill(0.0f); previousEnvelope.fill(0.0f); biasMemory.fill(0.0f);
    recoveryGain.fill(1.0f); frequencyState.fill(0.0f); antialiased.reset();
    drive.reset(dsp::dbToLinear(config.driveDb)); trim.reset(dsp::dbToLinear(config.outputTrimDb));
}
void ResponsivePreampStage::setConfig(const PreampStageConfig& next, std::size_t interpolationSamples) noexcept
{
    config = next;
    config.driveDb = std::clamp(config.driveDb, -12.0f, 42.0f);
    config.bias = std::clamp(config.bias, -0.8f, 0.8f);
    config.asymmetry = std::clamp(config.asymmetry, -0.8f, 0.8f);
    config.memoryAmount = clamp01(config.memoryAmount);
    config.dynamicBias = clamp01(config.dynamicBias);
    config.frequencySaturation = clamp01(config.frequencySaturation);
    config.attackReduction = clamp01(config.attackReduction);
    config.oversamplingFactor = static_cast<int>(factorFromInt(config.oversamplingFactor));
    // Exactly the two values the saturator's second tanh can take, given the branch on the sign
    // of the biased input. Computing them here rather than per oversampled sample removes one of
    // the two transcendental calls from the hottest loop in the amplifier without changing a bit
    // of the output.
    // Derived through whichever curve the stage is actually going to use. Mixing them would
    // leave the subtraction failing to cancel, and the stage would develop a standing DC offset.
    // This now has three cases rather than two, and the failure is the same in all of them: an
    // offset computed through tanh while the stage clips with a hard clipper does not centre it,
    // it displaces it, and the DC blocker downstream then spends the attack of every note
    // recovering from a step it should never have seen.
    usesCustomShape = config.shape != dsp::Waveshape::hyperbolicTangent;
    usesAntialiasing = config.antialiasedSaturation && dsp::supportsAntiderivative(config.shape);
    const auto offsetFor = [this](float argument) noexcept
    {
        if (usesCustomShape || usesAntialiasing) return dsp::shapeSample(argument, config.shape, 1.0f);
        return config.approximateSaturation ? dsp::fastTanh(argument) : std::tanh(argument);
    };
    biasOffsetPositive = offsetFor(config.bias * (1.0f + config.asymmetry));
    biasOffsetNegative = offsetFor(config.bias * (1.0f - config.asymmetry));
    lowCut.setCoefficients(dsp::BiquadCoefficients::make(dsp::FilterType::highPass, spec.sampleRate,
        config.lowCutHz, 0.707), interpolationSamples);
    highCut.setCoefficients(dsp::BiquadCoefficients::make(dsp::FilterType::lowPass, spec.sampleRate,
        config.highCutHz, 0.707), interpolationSamples);
    dcBlock.setCoefficients(dsp::BiquadCoefficients::make(dsp::FilterType::highPass, spec.sampleRate,
        12.0, 0.707), interpolationSamples);
    drive.setTarget(dsp::dbToLinear(config.driveDb)); trim.setTarget(dsp::dbToLinear(config.outputTrimDb));
}
dsp::Oversampler& ResponsivePreampStage::selectedOversampler() noexcept
{
    if (config.oversamplingFactor >= 8) return oversamplers[3];
    if (config.oversamplingFactor >= 4) return oversamplers[2];
    if (config.oversamplingFactor >= 2) return oversamplers[1];
    return oversamplers[0];
}
void ResponsivePreampStage::process(float* const* channels, std::size_t channelCount,
                                    std::size_t samples) noexcept
{
    const auto count = std::min(channelCount, dsp::maximumChannels);
    lowCut.process(channels, count, samples); highCut.process(channels, count, samples);
    const auto envAttack = coefficient(spec.sampleRate, 1.2);
    const auto envRelease = coefficient(spec.sampleRate, 75.0);
    const auto biasCoefficient = coefficient(spec.sampleRate, 140.0 + 500.0 * config.memoryAmount);
    const auto recoveryCoefficient = coefficient(spec.sampleRate, 180.0 + 900.0 * config.memoryAmount);
    for (std::size_t sample = 0; sample < samples; ++sample)
    {
        const auto stageDrive = drive.next();
        for (std::size_t channel = 0; channel < count; ++channel)
        {
            const auto input = channels[channel][sample];
            const auto detector = std::abs(input);
            const auto envCoefficient = detector > envelope[channel] ? envAttack : envRelease;
            envelope[channel] = detector + envCoefficient * (envelope[channel] - detector);
            const auto attack = std::max(0.0f, envelope[channel] - previousEnvelope[channel]);
            previousEnvelope[channel] = envelope[channel];
            const auto biasTarget = config.dynamicBias * config.memoryAmount * envelope[channel]
                                  * (input >= 0.0f ? 0.32f : -0.18f);
            biasMemory[channel] = biasTarget + biasCoefficient * (biasMemory[channel] - biasTarget);
            const auto recoveryTarget = std::clamp(1.0f - config.memoryAmount * envelope[channel] * 0.38f,
                                                   0.55f, 1.0f);
            recoveryGain[channel] = recoveryTarget + recoveryCoefficient
                                  * (recoveryGain[channel] - recoveryTarget);
            frequencyState[channel] += 0.08f * (input - frequencyState[channel]);
            const auto high = input - frequencyState[channel];
            const auto transientGain = 1.0f / (1.0f + 22.0f * config.attackReduction * attack);
            channels[channel][sample] = ((input + config.frequencySaturation * high * 0.65f)
                * stageDrive * recoveryGain[channel] * transientGain) + biasMemory[channel];
        }
    }
    // Hoisted out of the lambda so the saturator reads immutable locals rather than chasing
    // `this` for four values on every oversampled sample.
    const auto bias = config.bias;
    const auto positivePolarity = 1.0f + config.asymmetry;
    const auto negativePolarity = 1.0f - config.asymmetry;
    const auto positiveOffset = biasOffsetPositive;
    const auto negativeOffset = biasOffsetNegative;
    // Whole loop bodies rather than a branch inside one: this runs at up to eight times the
    // sample rate for every stage, and a per-sample test on a value fixed for the block is the
    // kind of thing that eats an approximation's saving before it arrives. The two hyperbolic
    // tangent bodies at the bottom are untouched on purpose -- they are what the seven original
    // voicings run, and the regression guard in nts_amp_tests holds them to the sample.
    if (usesAntialiasing)
    {
        /* Antiderivative anti-aliasing, and one honest limitation.

           The curve this stage applies is not `shape` alone: it is `shape(x * polarity)` where
           the polarity switches on the sign of the biased input. ADAA integrates `shape`, so
           across a sample interval that straddles zero the two ends are evaluated under
           different polarities and the averaged result is slightly wrong. The error is bounded by
           the asymmetry -- it vanishes at asymmetry 0 and the voicings that enable this run 0.03
           to 0.06 -- and it is confined to the one interval per zero crossing. Splitting the
           interval at the crossing would fix it exactly and costs a branch and a root-find in the
           hottest loop in the amplifier, which is not a trade worth making for that error.
        */
        auto& shaper = antialiased;
        const auto shape = config.shape;
        selectedOversampler().processIndexed(channels, count, samples,
            [&shaper, shape, bias, positivePolarity, negativePolarity,
             positiveOffset, negativeOffset](float input, std::size_t channel) noexcept
        {
            const auto biased = input + bias;
            const auto positive = biased >= 0.0f;
            const auto polarity = positive ? positivePolarity : negativePolarity;
            return shaper.process(biased * polarity, shape, 1.0f, channel)
                 - (positive ? positiveOffset : negativeOffset);
        });
    }
    else if (usesCustomShape)
    {
        const auto shape = config.shape;
        selectedOversampler().process(channels, count, samples,
            [shape, bias, positivePolarity, negativePolarity,
             positiveOffset, negativeOffset](float input) noexcept
        {
            const auto biased = input + bias;
            const auto positive = biased >= 0.0f;
            const auto polarity = positive ? positivePolarity : negativePolarity;
            return dsp::shapeSample(biased * polarity, shape, 1.0f)
                 - (positive ? positiveOffset : negativeOffset);
        });
    }
    else if (config.approximateSaturation)
        selectedOversampler().process(channels, count, samples,
            [bias, positivePolarity, negativePolarity, positiveOffset, negativeOffset](float input) noexcept
        {
            const auto biased = input + bias;
            const auto positive = biased >= 0.0f;
            const auto polarity = positive ? positivePolarity : negativePolarity;
            return dsp::fastTanh(biased * polarity) - (positive ? positiveOffset : negativeOffset);
        });
    else
        selectedOversampler().process(channels, count, samples,
            [bias, positivePolarity, negativePolarity, positiveOffset, negativeOffset](float input) noexcept
        {
            const auto biased = input + bias;
            const auto positive = biased >= 0.0f;
            const auto polarity = positive ? positivePolarity : negativePolarity;
            return std::tanh(biased * polarity) - (positive ? positiveOffset : negativeOffset);
        });
    dcBlock.process(channels, count, samples);
    for (std::size_t sample = 0; sample < samples; ++sample)
    {
        const auto gain = trim.next();
        for (std::size_t channel = 0; channel < count; ++channel) channels[channel][sample] *= gain;
    }
}
float ResponsivePreampStage::biasState(std::size_t channel) const noexcept
{
    return channel < dsp::maximumChannels ? biasMemory[channel] : 0.0f;
}
std::size_t ResponsivePreampStage::latencySamples() const noexcept
{
    // Base-rate group delay of the anti-alias pair, which is the taps per phase.
    return config.oversamplingFactor == 1 ? 0 : dsp::antiAliasTapsPerPhase;
}

void ToneStack::prepare(const dsp::ProcessSpec& newSpec) noexcept
{
    spec = newSpec; low.prepare(spec); middle.prepare(spec); high.prepare(spec); setParameters({}); reset();
}
void ToneStack::reset() noexcept { low.reset(); middle.reset(); high.reset(); }
void ToneStack::setParameters(const ToneStackParameters& next, std::size_t interpolationSamples) noexcept
{
    parameters = next; parameters.bass = clamp01(parameters.bass); parameters.mid = clamp01(parameters.mid);
    parameters.treble = clamp01(parameters.treble);
    double lowGain {}, midGain {}, highGain {};
    double lowFrequency = 120.0, midFrequency = 750.0, highFrequency = 3200.0, midQ = 0.8;
    if (parameters.type == ToneStackType::passiveCoupled)
    {
        lowGain = (parameters.bass - 0.5) * 22.0 - (parameters.mid - 0.5) * 5.0;
        midGain = (parameters.mid - 0.5) * 17.0 - (parameters.bass - 0.5) * 4.0
                - (parameters.treble - 0.5) * 3.0 - 3.0;
        highGain = (parameters.treble - 0.5) * 21.0 - (parameters.mid - 0.5) * 4.5;
        lowFrequency = 105.0 + 55.0 * parameters.mid;
        midFrequency = 560.0 + 520.0 * parameters.treble;
        highFrequency = 2700.0 + 1300.0 * parameters.bass;
        midQ = 0.62 + 0.5 * (1.0 - parameters.bass);
    }
    else if (parameters.type == ToneStackType::activeThreeBand)
    {
        lowGain = (parameters.bass - 0.5) * 30.0;
        midGain = (parameters.mid - 0.5) * 30.0;
        highGain = (parameters.treble - 0.5) * 30.0;
    }
    else
    {
        lowGain = (parameters.bass - 0.5) * 24.0;
        midGain = (parameters.mid - 0.5) * 24.0;
        highGain = (parameters.treble - 0.5) * 22.0;
        midFrequency = std::clamp<double>(parameters.midFrequencyHz, 120.0, 2400.0);
        midQ = std::clamp<double>(parameters.midQ, 0.25, 4.0);
        lowFrequency = 90.0; highFrequency = 2800.0;
    }
    generated[0] = dsp::BiquadCoefficients::make(dsp::FilterType::lowShelf, spec.sampleRate,
                                                  lowFrequency, 0.707, lowGain);
    generated[1] = dsp::BiquadCoefficients::make(dsp::FilterType::peaking, spec.sampleRate,
                                                  midFrequency, midQ, midGain);
    generated[2] = dsp::BiquadCoefficients::make(dsp::FilterType::highShelf, spec.sampleRate,
                                                  highFrequency, 0.707, highGain);
    low.setCoefficients(generated[0], interpolationSamples);
    middle.setCoefficients(generated[1], interpolationSamples);
    high.setCoefficients(generated[2], interpolationSamples);
}
void ToneStack::process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept
{
    low.process(channels, channelCount, samples); middle.process(channels, channelCount, samples);
    high.process(channels, channelCount, samples);
}

void PhaseInverter::prepare(const dsp::ProcessSpec& spec) noexcept
{
    sampleRate = spec.sampleRate;
    lowCut.prepare(spec); shaping.prepare(spec); setParameters(parameters, 0); reset();
}
void PhaseInverter::reset() noexcept { lowCut.reset(); shaping.reset(); feedbackState.fill(0.0f); }
void PhaseInverter::setParameters(const PhaseInverterParameters& next, std::size_t interpolationSamples) noexcept
{
    parameters = next;
    parameters.drive = std::clamp(parameters.drive, 0.1f, 8.0f);
    parameters.headroom = std::clamp(parameters.headroom, 0.15f, 1.5f);
    parameters.asymmetry = std::clamp(parameters.asymmetry, -0.8f, 0.8f);
    parameters.differentialImbalance = std::clamp(parameters.differentialImbalance, -0.4f, 0.4f);
    parameters.feedback = clamp01(parameters.feedback);
    lowCut.setCoefficients(dsp::BiquadCoefficients::make(dsp::FilterType::highPass, sampleRate, 35.0), interpolationSamples);
    shaping.setCoefficients(dsp::BiquadCoefficients::make(dsp::FilterType::peaking, sampleRate, 1800.0, 0.8, 1.5), interpolationSamples);
}
void PhaseInverter::process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept
{
    const auto count = std::min(channelCount, dsp::maximumChannels);
    lowCut.process(channels, count, samples); shaping.process(channels, count, samples);
    for (std::size_t channel = 0; channel < count; ++channel)
        for (std::size_t sample = 0; sample < samples; ++sample)
        {
            const auto input = channels[channel][sample] - parameters.feedback * 0.35f * feedbackState[channel];
            const auto imbalance = channel == 0 ? 1.0f + parameters.differentialImbalance
                                                : 1.0f - parameters.differentialImbalance;
            const auto polarity = input >= 0.0f ? 1.0f + parameters.asymmetry : 1.0f - parameters.asymmetry;
            const auto output = parameters.headroom * std::tanh(input * parameters.drive * imbalance
                                                              * polarity / parameters.headroom);
            feedbackState[channel] = output; channels[channel][sample] = output;
        }
}

void PowerAmp::prepare(const dsp::ProcessSpec& newSpec) noexcept
{
    spec = newSpec; presence.prepare(spec); resonance.prepare(spec);
    master.prepare(spec.sampleRate, 15.0, dsp::SmoothingMode::logarithmic);
    setParameters(parameters, 0); reset();
}
void PowerAmp::reset() noexcept
{
    presence.reset(); resonance.reset(); supply.fill(1.0f); energy.fill(0.0f); feedbackState.fill(0.0f);
    antialiased.reset();
    master.reset(dsp::dbToLinear(parameters.masterDb));
}
void PowerAmp::setParameters(const PowerAmpParameters& next, std::size_t interpolationSamples) noexcept
{
    parameters = next;
    parameters.saturation = clamp01(parameters.saturation); parameters.damping = clamp01(parameters.damping);
    parameters.sag = clamp01(parameters.sag); parameters.presence = clamp01(parameters.presence);
    parameters.resonance = clamp01(parameters.resonance); parameters.feedback = clamp01(parameters.feedback);
    parameters.biasCharacter = std::clamp(parameters.biasCharacter, -0.6f, 0.6f);
    master.setTarget(dsp::dbToLinear(std::clamp(parameters.masterDb, -60.0f, 18.0f)));
    sagAttackCoefficient = coefficient(spec.sampleRate, parameters.sagAttackMs);
    sagRecoveryCoefficient = coefficient(spec.sampleRate, parameters.sagRecoveryMs);
    presence.setCoefficients(dsp::BiquadCoefficients::make(dsp::FilterType::highShelf, spec.sampleRate,
        2600.0, 0.707, (parameters.presence - 0.5f) * 12.0f), interpolationSamples);
    resonance.setCoefficients(dsp::BiquadCoefficients::make(dsp::FilterType::lowShelf, spec.sampleRate,
        105.0, 0.707, (parameters.resonance - 0.5f) * 14.0f), interpolationSamples);
}
void PowerAmp::process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept
{
    const auto count = std::min(channelCount, dsp::maximumChannels);
    /* One loop body, two curves, and the branch hoisted out of it.

       The valve path has to compile to exactly the code it was -- the seven original voicings run
       it and the regression guard holds them to the sample -- so it is passed as `std::tanh`
       rather than routed through the general dispatch that would also answer `hyperbolicTangent`.
       Templating the body on the curve is what lets both share the sag, feedback and bias
       arithmetic without either paying for the other's test.
    */
    const auto run = [&](auto&& curve) noexcept
    {
        for (std::size_t sample = 0; sample < samples; ++sample)
        {
            const auto masterGain = master.next();
            for (std::size_t channel = 0; channel < count; ++channel)
            {
                const auto input = channels[channel][sample] * masterGain
                                 - parameters.feedback * 0.28f * feedbackState[channel];
                const auto instantEnergy = input * input;
                energy[channel] = instantEnergy + 0.995f * (energy[channel] - instantEnergy);
                const auto targetSupply = std::clamp(1.0f - parameters.sag * std::sqrt(std::max(0.0f, energy[channel]))
                                                     * 0.72f, 0.32f, 1.0f);
                const auto supplyCoefficient = targetSupply < supply[channel]
                    ? sagAttackCoefficient : sagRecoveryCoefficient;
                supply[channel] = targetSupply + supplyCoefficient * (supply[channel] - targetSupply);
                const auto headroom = supply[channel] * (0.62f + 0.38f * parameters.damping);
                const auto drive = 1.0f + parameters.saturation * 7.0f;
                const auto biased = input + parameters.biasCharacter * (1.0f - supply[channel]) * 0.2f;
                const auto output = headroom * curve(biased * drive / std::max(0.12f, headroom), channel);
                feedbackState[channel] = output;
                channels[channel][sample] = output;
            }
        }
    };
    if (parameters.shape == dsp::Waveshape::hyperbolicTangent)
        run([](float value, std::size_t) noexcept { return std::tanh(value); });
    else
        run([this](float value, std::size_t channel) noexcept
            { return antialiased.process(value, parameters.shape, 1.0f, channel); });
    resonance.process(channels, count, samples); presence.process(channels, count, samples);
}
float PowerAmp::supplyState(std::size_t channel) const noexcept
{
    return channel < dsp::maximumChannels ? supply[channel] : 1.0f;
}

std::vector<float> makeDefaultCabinetImpulse(int slot)
{
    std::vector<float> impulse(384);
    for (std::size_t index = 0; index < impulse.size(); ++index)
    {
        const auto time = static_cast<float>(index);
        impulse[index] = slot == 0
            ? (index == 0 ? 0.72f : 0.0f) + 0.16f * std::exp(-time / 68.0f) * std::sin(0.31f * time)
            : (index == 2 ? 0.62f : 0.0f) + 0.14f * std::exp(-time / 82.0f) * std::sin(0.24f * time + 0.4f);
    }
    return impulse;
}
CabinetMetadata defaultCabinetMetadata(int slot)
{
    return slot == 0 ? CabinetMetadata { "TubeForge 4x12 Edge", "Dynamic 57", 0.25f }
                     : CabinetMetadata { "TubeForge 2x12 Center", "Ribbon 121", 0.65f };
}
void CabinetSection::restoreDefaultImpulses(std::size_t crossfadeSamples)
{
    static_cast<void>(loadImpulseA(makeDefaultCabinetImpulse(0), {}, defaultCabinetMetadata(0), crossfadeSamples));
    static_cast<void>(loadImpulseB(makeDefaultCabinetImpulse(1), {}, defaultCabinetMetadata(1), crossfadeSamples));
}
void CabinetSection::prepare(const dsp::ProcessSpec& newSpec)
{
    spec = newSpec; spec.channels = std::min(spec.channels, dsp::maximumChannels);
    first.prepare(maximumIrLength, spec.channels, spec.maximumBlockSize);
    second.prepare(maximumIrLength, spec.channels, spec.maximumBlockSize);
    lowCut.prepare(spec); highCut.prepare(spec);
    dryBuffer.assign(spec.maximumBlockSize * spec.channels, 0.0f);
    firstBuffer.assign(spec.maximumBlockSize * spec.channels, 0.0f);
    secondBuffer.assign(spec.maximumBlockSize * spec.channels, 0.0f);
    for (auto& ring : slotDelay) ring.assign((maximumAlignmentSamples + 1) * spec.channels, 0.0f);
    crossBuffer.assign(spec.maximumBlockSize * spec.channels, 0.0f);
    restoreDefaultImpulses(0);
    setParameters(parameters, 0); reset();
}
void CabinetSection::reset() noexcept
{
    first.reset(); second.reset(); lowCut.reset(); highCut.reset();
    if (bufferedPrepared) { firstBuffered.reset(); secondBuffered.reset(); }
    if (crossPrepared)
    {
        firstCross.reset(); secondCross.reset();
        if (bufferedPrepared) { firstCrossBuffered.reset(); secondCrossBuffered.reset(); }
    }
    for (auto& ring : slotDelay) std::fill(ring.begin(), ring.end(), 0.0f);
    for (auto& positions : delayPosition) positions.fill(0);
    // Both histories are clean now, so nothing is owed a clear. Engagement is re-derived from
    // the blend rather than assumed, or a reset while parked at a rail would silently re-enable
    // the idle side.
    firstNeedsHistoryReset = false; secondNeedsHistoryReset = false;
    // Width needs both responses running whatever the blend says: at blend 0 the second side would
    // otherwise be skipped and zeroed, and asking for a stereo split would produce silence on the
    // right rather than cabinet B.
    firstEngaged = slotGain[0] * std::max(1.0f - parameters.blend, parameters.width) >= disengageThreshold;
    secondEngaged = slotGain[1] * std::max(parameters.blend, parameters.width) >= disengageThreshold;
    monoSumEnergy = 0.0; channelEnergy = 0.0;
    monoLossDb.store(0.0f, std::memory_order_relaxed);
}
void CabinetSection::measureMonoCompatibility(float* const* channels, std::size_t count,
                                              std::size_t samples) noexcept
{
    /* How much level the output loses if a mix bus folds it to mono.

       This is the number the width control needs alongside it. Two different cabinet responses
       decorrelate mostly harmlessly, but `delaySamplesB` becomes an inter-channel delay once
       width is up, and an inter-channel delay is a comb filter under summing -- wide on
       speakers, hollow in mono, and the user cannot hear which until something sums it.

       Measured rather than predicted: the ratio between the energy of the actual mono sum and the
       mean channel energy. 0 dB means the channels are identical and nothing is lost. -3 dB is
       the uncorrelated case. Large negative numbers mean cancellation, which is what a comb
       filter does to the bands it nulls.

       One-pole averaged over about a second so it reads as a meter rather than flickering, and
       held rather than decayed towards a default when the signal is too quiet to measure -- the
       same reason the loudness matcher should not read silence as evidence. */
    if (count < 2) { monoLossDb.store(0.0f, std::memory_order_relaxed); return; }
    double sum {}, mean {};
    for (std::size_t sample = 0; sample < samples; ++sample)
    {
        const auto left = static_cast<double>(channels[0][sample]);
        const auto right = static_cast<double>(channels[1][sample]);
        const auto monoSum = 0.5 * (left + right);
        sum += monoSum * monoSum;
        mean += 0.5 * (left * left + right * right);
    }
    const auto blocks = std::max(1.0, spec.sampleRate / std::max(1.0, static_cast<double>(samples)));
    const auto coefficient = std::exp(-1.0 / blocks);
    monoSumEnergy = sum + coefficient * (monoSumEnergy - sum);
    channelEnergy = mean + coefficient * (channelEnergy - mean);
    if (channelEnergy <= 1.0e-9) return;
    const auto ratio = std::max(monoSumEnergy / channelEnergy, 1.0e-6);
    monoLossDb.store(static_cast<float>(std::clamp(10.0 * std::log10(ratio), -60.0, 0.0)),
                     std::memory_order_relaxed);
}

void CabinetSection::updateEngagement(bool& engaged, bool& needsHistoryReset, float weight) noexcept
{
    if (engaged) engaged = weight >= disengageThreshold;
    else if (weight > engageThreshold) { engaged = true; needsHistoryReset = true; }
}
void CabinetSection::setParameters(const CabinetParameters& next, std::size_t interpolationSamples) noexcept
{
    parameters = next; parameters.blend = clamp01(parameters.blend);
    parameters.width = clamp01(parameters.width);
    parameters.bassDiBlend = clamp01(parameters.bassDiBlend);
    for (std::size_t slot = 0; slot < parameters.slots.size(); ++slot)
    {
        auto& settings = parameters.slots[slot];
        settings.delaySamples = std::min(settings.delaySamples, maximumAlignmentSamples);
        settings.pan = std::clamp(settings.pan, -1.0f, 1.0f);
        settings.levelDb = std::clamp(settings.levelDb, -60.0f, 12.0f);
        // Folded into one linear gain here so the per-sample loop stays a multiply. A muted slot
        // is exactly zero rather than -60 dB, which is what lets the engagement test below skip
        // its convolution outright.
        slotGain[slot] = settings.mute ? 0.0f : dsp::dbToLinear(settings.levelDb);
        // Linear placement, and the law matters only at the ends: pan -1 must give exactly
        // (1, 0) and +1 exactly (0, 1), because those two are the hard split `width` performed
        // before slots could be panned, and anything else would re-voice every saved project.
        panGain[slot][0] = (1.0f - settings.pan) * 0.5f;
        panGain[slot][1] = (1.0f + settings.pan) * 0.5f;
    }
    outputTrimGain = dsp::dbToLinear(std::clamp(parameters.outputTrimDb, -24.0f, 12.0f));
    // Only the decision is made here; the history clear it can ask for happens on the audio
    // thread in process, because this may be called from a worker. Width counts as weight on both
    // sides: see the note in reset. A muted slot contributes nothing however the blend is set,
    // so its gain gates the whole test -- which is the point of having a mute at all.
    updateEngagement(firstEngaged, firstNeedsHistoryReset,
                     slotGain[0] * std::max(1.0f - parameters.blend, parameters.width));
    updateEngagement(secondEngaged, secondNeedsHistoryReset,
                     slotGain[1] * std::max(parameters.blend, parameters.width));
    lowCut.setCoefficients(dsp::BiquadCoefficients::make(dsp::FilterType::highPass, spec.sampleRate,
        parameters.lowCutHz, 0.707), interpolationSamples);
    highCut.setCoefficients(dsp::BiquadCoefficients::make(dsp::FilterType::lowPass, spec.sampleRate,
        parameters.highCutHz, 0.707), interpolationSamples);
}
void CabinetSection::selectImplementation(std::size_t crossfadeSamples)
{
    // One decision for the whole section, from the longer of the two responses. Blending sides
    // with different latencies would comb-filter, so they must share an implementation.
    const auto wantBuffered = std::max(firstLength, secondLength) > partitionedThresholdTaps;
    if (wantBuffered == usingBuffered.load(std::memory_order_relaxed)) return;

    if (wantBuffered && ! bufferedPrepared)
    {
        // Allocated on whichever thread is loading -- never the audio thread, which only ever
        // reads the flag published below.
        firstBuffered.prepare(partitionSamples, maximumIrLength, spec.channels);
        secondBuffered.prepare(partitionSamples, maximumIrLength, spec.channels);
        if (crossPrepared)
        {
            firstCrossBuffered.prepare(partitionSamples, maximumIrLength, spec.channels);
            secondCrossBuffered.prepare(partitionSamples, maximumIrLength, spec.channels);
        }
        bufferedPrepared = true;
    }

    // Both sides move together, so both have to be reloaded into the incoming pair.
    if (wantBuffered)
    {
        if (! firstLeft.empty()) { firstBuffered.loadInactiveImpulse(firstLeft, firstRight); firstBuffered.requestSwap(crossfadeSamples); }
        if (! secondLeft.empty()) { secondBuffered.loadInactiveImpulse(secondLeft, secondRight); secondBuffered.requestSwap(crossfadeSamples); }
        // The cross pair moves with its slot, never separately: a true-stereo slot whose direct
        // and cross halves ran different implementations would sum two signals a partition apart.
        if (! firstCrossLeft.empty()) { firstCrossBuffered.loadInactiveImpulse(firstCrossLeft, firstCrossRight); firstCrossBuffered.requestSwap(crossfadeSamples); }
        if (! secondCrossLeft.empty()) { secondCrossBuffered.loadInactiveImpulse(secondCrossLeft, secondCrossRight); secondCrossBuffered.requestSwap(crossfadeSamples); }
    }
    else
    {
        if (! firstLeft.empty()) { first.loadInactiveImpulse(firstLeft, firstRight); first.requestSwap(crossfadeSamples); }
        if (! secondLeft.empty()) { second.loadInactiveImpulse(secondLeft, secondRight); second.requestSwap(crossfadeSamples); }
        if (! firstCrossLeft.empty()) { firstCross.loadInactiveImpulse(firstCrossLeft, firstCrossRight); firstCross.requestSwap(crossfadeSamples); }
        if (! secondCrossLeft.empty()) { secondCross.loadInactiveImpulse(secondCrossLeft, secondCrossRight); secondCross.requestSwap(crossfadeSamples); }
    }
    usingBuffered.store(wantBuffered, std::memory_order_release);
}

void CabinetSection::prepareCrossPair()
{
    /* On first use only, like the buffered pair and for the same reason: a user who never loads a
       true-stereo response should not carry the memory of the convolvers for one. Called from a
       loading thread, never from the audio thread, which only ever reads the flags below. */
    if (crossPrepared) return;
    firstCross.prepare(maximumIrLength, spec.channels, spec.maximumBlockSize);
    secondCross.prepare(maximumIrLength, spec.channels, spec.maximumBlockSize);
    if (bufferedPrepared)
    {
        firstCrossBuffered.prepare(partitionSamples, maximumIrLength, spec.channels);
        secondCrossBuffered.prepare(partitionSamples, maximumIrLength, spec.channels);
    }
    crossPrepared = true;
}

bool CabinetSection::loadImpulseA(std::span<const float> left, std::span<const float> right,
                                  CabinetMetadata metadata, std::size_t crossfadeSamples,
                                  TrueStereoImpulse cross)
{
    const auto buffered = usingBuffered.load(std::memory_order_relaxed);
    if (buffered ? ! firstBuffered.loadInactiveImpulse(left, right)
                 : ! first.loadInactiveImpulse(left, right)) return false;
    firstMetadata = std::move(metadata); firstLength = std::max(left.size(), right.size());
    firstLeft.assign(left.begin(), left.end());
    firstRight.assign(right.begin(), right.end());
    /* The cross pair is loaded with the terms *swapped* relative to the input swap below.

       A true-stereo response is the matrix [[LL, LR], [RL, RR]]. The direct convolver holds
       (LL, RR) and sees (in_L, in_R); the cross convolver sees (in_R, in_L), so to produce the
       left output's contribution from the right input it must hold RL in its first channel and
       LR in its second. Getting that pair the wrong way round is silent -- it still sounds like a
       cabinet, just not the one that was measured -- which is why it is spelled out here. */
    if (cross.engaged())
    {
        prepareCrossPair();
        firstCrossLeft.assign(cross.rightToLeft.begin(), cross.rightToLeft.end());
        firstCrossRight.assign(cross.leftToRight.begin(), cross.leftToRight.end());
        firstLength = std::max(firstLength, std::max(firstCrossLeft.size(), firstCrossRight.size()));
        if (buffered) firstCrossBuffered.loadInactiveImpulse(firstCrossLeft, firstCrossRight);
        else firstCross.loadInactiveImpulse(firstCrossLeft, firstCrossRight);
        if (buffered) firstCrossBuffered.requestSwap(crossfadeSamples);
        else firstCross.requestSwap(crossfadeSamples);
    }
    else { firstCrossLeft.clear(); firstCrossRight.clear(); }
    // Published after the impulses are staged, so the audio thread cannot start reading a cross
    // convolver in the block before it has one.
    firstTrueStereo = cross.engaged();
    if (buffered) firstBuffered.requestSwap(crossfadeSamples); else first.requestSwap(crossfadeSamples);
    selectImplementation(crossfadeSamples);
    return true;
}
bool CabinetSection::loadImpulseB(std::span<const float> left, std::span<const float> right,
                                  CabinetMetadata metadata, std::size_t crossfadeSamples,
                                  TrueStereoImpulse cross)
{
    const auto buffered = usingBuffered.load(std::memory_order_relaxed);
    if (buffered ? ! secondBuffered.loadInactiveImpulse(left, right)
                 : ! second.loadInactiveImpulse(left, right)) return false;
    secondMetadata = std::move(metadata); secondLength = std::max(left.size(), right.size());
    secondLeft.assign(left.begin(), left.end());
    secondRight.assign(right.begin(), right.end());
    // See loadImpulseA for why the cross terms are stored the other way round.
    if (cross.engaged())
    {
        prepareCrossPair();
        secondCrossLeft.assign(cross.rightToLeft.begin(), cross.rightToLeft.end());
        secondCrossRight.assign(cross.leftToRight.begin(), cross.leftToRight.end());
        secondLength = std::max(secondLength,
                                std::max(secondCrossLeft.size(), secondCrossRight.size()));
        if (buffered) secondCrossBuffered.loadInactiveImpulse(secondCrossLeft, secondCrossRight);
        else secondCross.loadInactiveImpulse(secondCrossLeft, secondCrossRight);
        if (buffered) secondCrossBuffered.requestSwap(crossfadeSamples);
        else secondCross.requestSwap(crossfadeSamples);
    }
    else { secondCrossLeft.clear(); secondCrossRight.clear(); }
    secondTrueStereo = cross.engaged();
    if (buffered) secondBuffered.requestSwap(crossfadeSamples); else second.requestSwap(crossfadeSamples);
    selectImplementation(crossfadeSamples);
    return true;
}
std::size_t CabinetSection::latencySamples() const noexcept
{
    if (parameters.bypass) return 0;
    return usingBuffered.load(std::memory_order_acquire) ? firstBuffered.latencySamples() : 0;
}
std::size_t CabinetSection::tailSamples() const noexcept
{
    if (parameters.bypass) return 0;
    // The staged length rather than the sounding one, so a response that is mid-fade or about
    // to fade in cannot report a tail shorter than the decay it is going to produce.
    // Each slot is read through its own delay, so its decay finishes that many samples later.
    return std::max(firstLength + parameters.slots[0].delaySamples,
                    secondLength + parameters.slots[1].delaySamples);
}
void CabinetSection::process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept
{
    const auto count = std::min(channelCount, spec.channels);
    const auto processSamples = std::min(samples, spec.maximumBlockSize);
    if (parameters.bypass) return;
    // A side mid-swap, or with one staged, runs whatever the blend says: skipping it would
    // strand a response the loader has already handed over.
    const auto pathIsBuffered = usingBuffered.load(std::memory_order_acquire);
    const auto runFirst = firstEngaged
        || (pathIsBuffered ? firstBuffered.isCrossfading() || firstBuffered.isSwapPending()
                           : first.isCrossfading() || first.isSwapPending());
    const auto runSecond = secondEngaged
        || (pathIsBuffered ? secondBuffered.isCrossfading() || secondBuffered.isSwapPending()
                           : second.isCrossfading() || second.isSwapPending());
    if (runFirst && firstNeedsHistoryReset)
    { if (pathIsBuffered) firstBuffered.reset(); else first.reset(); firstNeedsHistoryReset = false; }
    if (runSecond && secondNeedsHistoryReset)
    { if (pathIsBuffered) secondBuffered.reset(); else second.reset(); secondNeedsHistoryReset = false; }

    std::array<float*, dsp::maximumChannels> firstPointers {}, secondPointers {};
    for (std::size_t channel = 0; channel < count; ++channel)
    {
        auto* dry = dryBuffer.data() + channel * spec.maximumBlockSize;
        firstPointers[channel] = firstBuffer.data() + channel * spec.maximumBlockSize;
        secondPointers[channel] = secondBuffer.data() + channel * spec.maximumBlockSize;
        std::copy_n(channels[channel], processSamples, dry);
        // A skipped side is zeroed rather than left holding the copied input: it is still read
        // by the blend below, and at the dead zone's edge the residual weight is not exactly
        // zero, so passing raw dry signal through would leak an unfiltered copy of the input.
        if (runFirst) std::copy_n(channels[channel], processSamples, firstPointers[channel]);
        else std::fill_n(firstPointers[channel], processSamples, 0.0f);
        if (runSecond) std::copy_n(channels[channel], processSamples, secondPointers[channel]);
        else std::fill_n(secondPointers[channel], processSamples, 0.0f);
    }
    // Read once, so a switch published mid-block cannot send one side down each path.
    const auto buffered = usingBuffered.load(std::memory_order_acquire);
    if (runFirst)
    {
        if (buffered) firstBuffered.process(firstPointers.data(), count, processSamples);
        else first.process(firstPointers.data(), count, processSamples);
    }
    if (runSecond)
    {
        if (buffered) secondBuffered.process(secondPointers.data(), count, processSamples);
        else second.process(secondPointers.data(), count, processSamples);
    }

    /* The cross terms of a true-stereo response, summed onto the direct ones.

       Gated on `crossPrepared` and on the slot's own flag, so a rig with no true-stereo response
       loaded -- which is almost every rig -- executes nothing here at all.

       The swap is the whole trick: the cross convolver was loaded with (RL, LR) in
       `loadImpulseA`, so feeding it (in_R, in_L) makes channel 0 produce `in_R * RL` and channel 1
       produce `in_L * LR`. Adding those to the direct pair's `in_L * LL` and `in_R * RR` is the
       true-stereo matrix product, with no new inner loop anywhere.

       Needs two channels to mean anything: a mono output has no second input to cross-feed from,
       so a true-stereo response there is used as its direct pair alone. */
    const auto crossActive = crossPrepared && count > 1;
    if (crossActive && ((runFirst && firstTrueStereo) || (runSecond && secondTrueStereo)))
    {
        std::array<float*, dsp::maximumChannels> crossPointers {};
        for (std::size_t channel = 0; channel < count; ++channel)
            crossPointers[channel] = crossBuffer.data() + channel * spec.maximumBlockSize;

        const auto runCross = [&](bool engaged, auto& direct, auto& bufferedCross,
                                  std::array<float*, dsp::maximumChannels>& target)
        {
            if (! engaged) return;
            // Swapped: channel 0 of the cross convolver reads the *right* input.
            for (std::size_t sample = 0; sample < processSamples; ++sample)
            {
                crossPointers[0][sample] = dryBuffer[spec.maximumBlockSize + sample];
                crossPointers[1][sample] = dryBuffer[sample];
            }
            for (std::size_t channel = 2; channel < count; ++channel)
                std::fill_n(crossPointers[channel], processSamples, 0.0f);
            if (buffered) bufferedCross.process(crossPointers.data(), count, processSamples);
            else direct.process(crossPointers.data(), count, processSamples);
            for (std::size_t channel = 0; channel < 2; ++channel)
                for (std::size_t sample = 0; sample < processSamples; ++sample)
                    target[channel][sample] += crossPointers[channel][sample];
        };
        runCross(runFirst && firstTrueStereo, firstCross, firstCrossBuffered, firstPointers);
        runCross(runSecond && secondTrueStereo, secondCross, secondCrossBuffered, secondPointers);
    }
    const auto delaySize = maximumAlignmentSamples + 1;
    const auto delayA = parameters.slots[0].delaySamples;
    const auto delayB = parameters.slots[1].delaySamples;
    // Folded into the level so the inner loop has one multiply per slot rather than a branch.
    // `phaseInvert` was a test on B alone before slots had settings; negating the gain is the
    // same arithmetic for both and costs nothing here.
    const auto gainA = parameters.slots[0].phaseInvert ? -slotGain[0] : slotGain[0];
    const auto gainB = parameters.slots[1].phaseInvert ? -slotGain[1] : slotGain[1];
    for (std::size_t sample = 0; sample < processSamples; ++sample)
        for (std::size_t channel = 0; channel < count; ++channel)
        {
            auto* ringA = slotDelay[0].data() + channel * delaySize;
            auto* ringB = slotDelay[1].data() + channel * delaySize;
            auto& positionA = delayPosition[0][channel];
            auto& positionB = delayPosition[1][channel];
            ringA[positionA] = firstPointers[channel][sample];
            ringB[positionB] = secondPointers[channel][sample];
            // A slot at zero delay reads back the sample just written, so an unused ring is
            // transparent rather than merely short.
            const auto firstSample = ringA[(positionA + delaySize - delayA) % delaySize] * gainA;
            const auto secondSample = ringB[(positionB + delaySize - delayB) % delaySize] * gainB;
            positionA = (positionA + 1) % delaySize;
            positionB = (positionB + 1) % delaySize;
            const auto summed = firstSample * (1.0f - parameters.blend)
                              + secondSample * parameters.blend;
            /* Width interpolates away from the sum towards where the two slots are *placed*.

               That placement used to be the fixed rule "channel 0 takes A, channel 1 takes B";
               it is now each slot's own `pan`, whose defaults are exactly that rule (see
               `CabinetParameters::slots`). A mono output has one channel and so stays on the
               sum, which is the only thing it can be. No level compensation: with two similar
               responses at the default blend of 0.5 the sum is already approximately either one
               of them, so the crossfade is close to level-matched, and with two deliberately
               different responses any correction would be guessing at which of them the user
               considers the reference. */
            /* Only two placements exist, so anything past the second channel takes the first
               one -- which is what the fixed rule did, where every channel that was not
               channel 1 received slot A. A surround bus is not a thing this stage models, and
               silently reading past the pan table would be the wrong way to find that out. */
            const auto placement = static_cast<std::size_t>(channel == 1 ? 1 : 0);
            const auto split = count > 1
                ? firstSample * panGain[0][placement] + secondSample * panGain[1][placement]
                : summed;
            const auto cabinet = summed + (split - summed) * parameters.width;
            const auto dry = dryBuffer[channel * spec.maximumBlockSize + sample];
            channels[channel][sample] = (cabinet * (1.0f - parameters.bassDiBlend)
                                         + dry * parameters.bassDiBlend) * outputTrimGain;
        }
    lowCut.process(channels, count, processSamples); highCut.process(channels, count, processSamples);
    measureMonoCompatibility(channels, count, processSamples);
}

void AmpVoice::prepare(const dsp::ProcessSpec& newSpec)
{
    spec = newSpec; spec.channels = std::min(spec.channels, dsp::maximumChannels);
    calibrator.prepare(spec); gate.prepare(spec); preEq.prepare(spec);
    for (auto& stage : stages) stage.prepare(spec);
    toneStack.prepare(spec); phaseInverter.prepare(spec); powerAmp.prepare(spec); cabinet.prepare(spec);
    bassCrossover.prepare(spec); bassCompressor.prepare(spec);
    postLow.prepare(spec); postMid.prepare(spec); postHigh.prepare(spec);
    inputTrim.prepare(spec.sampleRate, 15.0, dsp::SmoothingMode::logarithmic);
    outputGain.prepare(spec.sampleRate, 20.0, dsp::SmoothingMode::logarithmic);
    cleanBlend.prepare(spec.sampleRate, 25.0);
    dryMix.prepare(spec.sampleRate, 25.0);
    lowBuffer.assign(spec.maximumBlockSize * spec.channels, 0.0f);
    highBuffer.assign(spec.maximumBlockSize * spec.channels, 0.0f);
    dryBuffer.assign(spec.maximumBlockSize * spec.channels, 0.0f);
    dryTap.assign(spec.maximumBlockSize * spec.channels, 0.0f);
    /* Capacity for the deepest chain either delay can ever be asked to hold in step.

       Four stages is the maximum `stageCount` is clamped to and each contributes
       `antiAliasTapsPerPhase` when oversampled, plus whatever the cabinet's partitioned path
       costs. Allocated once for the worst case rather than per configuration, because
       `setParameters` runs on the audio thread and `setDelay` clamps to the prepared capacity --
       so a chain deeper than this would quietly align to the wrong figure rather than reallocate.
    */
    const auto worstLatency = stages.size() * dsp::antiAliasTapsPerPhase
                            + CabinetSection::maximumIrLength;
    dryDelay.prepare(worstLatency, spec.channels);
    lowBandDelay.prepare(worstLatency, spec.channels);
    alignedLatency = std::numeric_limits<std::size_t>::max();
    configure(makeOriginalPreset(Topology::tightModern, Instrument::guitar)); reset();
}
void AmpVoice::reset() noexcept
{
    calibrator.reset(); gate.reset(); preEq.reset(); for (auto& stage : stages) stage.reset();
    toneStack.reset(); phaseInverter.reset(); powerAmp.reset(); cabinet.reset(); bassCrossover.reset();
    bassCompressor.reset(); postLow.reset(); postMid.reset(); postHigh.reset();
    inputEnergy.fill(0.0); outputEnergy.fill(0.0); matchGain.fill(1.0f);
    dryDelay.reset(); lowBandDelay.reset();
    inputTrim.reset(dsp::dbToLinear(parameters.manualInputTrimDb));
    outputGain.reset(dsp::dbToLinear(parameters.outputGainDb)); cleanBlend.reset(parameters.bass.cleanBlend);
    dryMix.reset(clamp01(parameters.dryBlend));
}
void AmpVoice::configure(const AmpPreset& preset) noexcept
{
    calibrator.setProfile(preset.calibration); setParameters(preset.parameters); reset();
}
void AmpVoice::setParameters(const AmpParameters& next) noexcept
{
    parameters = next; parameters.stageCount = std::clamp(parameters.stageCount, std::size_t { 2 }, std::size_t { 4 });
    inputTrim.setTarget(dsp::dbToLinear(std::clamp(parameters.manualInputTrimDb, -24.0f, 24.0f)));
    outputGain.setTarget(dsp::dbToLinear(std::clamp(parameters.outputGainDb, -60.0f, 18.0f)));
    cleanBlend.setTarget(clamp01(parameters.bass.cleanBlend));
    dsp::NoiseGateParameters gateParameters;
    gateParameters.thresholdDb = parameters.gateThresholdDb;
    gateParameters.rangeDb = std::clamp(parameters.gateDepthDb, -90.0f, 0.0f);
    gateParameters.attackMs = std::clamp(static_cast<double>(parameters.gateAttackMs), 0.1, 50.0);
    gateParameters.holdMs = std::clamp(static_cast<double>(parameters.gateHoldMs), 0.0, 500.0);
    gateParameters.releaseMs = std::clamp(static_cast<double>(parameters.gateReleaseMs), 5.0, 2000.0);
    gate.setParameters(gateParameters);
    preEq.setParameters(parameters.preEq);
    for (std::size_t index = 0; index < stages.size(); ++index)
    {
        auto config = parameters.stages[index];
        if (parameters.instrument == Instrument::bass) config.driveDb += parameters.bass.highDriveDb;
        stages[index].setConfig(config);
    }
    toneStack.setParameters(parameters.toneStack); phaseInverter.setParameters(parameters.phaseInverter);
    powerAmp.setParameters(parameters.powerAmp); cabinet.setParameters(parameters.cabinet);
    dryMix.setTarget(clamp01(parameters.dryBlend));
    /* Re-derive the parallel-path alignment from what the chain now actually costs.

       After the stages are configured, because `latencySamples()` reads their oversampling
       factors -- doing it earlier aligns to the previous configuration, which is the sort of
       mistake that shows up as a voicing that sounds thin only after a preset change. Guarded on
       a change so a parameter sweep that leaves the depth alone does not keep resetting the
       delays' read positions under the signal.
    */
    if (const auto latency = latencySamples(); latency != alignedLatency)
    {
        alignedLatency = latency;
        dryDelay.setDelay(latency); lowBandDelay.setDelay(latency);
    }
    bassCrossover.setFrequency(parameters.bass.crossoverHz, 128);
    dsp::CompressorParameters compressor; compressor.thresholdDb = -28.0f;
    compressor.ratio = 1.0f + 7.0f * clamp01(parameters.bass.lowCompression);
    compressor.attackMs = 18.0; compressor.releaseMs = 150.0; bassCompressor.setParameters(compressor);
    postLow.setCoefficients(dsp::BiquadCoefficients::make(dsp::FilterType::lowShelf, spec.sampleRate,
        110.0, 0.707, parameters.postLowDb), 64);
    postMid.setCoefficients(dsp::BiquadCoefficients::make(dsp::FilterType::peaking, spec.sampleRate,
        900.0, 0.8, parameters.postMidDb), 64);
    postHigh.setCoefficients(dsp::BiquadCoefficients::make(dsp::FilterType::highShelf, spec.sampleRate,
        4200.0, 0.707, parameters.postHighDb), 64);
}
void AmpVoice::processDrivenPath(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept
{
    preEq.process(channels, channelCount, samples);
    for (std::size_t index = 0; index < parameters.stageCount; ++index)
        stages[index].process(channels, channelCount, samples);
    toneStack.process(channels, channelCount, samples); phaseInverter.process(channels, channelCount, samples);
    powerAmp.process(channels, channelCount, samples); cabinet.process(channels, channelCount, samples);
}
void AmpVoice::process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept
{
    const auto count = std::min(channelCount, spec.channels);
    const auto processSamples = std::min(samples, spec.maximumBlockSize);
    std::array<const float*, dsp::maximumChannels> readPointers {};
    for (std::size_t channel = 0; channel < count; ++channel) readPointers[channel] = channels[channel];
    calibrator.process(readPointers.data(), count, processSamples);
    std::array<float*, dsp::maximumChannels> dryPointers {};
    for (std::size_t channel = 0; channel < count; ++channel)
        dryPointers[channel] = dryTap.data() + channel * spec.maximumBlockSize;
    for (std::size_t sample = 0; sample < processSamples; ++sample)
    {
        const auto trimGain = inputTrim.next();
        for (std::size_t channel = 0; channel < count; ++channel)
        {
            channels[channel][sample] *= trimGain;
            dryBuffer[channel * spec.maximumBlockSize + sample] = channels[channel][sample];
            // A second copy, because the two are consumed differently: `dryBuffer` feeds the
            // loudness matcher, which compares energies and does not care about a few samples of
            // skew, while this one is *summed* with the wet path and cares about nothing else.
            dryPointers[channel][sample] = channels[channel][sample];
        }
    }
    // Delayed by exactly what the drive path is about to cost, so the two rejoin in phase.
    // In place: DelayLine reads after it writes, so input and output may be the same buffer.
    std::array<const float*, dsp::maximumChannels> dryReadPointers {};
    for (std::size_t channel = 0; channel < count; ++channel) dryReadPointers[channel] = dryPointers[channel];
    dryDelay.process(dryReadPointers.data(), dryPointers.data(), count, processSamples);
    // Bypassed rather than opened wide, so a disabled gate costs nothing and
    // cannot colour the signal through its sidechain filter.
    if (parameters.gateEnabled) gate.process(channels, count, processSamples);
    if (parameters.instrument == Instrument::bass)
    {
        std::array<const float*, dsp::maximumChannels> inputPointers {};
        std::array<float*, dsp::maximumChannels> lowPointers {}, highPointers {};
        for (std::size_t channel = 0; channel < count; ++channel)
        {
            inputPointers[channel] = channels[channel];
            lowPointers[channel] = lowBuffer.data() + channel * spec.maximumBlockSize;
            highPointers[channel] = highBuffer.data() + channel * spec.maximumBlockSize;
        }
        bassCrossover.process(inputPointers.data(), lowPointers.data(), highPointers.data(), count, processSamples);
        bassCompressor.process(lowPointers.data(), count, processSamples);
        if (parameters.bass.lowSaturation)
        {
            // Was `tanh(x * 1.35f)`, with both the curve and the amount written into this loop.
            // The gain is now `lowDriveDb`, whose default reproduces 1.35x, and the curve is the
            // band's own -- which is what lets a transistor bi-amp's low side stay clean and then
            // stop where a valve's rounds.
            const auto lowDrive = dsp::dbToLinear(std::clamp(parameters.bass.lowDriveDb, -12.0f, 24.0f));
            const auto lowShape = parameters.bass.lowShape;
            for (std::size_t channel = 0; channel < count; ++channel)
                for (std::size_t sample = 0; sample < processSamples; ++sample)
                    lowPointers[channel][sample] =
                        dsp::shapeSample(lowPointers[channel][sample], lowShape, lowDrive);
        }
        if (count == 2 && parameters.bass.lowMono > 0.0f)
            for (std::size_t sample = 0; sample < processSamples; ++sample)
            {
                const auto mono = 0.5f * (lowPointers[0][sample] + lowPointers[1][sample]);
                for (std::size_t channel = 0; channel < 2; ++channel)
                    lowPointers[channel][sample] += (mono - lowPointers[channel][sample]) * clamp01(parameters.bass.lowMono);
            }
        /* Hold the clean low band back by what the driven high band is about to spend.

           Only the high band goes through the preamp stages, so before this the two halves of a
           Linkwitz-Riley split rejoined 16 to 24 samples apart -- and an LR crossover sums flat
           only while its bands keep the phase relationship it was designed around. At the
           120-180 Hz the valve voicings split at that skew is about 20 degrees and passes for
           voicing; at the 500 Hz a bi-amp wants, it is 60 and audible right where the crossover
           lives. Predates the dry blend and was found while building it.
        */
        if (parameters.bass.alignBands)
        {
            std::array<const float*, dsp::maximumChannels> lowReadPointers {};
            for (std::size_t channel = 0; channel < count; ++channel) lowReadPointers[channel] = lowPointers[channel];
            lowBandDelay.process(lowReadPointers.data(), lowPointers.data(), count, processSamples);
        }
        processDrivenPath(highPointers.data(), count, processSamples);
        // Both default to 0 dB, so a preset saved before these existed recombines its two bands
        // with exactly the weights it always did. Static for the whole block by construction --
        // see the note on BassPathParameters about what a moving band level does to an LR4 sum.
        const auto lowLevel = dsp::dbToLinear(std::clamp(parameters.bass.lowLevelDb, -24.0f, 12.0f));
        const auto highLevel = dsp::dbToLinear(std::clamp(parameters.bass.highLevelDb, -24.0f, 12.0f));
        for (std::size_t sample = 0; sample < processSamples; ++sample)
        {
            const auto blend = cleanBlend.next();
            for (std::size_t channel = 0; channel < count; ++channel)
                channels[channel][sample] = lowPointers[channel][sample] * blend * lowLevel
                                          + highPointers[channel][sample] * (1.0f - 0.35f * blend) * highLevel;
        }
    }
    else
        processDrivenPath(channels, count, processSamples);
    postLow.process(channels, count, processSamples); postMid.process(channels, count, processSamples);
    postHigh.process(channels, count, processSamples);

    /* The parallel clean path rejoins here: after everything the amplifier does to the signal and
       before the output level, so the level control acts on the mix rather than on the wet half.

       Skipped entirely at zero, which is the default and every preset written before the control
       existed. Not an optimisation -- `dryMix` is smoothed, so a blend parked at zero would still
       add a multiply and a load per sample forever, and more to the point the block below must be
       provably a no-op for the regression guard on the seven original voicings to mean anything.
    */
    if (parameters.dryBlend > 0.0f)
        for (std::size_t sample = 0; sample < processSamples; ++sample)
        {
            const auto mix = dryMix.next();
            for (std::size_t channel = 0; channel < count; ++channel)
                channels[channel][sample] = channels[channel][sample] * (1.0f - mix)
                                          + dryPointers[channel][sample] * mix;
        }

    for (std::size_t sample = 0; sample < processSamples; ++sample)
    {
        const auto level = outputGain.next();
        for (std::size_t channel = 0; channel < count; ++channel)
        {
            const auto dry = dryBuffer[channel * spec.maximumBlockSize + sample];
            inputEnergy[channel] = dry * dry + 0.9995 * (inputEnergy[channel] - dry * dry);
            const auto processed = channels[channel][sample];
            outputEnergy[channel] = processed * processed + 0.9995 * (outputEnergy[channel] - processed * processed);
            if (parameters.loudnessMatch && outputEnergy[channel] > 1.0e-9)
            {
                const auto target = std::clamp(static_cast<float>(std::sqrt(
                    inputEnergy[channel] / outputEnergy[channel])), 0.25f, 4.0f);
                matchGain[channel] += 0.0002f * (target - matchGain[channel]);
            }
            else matchGain[channel] += 0.0002f * (1.0f - matchGain[channel]);
            channels[channel][sample] *= level * matchGain[channel];
        }
    }
}
std::size_t AmpVoice::latencySamples() const noexcept
{
    std::size_t latency {};
    for (std::size_t index = 0; index < parameters.stageCount; ++index) latency += stages[index].latencySamples();
    // The cabinet contributes only when a response long enough to want the partitioned path is
    // loaded. Reported rather than hidden: the host compensates for it, and the plug-in's dry
    // path is delayed by the same amount, so the bypass crossfade stays aligned.
    return latency + cabinet.latencySamples();
}

void TraditionalAmpProcessor::prepare(const dsp::ProcessSpec& newSpec)
{
    spec = newSpec; crossfader.prepare(spec, voices.size());
    for (auto& voice : voices) voice.prepare(spec);
    presets[0] = makeOriginalPreset(Topology::tightModern, Instrument::guitar);
    presets[1] = presets[0];
    requestedParameters = presets[0].parameters;
    voices[0].configure(presets[0]); voices[1].configure(presets[1]); reset();
}
void TraditionalAmpProcessor::reset() noexcept
{
    for (auto& voice : voices) voice.reset(); crossfader.reset();
}
void TraditionalAmpProcessor::setParameters(const AmpParameters& parameters) noexcept
{
    // The audio callback calls this once a block with whatever the controls currently read,
    // which is almost always exactly what they read last block. Reconfiguring regardless is
    // not free, and the cost is not only the obvious one:
    //
    //   - it re-derives around twenty-five sets of biquad coefficients per voice, each costing
    //     a sin, a cos and a pow, for values that have not moved;
    //   - and because every setCoefficients call restarts a 64-sample interpolation, it pins
    //     every filter in the amplifier to its per-sample interpolating path. The settled path,
    //     which holds state and coefficients in registers, could otherwise never run at all.
    //
    // Comparing first costs one struct compare of plain values. The dirty flag covers the
    // paths that reconfigure a voice without going through here; see its declaration.
    if (! parametersDirty && ! transitionPending && parameters == requestedParameters) return;

    // A change of oversampling factor swaps in a different polyphase filter whose
    // delay lines hold unrelated state, so it has to cross-fade like any other
    // discrete change rather than switching under the signal.
    auto oversamplingChanged = false;
    for (std::size_t index = 0; index < parameters.stages.size(); ++index)
        oversamplingChanged = oversamplingChanged
            || parameters.stages[index].oversamplingFactor
                   != requestedParameters.stages[index].oversamplingFactor;
    const auto discreteChanged = parameters.instrument != requestedParameters.instrument
                              || parameters.topology != requestedParameters.topology
                              || oversamplingChanged;
    requestedParameters = parameters;
    if (discreteChanged)
    {
        transitionTarget = 1 - crossfader.mode();
        presets[transitionTarget].parameters = parameters;
        voices[transitionTarget].setParameters(parameters);
        voices[transitionTarget].reset();
        crossfader.requestMode(transitionTarget, 1024);
        transitionPending = true;
        return;
    }
    if (transitionPending)
    {
        presets[transitionTarget].parameters = parameters;
        voices[transitionTarget].setParameters(parameters);
        if (crossfader.mode() == transitionTarget && ! crossfader.isCrossfading())
        {
            transitionPending = false;
            // Only the target voice has been tracking the controls through the transition, so
            // the other one is now stale. Force the next call to sync both.
            parametersDirty = true;
        }
        return;
    }
    for (auto& voice : voices) voice.setParameters(parameters);
    for (auto& preset : presets) preset.parameters = parameters;
    parametersDirty = false;
}
void TraditionalAmpProcessor::setParametersImmediately(const AmpParameters& parameters) noexcept
{
    requestedParameters = parameters;
    for (std::size_t index = 0; index < voices.size(); ++index)
    {
        presets[index].parameters = parameters;
        voices[index].setParameters(parameters);
        voices[index].reset();
    }
    crossfader.reset();
    transitionTarget = 0;
    transitionPending = false;
    parametersDirty = true;
}
void TraditionalAmpProcessor::loadPreset(const AmpPreset& preset, std::size_t crossfadeSamples)
{
    const auto inactive = 1 - crossfader.mode();
    presets[inactive] = preset;
    voices[inactive].configure(preset);
    crossfader.requestMode(inactive, crossfadeSamples);
    requestedParameters = preset.parameters;
    transitionTarget = inactive;
    transitionPending = true;
    parametersDirty = true;
}
void TraditionalAmpProcessor::process(float* const* channels, std::size_t channelCount,
                                      std::size_t samples) noexcept
{
    crossfader.process(channels, channelCount, samples,
        [this](std::size_t voice, float* const* buffers, std::size_t count, std::size_t blockSamples) noexcept
        {
            voices[voice].process(buffers, count, blockSamples);
        });
}
CalibrationReading TraditionalAmpProcessor::calibrationReading() const noexcept
{
    return voices[crossfader.mode()].calibrationReading();
}
std::size_t TraditionalAmpProcessor::latencySamples() const noexcept
{
    return voices[crossfader.mode()].latencySamples();
}
std::size_t TraditionalAmpProcessor::tailSamples() const noexcept
{
    return std::max(voices[0].tailSamples(), voices[1].tailSamples());
}
bool AmpVoice::loadCabinetImpulse(int slot, std::span<const float> left, std::span<const float> right,
                                  CabinetMetadata metadata, std::size_t crossfadeSamples)
{
    return slot == 0 ? cabinet.loadImpulseA(left, right, std::move(metadata), crossfadeSamples)
                     : cabinet.loadImpulseB(left, right, std::move(metadata), crossfadeSamples);
}
bool TraditionalAmpProcessor::loadCabinetImpulse(int slot, std::span<const float> left,
                                                 std::span<const float> right, CabinetMetadata metadata,
                                                 std::size_t crossfadeSamples)
{
    // Both voices: they alternate across preset changes, so loading into the sounding one
    // alone would put the previous cabinet back the next time a preset was recalled.
    auto loaded = true;
    for (auto& voice : voices)
        loaded = voice.loadCabinetImpulse(slot, left, right, metadata, crossfadeSamples) && loaded;
    return loaded;
}
void TraditionalAmpProcessor::restoreDefaultCabinet(std::size_t crossfadeSamples)
{
    for (auto& voice : voices) voice.restoreDefaultCabinet(crossfadeSamples);
}

std::vector<float> renderOffline(TraditionalAmpProcessor& processor, std::span<const float> monoInput,
                                 std::size_t blockSize)
{
    blockSize = std::max<std::size_t>(1, blockSize);
    std::vector<float> result(monoInput.size());
    std::vector<float> block(blockSize);
    for (std::size_t offset = 0; offset < monoInput.size(); offset += blockSize)
    {
        const auto count = std::min(blockSize, monoInput.size() - offset);
        std::fill(block.begin(), block.end(), 0.0f);
        std::copy_n(monoInput.data() + offset, count, block.data());
        float* channel[] { block.data() };
        processor.process(channel, 1, count);
        std::copy_n(block.data(), count, result.data() + offset);
    }
    return result;
}
} // namespace nts::amp


