#pragma once

#include "ProjectState.h"

#include <filesystem>

namespace nts::state
{
class SettingsStore
{
public:
    explicit SettingsStore(std::filesystem::path settingsFile);

    [[nodiscard]] bool save(const ProjectState& state, std::string& error) const;
    [[nodiscard]] StateResult load() const;
    [[nodiscard]] ProjectState loadOrDefault() const;

private:
    std::filesystem::path file;
};
} // namespace nts::state
