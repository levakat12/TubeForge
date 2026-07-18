#include "nts/state/SettingsStore.h"

#include <fstream>
#include <iterator>

namespace nts::state
{
SettingsStore::SettingsStore(std::filesystem::path settingsFile)
    : file(std::move(settingsFile))
{
}

bool SettingsStore::save(const ProjectState& state, std::string& error) const
{
    if (! validate(state, error))
        return false;

    std::error_code filesystemError;
    if (const auto parent = file.parent_path(); ! parent.empty())
        std::filesystem::create_directories(parent, filesystemError);
    if (filesystemError)
    {
        error = filesystemError.message();
        return false;
    }

    const auto temporary = file.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (! output)
        {
            error = "Unable to open temporary settings file";
            return false;
        }
        output << serialize(state, true);
        if (! output)
        {
            error = "Unable to write settings file";
            return false;
        }
    }

    std::filesystem::copy_file(temporary, file,
                               std::filesystem::copy_options::overwrite_existing,
                               filesystemError);
    if (filesystemError)
    {
        error = filesystemError.message();
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return false;
    }

    std::filesystem::remove(temporary, filesystemError);
    if (filesystemError)
    {
        error = "Settings were saved, but the temporary file could not be removed: "
            + filesystemError.message();
        return false;
    }
    error.clear();
    return true;
}

StateResult SettingsStore::load() const
{
    std::ifstream input(file, std::ios::binary);
    if (! input)
        return { std::nullopt, "Settings file does not exist or cannot be opened" };

    const std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    return deserialize(contents);
}

ProjectState SettingsStore::loadOrDefault() const
{
    const auto loaded = load();
    return loaded.state.value_or(ProjectState {});
}
} // namespace nts::state
