#include <nts/ir/CabinetLibrary.h>

#include <algorithm>

namespace nts::ir
{
namespace
{
/// Extensions the cabinet loader can actually decode. Kept here rather than duplicated as a
/// wildcard string, so the browser cannot offer a file the loader will then refuse.
[[nodiscard]] bool isImpulseResponse(const juce::File& file)
{
    const auto extension = file.getFileExtension().toLowerCase();
    return extension == ".wav" || extension == ".aiff" || extension == ".aif" || extension == ".flac";
}
} // namespace

CabinetLibrary::CabinetLibrary(juce::File preferencesFile) : preferences(std::move(preferencesFile))
{
    load();
    if (root != juce::File {}) refresh();
}

void CabinetLibrary::setRootDirectory(const juce::File& directory)
{
    root = directory;
    save();
    refresh();
}

void CabinetLibrary::refresh()
{
    scanned.clear();
    scanStopped = false;
    if (root == juce::File {} || ! root.isDirectory()) return;

    /* Bounded on both count and depth, because this walks a directory the user chose.

       Somebody will eventually point it at a drive root, and the failure there is not merely a
       slow scan -- it is a list of forty thousand entries that the interface then has to draw. A
       truncated scan that says it was truncated is the honest outcome; a hang is not. */
    for (const auto& item : juce::RangedDirectoryIterator(root, true, "*",
                                                          juce::File::findFiles))
    {
        if (scanned.size() >= maximumEntries) { scanStopped = true; break; }
        const auto file = item.getFile();
        if (! isImpulseResponse(file)) continue;
        const auto relative = file.getParentDirectory().getRelativePathFrom(root);
        // A path that climbs out of the root means a symlink pointing elsewhere; filed at the
        // root rather than under a folder name made of "..".
        const auto folder = relative == "." || relative.startsWith("..") ? juce::String()
                                                                        : relative;
        if (folder.isNotEmpty() && folder.retainCharacters("\\/").length() >= 8)
        {
            scanStopped = true;
            continue;
        }
        scanned.push_back({ file, file.getFileNameWithoutExtension(), folder,
                            isFavourite(file) });
    }

    // Favourites first, then by folder, then by name. Sorting rather than filtering: a favourite
    // is a shortcut to the top of the list, not a different list.
    std::sort(scanned.begin(), scanned.end(), [](const auto& first, const auto& second)
    {
        if (first.favourite != second.favourite) return first.favourite;
        if (first.folder != second.folder) return first.folder < second.folder;
        return first.name.compareNatural(second.name) < 0;
    });
}

std::vector<juce::String> CabinetLibrary::folders() const
{
    std::vector<juce::String> names;
    for (const auto& entry : scanned)
    {
        if (entry.folder.isEmpty()) continue;
        if (std::find(names.begin(), names.end(), entry.folder) == names.end())
            names.push_back(entry.folder);
    }
    std::sort(names.begin(), names.end(),
              [](const auto& first, const auto& second) { return first.compareNatural(second) < 0; });
    return names;
}

bool CabinetLibrary::isFavourite(const juce::File& file) const
{
    const auto path = file.getFullPathName().toStdString();
    return std::find(favourites.begin(), favourites.end(), path) != favourites.end();
}

void CabinetLibrary::setFavourite(const juce::File& file, bool favourite)
{
    const auto path = file.getFullPathName().toStdString();
    const auto existing = std::find(favourites.begin(), favourites.end(), path);
    if (favourite && existing == favourites.end()) favourites.push_back(path);
    else if (! favourite && existing != favourites.end()) favourites.erase(existing);
    else return;
    for (auto& entry : scanned)
        if (entry.file == file) entry.favourite = favourite;
    save();
}

void CabinetLibrary::noteUsed(const juce::File& file)
{
    const auto path = file.getFullPathName().toStdString();
    std::erase(recentPaths, path);
    recentPaths.insert(recentPaths.begin(), path);
    if (recentPaths.size() > maximumRecents) recentPaths.resize(maximumRecents);
    save();
}

std::vector<CabinetLibraryEntry> CabinetLibrary::recents() const
{
    std::vector<CabinetLibraryEntry> result;
    for (const auto& path : recentPaths)
    {
        const juce::File file(juce::String::fromUTF8(path.c_str()));
        // Filtered rather than pruned: a response on a drive that is not mounted right now should
        // come back when it is, and deleting the entry here would lose it permanently.
        if (! file.existsAsFile()) continue;
        result.push_back({ file, file.getFileNameWithoutExtension(),
                           root == juce::File {} ? juce::String()
                                                 : file.getParentDirectory().getRelativePathFrom(root),
                           isFavourite(file) });
    }
    return result;
}

void CabinetLibrary::load()
{
    if (! preferences.existsAsFile()) return;
    juce::var parsed;
    if (juce::JSON::parse(preferences.loadFileAsString(), parsed).failed() || ! parsed.isObject())
        return;
    root = juce::File(parsed.getProperty("root", "").toString());
    const auto readPaths = [&parsed](const char* key, std::vector<std::string>& into)
    {
        if (const auto* array = parsed.getProperty(key, {}).getArray())
            for (const auto& value : *array) into.push_back(value.toString().toStdString());
    };
    readPaths("favourites", favourites);
    readPaths("recents", recentPaths);
    if (recentPaths.size() > maximumRecents) recentPaths.resize(maximumRecents);
}

void CabinetLibrary::save() const
{
    auto* object = new juce::DynamicObject();
    object->setProperty("root", root.getFullPathName());
    const auto pathArray = [](const std::vector<std::string>& paths)
    {
        juce::Array<juce::var> values;
        for (const auto& path : paths) values.add(juce::String::fromUTF8(path.c_str()));
        return values;
    };
    object->setProperty("favourites", pathArray(favourites));
    object->setProperty("recents", pathArray(recentPaths));
    preferences.getParentDirectory().createDirectory();
    preferences.replaceWithText(juce::JSON::toString(juce::var(object), true));
}
} // namespace nts::ir
