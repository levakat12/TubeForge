#pragma once

#include "CircuitTypes.h"

#include <nts/dsp/Common.h>
#include <nts/dsp/Filters.h>
#include <nts/ml/PackedTanhModel.h>

#include <atomic>
#include <array>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace nts::circuit
{
class INonlinearComponentModel
{
public:
    virtual ~INonlinearComponentModel() = default;
    virtual void prepare(double sampleRate, std::size_t maximumBlockSize) = 0;
    virtual void reset() noexcept = 0;
    virtual void setPhysicalParameters(std::span<const float> values) noexcept = 0;
    virtual void process(std::span<const float> input, std::span<float> output) noexcept = 0;
    [[nodiscard]] virtual std::size_t latencySamples() const noexcept = 0;
};

class PackedNeuralComponentModel final : public INonlinearComponentModel
{
public:
    bool load(std::span<const std::byte> bytes, std::string& error);
    void prepare(double sampleRate, std::size_t maximumBlockSize) override;
    void reset() noexcept override;
    void setPhysicalParameters(std::span<const float> values) noexcept override;
    void process(std::span<const float> input, std::span<float> output) noexcept override;
    [[nodiscard]] std::size_t latencySamples() const noexcept override { return 0; }
    [[nodiscard]] int modelSampleRate() const noexcept { return model.sampleRate(); }

private:
    nts::ml::PackedTanhModel model;
    std::array<float, 32> physicalControls {};
    bool sampleRateMatches {};
};

class ICircuitComponent
{
public:
    virtual ~ICircuitComponent() = default;
    virtual void prepare(const nts::dsp::ProcessSpec& spec) = 0;
    virtual void reset() noexcept = 0;
    virtual void process(std::span<float> samples) noexcept = 0;
    [[nodiscard]] virtual ComponentTelemetry telemetry() const noexcept = 0;
    [[nodiscard]] virtual std::size_t latencySamples() const noexcept { return 0; }
    [[nodiscard]] virtual std::size_t stateSize() const noexcept { return 0; }
};

class TubeLibrary
{
public:
    [[nodiscard]] static std::span<const TubeDefinition> definitions() noexcept;
    [[nodiscard]] static const TubeDefinition* find(std::string_view stableId) noexcept;
};

[[nodiscard]] std::vector<ParameterDescriptor> parameterSchema(NodeType type);
[[nodiscard]] std::unique_ptr<ICircuitComponent> createComponent(
    const NodeSpec& spec, std::unique_ptr<INonlinearComponentModel> hybridModel = {});
[[nodiscard]] double toneStackMagnitude(const NodeSpec& spec, double frequency, double sampleRate) noexcept;
} // namespace nts::circuit
