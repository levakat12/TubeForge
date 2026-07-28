#include <nts/circuit/Components.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace nts::circuit
{
namespace
{
constexpr std::array<TubeDefinition, 5> tubes {{
    { "tube.12ax7.v1", "12AX7", 100.0f, 62500.0f, 0.0016f, 250.0f, 330.0f, 1500.0f, 100000.0f },
    { "tube.12at7.v1", "12AT7", 60.0f, 10900.0f, 0.0055f, 250.0f, 300.0f, 820.0f, 47000.0f },
    { "tube.12au7.v1", "12AU7", 17.0f, 7700.0f, 0.0022f, 250.0f, 300.0f, 820.0f, 22000.0f },
    { "tube.6v6.v1", "6V6", 9.8f, 50000.0f, 0.00375f, 285.0f, 350.0f, 470.0f, 5000.0f },
    { "tube.el34.v1", "EL34", 11.0f, 15000.0f, 0.011f, 400.0f, 800.0f, 470.0f, 3400.0f }
}};

float clamp01(float value) noexcept { return std::clamp(value, 0.0f, 1.0f); }

class MeteredComponent : public ICircuitComponent
{
public:
    ComponentTelemetry telemetry() const noexcept override
    {
        return { inputRms.load(std::memory_order_relaxed), outputRms.load(std::memory_order_relaxed),
                 plateCurrent.load(std::memory_order_relaxed), supply.load(std::memory_order_relaxed),
                 headroom.load(std::memory_order_relaxed), balance.load(std::memory_order_relaxed),
                 sag.load(std::memory_order_relaxed), operatingPoint.load(std::memory_order_relaxed),
                 stageGain.load(std::memory_order_relaxed), clipping.load(std::memory_order_relaxed),
                 harmonic.load(std::memory_order_relaxed), valid.load(std::memory_order_relaxed) };
    }

protected:
    void beginMeter(std::span<const float> samples) noexcept
    {
        float energy = 0.0f;
        for (const auto sample : samples) energy += sample * sample;
        inputRms.store(samples.empty() ? 0.0f : std::sqrt(energy / static_cast<float>(samples.size())),
                       std::memory_order_relaxed);
    }
    void endMeter(std::span<const float> samples) noexcept
    {
        float energy = 0.0f;
        for (const auto sample : samples) energy += sample * sample;
        const auto rms = samples.empty() ? 0.0f : std::sqrt(energy / static_cast<float>(samples.size()));
        outputRms.store(rms, std::memory_order_relaxed);
        const auto in = inputRms.load(std::memory_order_relaxed);
        stageGain.store(20.0f * std::log10(std::max(1.0e-6f, rms) / std::max(1.0e-6f, in)),
                        std::memory_order_relaxed);
        headroom.store(20.0f * std::log10(std::max(1.0e-6f, 1.0f / std::max(rms, 1.0e-6f))),
                       std::memory_order_relaxed);
    }
    std::atomic<float> inputRms {}, outputRms {}, plateCurrent {}, supply { 300.0f };
    std::atomic<float> headroom { 60.0f }, balance {}, sag {};
    std::atomic<float> operatingPoint { 0.5f }, stageGain {}, clipping {}, harmonic {};
    std::atomic<bool> valid { true };
};

class GainComponent final : public MeteredComponent
{
public:
    explicit GainComponent(const NodeSpec& node) : gain(parameterValue(node, "gain", 1.0f)) {}
    void prepare(const nts::dsp::ProcessSpec&) override {}
    void reset() noexcept override {}
    void process(std::span<float> samples) noexcept override
    {
        beginMeter(samples);
        for (auto& sample : samples) sample *= gain;
        endMeter(samples);
    }
private:
    float gain { 1.0f };
};

class FilterComponent final : public MeteredComponent
{
public:
    explicit FilterComponent(const NodeSpec& node)
        : cutoff(parameterValue(node, "cutoff-hz", 80.0f)), highPass(parameterValue(node, "high-pass", 1.0f) >= 0.5f) {}
    void prepare(const nts::dsp::ProcessSpec& newSpec) override
    {
        sampleRate = newSpec.sampleRate;
        update();
    }
    void reset() noexcept override { state = 0.0f; previousInput = 0.0f; }
    void process(std::span<float> samples) noexcept override
    {
        beginMeter(samples);
        for (auto& x : samples)
        {
            if (highPass)
            {
                const auto y = coefficient * (state + x - previousInput);
                previousInput = x; state = y; x = y;
            }
            else { state += coefficient * (x - state); x = state; }
        }
        endMeter(samples);
    }
    std::size_t stateSize() const noexcept override { return 2; }
private:
    void update() noexcept
    {
        const auto fc = std::clamp(cutoff, 5.0f, static_cast<float>(sampleRate * 0.45));
        coefficient = highPass ? std::exp(-2.0f * std::numbers::pi_v<float> * fc / static_cast<float>(sampleRate))
                               : 1.0f - std::exp(-2.0f * std::numbers::pi_v<float> * fc / static_cast<float>(sampleRate));
    }
    float cutoff {}, coefficient {}, state {}, previousInput {};
    double sampleRate { 48000.0 };
    bool highPass {};
};

class TriodeComponent final : public MeteredComponent
{
public:
    TriodeComponent(const NodeSpec& node, std::unique_ptr<INonlinearComponentModel> model)
        : tube(TubeLibrary::find(node.modelId)), backend(node.backend), hybrid(std::move(model)),
          drive(std::clamp(parameterValue(node, "drive", 0.5f), 0.0f, 1.0f)),
          bias(std::clamp(parameterValue(node, "bias", 0.5f), 0.0f, 1.0f)),
          plateVoltage(parameterValue(node, "plate-voltage-v", tube != nullptr ? tube->nominalPlateVoltage : 250.0f)),
          plateResistance(parameterValue(node, "plate-resistance-ohm", tube != nullptr ? tube->nominalPlateResistanceOhms : 100000.0f)),
          cathodeResistance(parameterValue(node, "cathode-resistance-ohm", tube != nullptr ? tube->nominalCathodeResistanceOhms : 1500.0f)),
          cathodeBypass(parameterValue(node, "cathode-bypass-f", 1.0e-6f)),
          gridStopper(parameterValue(node, "grid-stopper-ohm", 33000.0f)),
          couplingCap(parameterValue(node, "coupling-cap-f", 22.0e-9f)),
          inputAttenuation(parameterValue(node, "input-attenuation", 1.0f)),
          outputTrim(parameterValue(node, "output-trim", 0.82f)),
          aging(parameterValue(node, "aging", 0.0f))
    {
        valid.store(tube != nullptr && (backend != ModelBackend::neuralSurrogate || hybrid != nullptr));
    }
    void prepare(const nts::dsp::ProcessSpec& spec) override
    {
        sampleRate = spec.sampleRate;
        const auto couplingCorner = 1.0f / (2.0f * std::numbers::pi_v<float> * std::max(10000.0f, plateResistance) * couplingCap);
        hpCoefficient = std::exp(-2.0f * std::numbers::pi_v<float> * std::clamp(couplingCorner, 2.0f, 220.0f) / static_cast<float>(sampleRate));
        const auto cathodeCorner = 1.0f / (2.0f * std::numbers::pi_v<float> * cathodeResistance * cathodeBypass);
        cathodeCoefficient = 1.0f - std::exp(-2.0f * std::numbers::pi_v<float> * std::clamp(cathodeCorner, 1.0f, 5000.0f) / static_cast<float>(sampleRate));
        if (hybrid) hybrid->prepare(spec.sampleRate, spec.maximumBlockSize);
    }
    void reset() noexcept override
    {
        previousInput = highPassState = cathodeState = 0.0f;
        if (hybrid) hybrid->reset();
    }
    void process(std::span<float> samples) noexcept override
    {
        beginMeter(samples);
        if (!valid.load(std::memory_order_relaxed))
        {
            std::fill(samples.begin(), samples.end(), 0.0f); endMeter(samples); return;
        }
        if (hybrid)
        {
            const std::array<float, 4> parameters { drive, bias, plateVoltage / tube->maximumPlateVoltage,
                                                   cathodeResistance / 3000.0f };
            hybrid->setPhysicalParameters(parameters);
            hybrid->process(samples, samples);
        }
        else
        {
            const auto mu = tube->amplificationFactor / 100.0f;
            const auto gm = tube->transconductanceSiemens / 0.0016f;
            const auto gridLoss = 1.0f / (1.0f + gridStopper / 220000.0f);
            const auto agedGain = 1.0f - aging * 0.28f;
            const auto stageDrive = inputAttenuation * gridLoss * agedGain
                * (0.7f + drive * 7.0f * (0.55f * mu + 0.45f * std::sqrt(std::max(gm, 0.01f))));
            const auto asymmetry = (bias - 0.5f) * 0.55f;
            auto clippingEstimate = 0.0f;
            for (auto& x : samples)
            {
                const auto hp = hpCoefficient * (highPassState + x - previousInput);
                previousInput = x; highPassState = hp;
                cathodeState += cathodeCoefficient * (std::abs(hp) - cathodeState);
                auto v = hp * stageDrive - asymmetry - 0.18f * cathodeState;
                float y {};
                if (backend == ModelBackend::staticTransfer) y = std::tanh(v) + 0.08f * std::tanh(v * v);
                else if (backend == ModelBackend::graybox)
                    y = std::tanh(v / (1.0f + 0.12f * std::abs(v))) + 0.06f * std::tanh(v * v);
                else
                {
                    y = std::clamp(v, -4.0f, 4.0f);
                    const auto alpha = 0.16f + 0.12f * drive;
                    for (int iteration = 0; iteration < 3; ++iteration)
                        y -= (y + alpha * std::sinh(y) - v) / (1.0f + alpha * std::cosh(y));
                }
                const auto unclamped = (y - std::tanh(-asymmetry)) * outputTrim;
                clippingEstimate = std::max(clippingEstimate, std::max(0.0f, std::abs(unclamped) - 1.0f));
                x = std::clamp(unclamped, -2.0f, 2.0f);
            }
            clipping.store(clippingEstimate, std::memory_order_relaxed);
        }
        const auto rms = inputRms.load(std::memory_order_relaxed);
        const auto current = std::clamp((0.25f + 3.5f * rms * drive) * tube->transconductanceSiemens / 0.0016f,
                                        0.0f, 18.0f);
        plateCurrent.store(current, std::memory_order_relaxed);
        supply.store(plateVoltage, std::memory_order_relaxed);
        balance.store((bias - 0.5f) * 2.0f, std::memory_order_relaxed);
        operatingPoint.store(std::clamp((plateVoltage / tube->maximumPlateVoltage) * (1.15f - bias * 0.3f), 0.0f, 1.0f), std::memory_order_relaxed);
        harmonic.store(std::clamp(drive * 0.55f + std::abs(bias - 0.5f) * 0.7f + aging * 0.2f, 0.0f, 1.0f), std::memory_order_relaxed);
        valid.store(plateVoltage > 20.0f && plateVoltage <= tube->maximumPlateVoltage
                    && plateResistance >= 10000.0f && plateResistance <= 1000000.0f
                    && cathodeResistance >= 100.0f && cathodeResistance <= 10000.0f,
                    std::memory_order_relaxed);
        endMeter(samples);
    }
    std::size_t latencySamples() const noexcept override { return hybrid ? hybrid->latencySamples() : 0; }
    std::size_t stateSize() const noexcept override { return 3; }
private:
    const TubeDefinition* tube {};
    ModelBackend backend {};
    std::unique_ptr<INonlinearComponentModel> hybrid;
    float drive {}, bias {}, plateVoltage {}, plateResistance {}, cathodeResistance {}, cathodeBypass {};
    float gridStopper {}, couplingCap {}, inputAttenuation {}, outputTrim {}, aging {};
    float hpCoefficient {}, cathodeCoefficient {}, previousInput {}, highPassState {}, cathodeState {};
    double sampleRate { 48000.0 };
};

class ToneStackComponent final : public MeteredComponent
{
public:
    explicit ToneStackComponent(const NodeSpec& node)
        : bass(clamp01(parameterValue(node, "bass", 0.5f))), middle(clamp01(parameterValue(node, "middle", 0.5f))),
          treble(clamp01(parameterValue(node, "treble", 0.5f))),
          slopeResistance(parameterValue(node, "slope-resistance-ohm", 100000.0f)),
          midResistance(parameterValue(node, "mid-resistance-ohm", 25000.0f)),
          bassPot(parameterValue(node, "bass-pot-ohm", 1000000.0f)),
          midPot(parameterValue(node, "mid-pot-ohm", 25000.0f)),
          treblePot(parameterValue(node, "treble-pot-ohm", 250000.0f)),
          bassCap(parameterValue(node, "bass-cap-f", 22.0e-9f)),
          midCap(parameterValue(node, "mid-cap-f", 22.0e-9f)),
          trebleCap(parameterValue(node, "treble-cap-f", 250.0e-12f)) {}
    void prepare(const nts::dsp::ProcessSpec& spec) override
    {
        currentSpec = spec; currentSpec.channels = 1;
        low.prepare(currentSpec); mid.prepare(currentSpec); high.prepare(currentSpec); update();
        valid.store(slopeResistance >= 10000.0f && slopeResistance <= 1000000.0f && midResistance >= 1000.0f
                    && bassCap > 0.0f && midCap > 0.0f && trebleCap > 0.0f);
    }
    void reset() noexcept override { low.reset(); mid.reset(); high.reset(); }
    void process(std::span<float> samples) noexcept override
    {
        beginMeter(samples); float* channel = samples.data();
        low.process(&channel, 1, samples.size()); mid.process(&channel, 1, samples.size());
        high.process(&channel, 1, samples.size());
        for (auto& sample : samples) sample *= 0.72f;
        endMeter(samples);
    }
    std::size_t stateSize() const noexcept override { return 6; }
private:
    void update() noexcept
    {
        const auto bassCorner = 1.0 / (2.0 * std::numbers::pi * slopeResistance * bassCap);
        const auto midCorner = 1.0 / (2.0 * std::numbers::pi * (slopeResistance + midResistance) * midCap);
        const auto trebleCorner = 1.0 / (2.0 * std::numbers::pi * slopeResistance * trebleCap);
        const auto bassRange = std::clamp(bassPot / 1000000.0f, 0.2f, 2.0f);
        const auto midRange = std::clamp(midPot / 25000.0f, 0.2f, 4.0f);
        const auto trebleRange = std::clamp(treblePot / 250000.0f, 0.2f, 4.0f);
        low.setCoefficients(nts::dsp::BiquadCoefficients::make(nts::dsp::FilterType::lowShelf, currentSpec.sampleRate,
                                                               std::clamp(bassCorner, 40.0, 250.0), 0.707, (bass - 0.5f) * 18.0f * bassRange));
        mid.setCoefficients(nts::dsp::BiquadCoefficients::make(nts::dsp::FilterType::peaking, currentSpec.sampleRate,
                                                               std::clamp(midCorner * 1.8, 250.0, 1800.0), 0.7, (middle - 0.5f) * 16.0f * midRange));
        high.setCoefficients(nts::dsp::BiquadCoefficients::make(nts::dsp::FilterType::highShelf, currentSpec.sampleRate,
                                                                std::clamp(trebleCorner * 0.018, 1800.0, 7000.0), 0.707, (treble - 0.5f) * 18.0f * trebleRange));
    }
    float bass {}, middle {}, treble {}, slopeResistance {}, midResistance {}, bassPot {}, midPot {}, treblePot {};
    float bassCap {}, midCap {}, trebleCap {};
    nts::dsp::ProcessSpec currentSpec;
    nts::dsp::Biquad low, mid, high;
};

class PhaseInverterComponent final : public MeteredComponent
{
public:
    explicit PhaseInverterComponent(const NodeSpec& node)
        : drive(clamp01(parameterValue(node, "drive", 0.45f))), imbalance(parameterValue(node, "imbalance", 0.04f)) {}
    void prepare(const nts::dsp::ProcessSpec&) override {}
    void reset() noexcept override { delayed = 0.0f; }
    void process(std::span<float> samples) noexcept override
    {
        beginMeter(samples);
        for (auto& x : samples)
        {
            const auto positive = std::tanh(x * (1.5f + 5.0f * drive) - delayed * 0.12f);
            const auto negative = std::tanh(-x * (1.5f + 5.0f * drive) * (1.0f + imbalance));
            x = (positive - negative) * 0.47f; delayed = x;
        }
        balance.store(imbalance, std::memory_order_relaxed); endMeter(samples);
    }
    std::size_t stateSize() const noexcept override { return 1; }
private:
    float drive {}, imbalance {}, delayed {};
};

class PowerComponent final : public MeteredComponent
{
public:
    explicit PowerComponent(const NodeSpec& node)
        : drive(clamp01(parameterValue(node, "drive", 0.5f))), sagAmount(clamp01(parameterValue(node, "sag", 0.3f))),
          feedbackAmount(std::clamp(parameterValue(node, "feedback", 0.25f), 0.0f, 0.92f)),
          supplyNominal(std::clamp(parameterValue(node, "supply-voltage-v", 420.0f), 50.0f, 800.0f)),
          topology(std::clamp(static_cast<int>(std::lround(parameterValue(node, "topology", 2.0f))), 0, 2)),
          tubeCount(std::clamp(parameterValue(node, "tube-count", 2.0f), 1.0f, 8.0f)),
          biasPoint(clamp01(parameterValue(node, "bias", 0.55f))),
          loadOhms(parameterValue(node, "load-ohm", 3400.0f)),
          damping(clamp01(parameterValue(node, "damping", 0.5f))),
          saturationAmount(clamp01(parameterValue(node, "saturation", 0.5f))),
          attackMs(std::clamp(parameterValue(node, "supply-attack-ms", 8.0f), 0.5f, 1000.0f)),
          recoveryMs(std::clamp(parameterValue(node, "supply-recovery-ms", 85.0f), 1.0f, 5000.0f)),
          rectifierStiffness(clamp01(parameterValue(node, "rectifier-stiffness", 0.6f))),
          rippleAmount(clamp01(parameterValue(node, "ripple", 0.0f)))
    {
        valid.store(loadOhms >= 500.0f && loadOhms <= 20000.0f);
    }
    void prepare(const nts::dsp::ProcessSpec& spec) override
    {
        attack = 1.0f - std::exp(-1.0f / static_cast<float>(spec.sampleRate * attackMs * 0.001f));
        release = 1.0f - std::exp(-1.0f / static_cast<float>(spec.sampleRate * recoveryMs * 0.001f));
        ripplePhaseIncrement = 2.0f * std::numbers::pi_v<float> * 100.0f / static_cast<float>(spec.sampleRate);
    }
    void reset() noexcept override { envelope = delayed = ripplePhase = 0.0f; }
    void process(std::span<float> samples) noexcept override
    {
        beginMeter(samples);
        for (auto& x : samples)
        {
            const auto target = std::abs(x);
            envelope += (target > envelope ? attack : release) * (target - envelope);
            const auto sagNow = std::clamp(envelope * sagAmount * (0.48f - rectifierStiffness * 0.28f), 0.0f, 0.48f);
            ripplePhase += ripplePhaseIncrement;
            if (ripplePhase > 2.0f * std::numbers::pi_v<float>) ripplePhase -= 2.0f * std::numbers::pi_v<float>;
            const auto ripple = std::sin(ripplePhase) * rippleAmount * 0.018f;
            const auto topologyGain = topology == 0 ? 0.82f : topology == 1 ? 0.95f : 1.0f;
            const auto v = (x - delayed * feedbackAmount * (0.6f + damping * 0.4f))
                         * (1.2f + drive * 8.0f) * topologyGain * (1.0f - sagNow + ripple);
            auto y = std::tanh(v);
            if (topology == 2 && std::abs(v) < 0.07f + (1.0f - biasPoint) * 0.06f)
                y *= 0.72f + biasPoint * 0.16f + 3.1f * std::abs(v);
            x = std::tanh(y * (1.0f + saturationAmount * 0.45f)) * 0.86f; delayed = x;
        }
        const auto sagNow = std::clamp(envelope * sagAmount * 0.38f, 0.0f, 0.48f);
        sag.store(sagNow * 100.0f, std::memory_order_relaxed);
        supply.store(supplyNominal * (1.0f - sagNow), std::memory_order_relaxed);
        plateCurrent.store(envelope * tubeCount * (4.0f + 21.0f * drive), std::memory_order_relaxed);
        operatingPoint.store(biasPoint, std::memory_order_relaxed);
        harmonic.store(std::clamp(saturationAmount * 0.55f + (topology == 2 ? 0.2f : 0.08f), 0.0f, 1.0f), std::memory_order_relaxed);
        endMeter(samples);
    }
    std::size_t stateSize() const noexcept override { return 3; }
private:
    float drive {}, sagAmount {}, feedbackAmount {}, supplyNominal {};
    int topology {};
    float tubeCount {}, biasPoint {}, loadOhms {}, damping {}, saturationAmount {};
    float attackMs {}, recoveryMs {}, rectifierStiffness {}, rippleAmount {};
    float attack {}, release {}, envelope {}, delayed {}, ripplePhase {}, ripplePhaseIncrement {};
};

class FeedbackComponent final : public MeteredComponent
{
public:
    explicit FeedbackComponent(const NodeSpec& node)
        : amount(std::clamp(parameterValue(node, "amount", 0.25f), 0.0f, 0.92f)),
          presence(clamp01(parameterValue(node, "presence", 0.5f))) {}
    void prepare(const nts::dsp::ProcessSpec& spec) override
    {
        coefficient = 1.0f - std::exp(-2.0f * std::numbers::pi_v<float> * (900.0f + presence * 5000.0f)
                                      / static_cast<float>(spec.sampleRate));
    }
    void reset() noexcept override { lowState = delayed = 0.0f; }
    void process(std::span<float> samples) noexcept override
    {
        beginMeter(samples);
        for (auto& x : samples)
        {
            lowState += coefficient * (delayed - lowState);
            const auto high = delayed - lowState;
            x = std::clamp(x - amount * (lowState + high * (0.2f + presence)), -2.0f, 2.0f);
            delayed = x;
        }
        endMeter(samples);
    }
    std::size_t stateSize() const noexcept override { return 2; }
private:
    float amount {}, presence {}, coefficient {}, lowState {}, delayed {};
};

class TransformerComponent final : public MeteredComponent
{
public:
    explicit TransformerComponent(const NodeSpec& node)
        : saturation(clamp01(parameterValue(node, "saturation", 0.3f))), ratio(parameterValue(node, "turns-ratio", 20.0f)),
          leakage(clamp01(parameterValue(node, "leakage", 0.25f))), damping(clamp01(parameterValue(node, "damping", 0.5f))) {}
    void prepare(const nts::dsp::ProcessSpec& spec) override
    {
        highPass = std::exp(-2.0f * std::numbers::pi_v<float> * 28.0f / static_cast<float>(spec.sampleRate));
        lowPass = 1.0f - std::exp(-2.0f * std::numbers::pi_v<float> * (17000.0f - leakage * 7000.0f) / static_cast<float>(spec.sampleRate));
        valid.store(ratio >= 2.0f && ratio <= 100.0f);
    }
    void reset() noexcept override { hpState = previous = lpState = flux = 0.0f; }
    void process(std::span<float> samples) noexcept override
    {
        beginMeter(samples);
        for (auto& x : samples)
        {
            const auto hp = highPass * (hpState + x - previous); previous = x; hpState = hp;
            flux = 0.997f * flux + 0.003f * hp;
            const auto saturated = std::tanh((hp + flux * 0.4f) * (1.0f + saturation * 2.5f))
                                   / (1.0f + saturation * 0.55f);
            lpState += lowPass * (saturated - lpState); x = lpState * (0.82f + damping * 0.18f);
        }
        endMeter(samples);
    }
    std::size_t stateSize() const noexcept override { return 4; }
private:
    float saturation {}, ratio {}, leakage {}, damping {}, highPass {}, lowPass {}, hpState {}, previous {}, lpState {}, flux {};
};

class CabinetComponent final : public MeteredComponent
{
public:
    explicit CabinetComponent(const NodeSpec& node)
        : resonance(clamp01(parameterValue(node, "resonance", 0.5f))), brightness(clamp01(parameterValue(node, "brightness", 0.5f))) {}
    void prepare(const nts::dsp::ProcessSpec& spec) override
    {
        hp = std::exp(-2.0f * std::numbers::pi_v<float> * (55.0f + 45.0f * (1.0f - resonance)) / static_cast<float>(spec.sampleRate));
        lp = 1.0f - std::exp(-2.0f * std::numbers::pi_v<float> * (3500.0f + 4500.0f * brightness) / static_cast<float>(spec.sampleRate));
    }
    void reset() noexcept override { hpState = previous = lpState = resonanceState = 0.0f; }
    void process(std::span<float> samples) noexcept override
    {
        beginMeter(samples);
        for (auto& x : samples)
        {
            const auto highPassed = hp * (hpState + x - previous); previous = x; hpState = highPassed;
            resonanceState += 0.035f * (highPassed - resonanceState);
            const auto voiced = highPassed + resonanceState * resonance * 0.28f;
            lpState += lp * (voiced - lpState); x = lpState;
        }
        endMeter(samples);
    }
    std::size_t stateSize() const noexcept override { return 4; }
private:
    float resonance {}, brightness {}, hp {}, lp {}, hpState {}, previous {}, lpState {}, resonanceState {};
};
} // namespace

bool PackedNeuralComponentModel::load(std::span<const std::byte> bytes, std::string& error)
{
    if (!model.load(bytes, error)) return false;
    if (model.controlCount() > physicalControls.size())
    {
        error = "Neural component exposes more than 32 physical controls";
        return false;
    }
    return true;
}

void PackedNeuralComponentModel::prepare(double sampleRate, std::size_t)
{
    sampleRateMatches = model.isLoaded() && model.sampleRate() == static_cast<int>(std::lround(sampleRate));
}

void PackedNeuralComponentModel::reset() noexcept { model.reset(); }

void PackedNeuralComponentModel::setPhysicalParameters(std::span<const float> values) noexcept
{
    if (model.controlCount() == 0) return;
    std::fill(physicalControls.begin(), physicalControls.end(), 0.0f);
    std::copy_n(values.begin(), std::min(values.size(), model.controlCount()), physicalControls.begin());
    (void) model.setControls(std::span<const float>(physicalControls).first(model.controlCount()));
}

void PackedNeuralComponentModel::process(std::span<const float> input, std::span<float> output) noexcept
{
    if (!sampleRateMatches || !model.process(input, output)) std::fill(output.begin(), output.end(), 0.0f);
}

std::span<const TubeDefinition> TubeLibrary::definitions() noexcept { return tubes; }

const TubeDefinition* TubeLibrary::find(std::string_view stableId) noexcept
{
    const auto found = std::find_if(tubes.begin(), tubes.end(), [stableId](const auto& tube) { return tube.id == stableId; });
    return found == tubes.end() ? nullptr : &*found;
}

std::vector<ParameterDescriptor> parameterSchema(NodeType type)
{
    switch (type)
    {
        case NodeType::input: case NodeType::output: return { { "gain", 0.0f, 4.0f, 1.0f, "linear" } };
        case NodeType::filter: return { { "cutoff-hz", 5.0f, 20000.0f, 80.0f, "Hz" }, { "high-pass", 0.0f, 1.0f, 1.0f, "bool" } };
        case NodeType::triodeStage: return { { "drive", 0.0f, 1.0f, 0.5f, "normalized" }, { "bias", 0.0f, 1.0f, 0.5f, "normalized" }, { "plate-voltage-v", 20.0f, 800.0f, 250.0f, "V" }, { "plate-resistance-ohm", 10000.0f, 1000000.0f, 100000.0f, "ohm" }, { "cathode-resistance-ohm", 100.0f, 10000.0f, 1500.0f, "ohm" }, { "cathode-bypass-f", 1.0e-9f, 1.0e-3f, 1.0e-6f, "F" }, { "grid-stopper-ohm", 0.0f, 1000000.0f, 33000.0f, "ohm" }, { "coupling-cap-f", 1.0e-10f, 1.0e-3f, 22.0e-9f, "F" }, { "input-attenuation", 0.0f, 2.0f, 1.0f, "ratio" }, { "output-trim", 0.0f, 2.0f, 0.82f, "ratio" }, { "aging", 0.0f, 1.0f, 0.0f, "normalized" } };
        case NodeType::toneStack: return { { "bass", 0.0f, 1.0f, 0.5f, "normalized" }, { "middle", 0.0f, 1.0f, 0.5f, "normalized" }, { "treble", 0.0f, 1.0f, 0.5f, "normalized" }, { "slope-resistance-ohm", 10000.0f, 1000000.0f, 100000.0f, "ohm" }, { "mid-resistance-ohm", 1000.0f, 1000000.0f, 25000.0f, "ohm" }, { "bass-pot-ohm", 10000.0f, 2000000.0f, 1000000.0f, "ohm" }, { "mid-pot-ohm", 1000.0f, 1000000.0f, 25000.0f, "ohm" }, { "treble-pot-ohm", 10000.0f, 1000000.0f, 250000.0f, "ohm" }, { "bass-cap-f", 1.0e-10f, 1.0e-6f, 22.0e-9f, "F" }, { "mid-cap-f", 1.0e-10f, 1.0e-6f, 22.0e-9f, "F" }, { "treble-cap-f", 1.0e-11f, 1.0e-7f, 250.0e-12f, "F" } };
        case NodeType::phaseInverter: return { { "drive", 0.0f, 1.0f, 0.45f, "normalized" }, { "imbalance", -0.25f, 0.25f, 0.04f, "ratio" } };
        case NodeType::powerStage: return { { "drive", 0.0f, 1.0f, 0.5f, "normalized" }, { "sag", 0.0f, 1.0f, 0.3f, "normalized" }, { "feedback", 0.0f, 0.92f, 0.25f, "ratio" }, { "supply-voltage-v", 50.0f, 800.0f, 420.0f, "V" }, { "topology", 0.0f, 2.0f, 2.0f, "enum" }, { "tube-count", 1.0f, 8.0f, 2.0f, "count" }, { "bias", 0.0f, 1.0f, 0.55f, "normalized" }, { "load-ohm", 500.0f, 20000.0f, 3400.0f, "ohm" }, { "damping", 0.0f, 1.0f, 0.5f, "normalized" }, { "saturation", 0.0f, 1.0f, 0.5f, "normalized" }, { "supply-attack-ms", 0.5f, 1000.0f, 8.0f, "ms" }, { "supply-recovery-ms", 1.0f, 5000.0f, 85.0f, "ms" }, { "rectifier-stiffness", 0.0f, 1.0f, 0.6f, "normalized" }, { "ripple", 0.0f, 1.0f, 0.0f, "normalized" } };
        case NodeType::feedback: return { { "amount", 0.0f, 0.92f, 0.25f, "ratio" }, { "presence", 0.0f, 1.0f, 0.5f, "normalized" } };
        case NodeType::transformer: return { { "saturation", 0.0f, 1.0f, 0.3f, "normalized" }, { "turns-ratio", 2.0f, 100.0f, 20.0f, "ratio" }, { "leakage", 0.0f, 1.0f, 0.25f, "normalized" }, { "damping", 0.0f, 1.0f, 0.5f, "normalized" } };
        case NodeType::cabinet: return { { "resonance", 0.0f, 1.0f, 0.5f, "normalized" }, { "brightness", 0.0f, 1.0f, 0.5f, "normalized" } };
    }
    return {};
}

std::unique_ptr<ICircuitComponent> createComponent(const NodeSpec& spec, std::unique_ptr<INonlinearComponentModel> hybridModel)
{
    switch (spec.type)
    {
        case NodeType::input: case NodeType::output: return std::make_unique<GainComponent>(spec);
        case NodeType::filter: return std::make_unique<FilterComponent>(spec);
        case NodeType::triodeStage: return std::make_unique<TriodeComponent>(spec, std::move(hybridModel));
        case NodeType::toneStack: return std::make_unique<ToneStackComponent>(spec);
        case NodeType::phaseInverter: return std::make_unique<PhaseInverterComponent>(spec);
        case NodeType::powerStage: return std::make_unique<PowerComponent>(spec);
        case NodeType::feedback: return std::make_unique<FeedbackComponent>(spec);
        case NodeType::transformer: return std::make_unique<TransformerComponent>(spec);
        case NodeType::cabinet: return std::make_unique<CabinetComponent>(spec);
    }
    return {};
}

double toneStackMagnitude(const NodeSpec& spec, double frequency, double sampleRate) noexcept
{
    const auto bass = clamp01(parameterValue(spec, "bass", 0.5f));
    const auto middle = clamp01(parameterValue(spec, "middle", 0.5f));
    const auto treble = clamp01(parameterValue(spec, "treble", 0.5f));
    const auto resistance = std::clamp<double>(parameterValue(spec, "slope-resistance-ohm", 100000.0f), 10000.0, 1000000.0);
    const auto midResistance = std::clamp<double>(parameterValue(spec, "mid-resistance-ohm", 25000.0f), 1000.0, 1000000.0);
    const auto bassRange = std::clamp<double>(parameterValue(spec, "bass-pot-ohm", 1000000.0f) / 1000000.0f, 0.2, 2.0);
    const auto midRange = std::clamp<double>(parameterValue(spec, "mid-pot-ohm", 25000.0f) / 25000.0f, 0.2, 4.0);
    const auto trebleRange = std::clamp<double>(parameterValue(spec, "treble-pot-ohm", 250000.0f) / 250000.0f, 0.2, 4.0);
    const auto bassCap = std::clamp<double>(parameterValue(spec, "bass-cap-f", 22.0e-9f), 1.0e-10, 1.0e-6);
    const auto midCap = std::clamp<double>(parameterValue(spec, "mid-cap-f", 22.0e-9f), 1.0e-10, 1.0e-6);
    const auto trebleCap = std::clamp<double>(parameterValue(spec, "treble-cap-f", 250.0e-12f), 1.0e-11, 1.0e-7);
    const auto bassCorner = std::clamp(1.0 / (2.0 * std::numbers::pi * resistance * bassCap), 40.0, 250.0);
    const auto midCorner = std::clamp(1.8 / (2.0 * std::numbers::pi * (resistance + midResistance) * midCap), 250.0, 1800.0);
    const auto trebleCorner = std::clamp(0.018 / (2.0 * std::numbers::pi * resistance * trebleCap), 1800.0, 7000.0);
    const auto low = nts::dsp::BiquadCoefficients::make(nts::dsp::FilterType::lowShelf, sampleRate, bassCorner, 0.707, (bass - 0.5f) * 18.0f * bassRange);
    const auto mid = nts::dsp::BiquadCoefficients::make(nts::dsp::FilterType::peaking, sampleRate, midCorner, 0.7, (middle - 0.5f) * 16.0f * midRange);
    const auto high = nts::dsp::BiquadCoefficients::make(nts::dsp::FilterType::highShelf, sampleRate, trebleCorner, 0.707, (treble - 0.5f) * 18.0f * trebleRange);
    return 0.72 * low.magnitude(frequency, sampleRate) * mid.magnitude(frequency, sampleRate) * high.magnitude(frequency, sampleRate);
}
} // namespace nts::circuit
