#pragma once

#include "AudioProcessContext.h"

#include <concepts>
#include <cstddef>

namespace nts::audio
{
template <typename Processor>
concept AudioProcessor = requires(Processor& processor, AudioProcessContext& context)
{
    { processor.prepare(48000.0, std::size_t { 512 }, std::size_t { 2 }, std::size_t { 2 }) } -> std::same_as<void>;
    { processor.reset() } noexcept -> std::same_as<void>;
    { processor.process(context) } noexcept -> std::same_as<void>;
};
} // namespace nts::audio
