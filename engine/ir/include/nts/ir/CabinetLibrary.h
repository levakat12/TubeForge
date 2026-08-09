#pragma once

#include <juce_core/juce_core.h>

#include <string>
#include <vector>

/** The user's own collection of impulse responses, as a list rather than a file dialog.

    **Why this exists.** Two Load buttons and a file chooser is a workable way to use three impulse
    responses and a hopeless way to use four hundred, which is what a single commercial pack
    contains. The thing a player actually does is *audition* — try a dozen, keep two — and a modal
    file dialog makes each of those a four-click round trip through a directory tree they have
    already navigated eleven times.

    Deliberately a **scanned directory** rather than an imported library. Captures are converted on
    import because a `.nam` is not something the audio thread can read; an impulse response is a
    WAV file, and copying it into a private folder would mean a second copy of somebody's pack, a
    sync problem, and a library that disagrees with the disk the moment a file is renamed. The
    directory the user already keeps their responses in *is* the library.

    Favourites and recents are the plug-in's own, so those are stored — in one small file beside
    the other TubeForge preferences, keyed by path.
*/
namespace nts::ir
{
struct CabinetLibraryEntry
{
    juce::File file;
    /// File name without its extension, which is what a pack author put the useful part in.
    juce::String name;
    /// Path from the library root to the containing folder, or empty at the root. Used as the
    /// shelf a response is filed under, because that is how packs are already organised.
    juce::String folder;
    bool favourite {};
};

class CabinetLibrary
{
public:
    /** `preferencesFile` holds the root, the favourites and the recents. Read on construction. */
    explicit CabinetLibrary(juce::File preferencesFile);

    /// Where the scan starts. An empty file means the user has not chosen one yet.
    [[nodiscard]] juce::File rootDirectory() const { return root; }
    void setRootDirectory(const juce::File& directory);

    /** Rescans the root.

        Bounded at `maximumEntries` and at a directory depth of eight, because this walks a
        directory the user chose and "my drive" is a thing somebody will eventually choose. Hitting
        either bound is reported through `truncated` rather than silently producing a short list.
    */
    void refresh();
    [[nodiscard]] const std::vector<CabinetLibraryEntry>& entries() const { return scanned; }
    /// True when the scan stopped early. The interface says so rather than implying completeness.
    [[nodiscard]] bool truncated() const { return scanStopped; }
    /// Distinct folder names found, in the order they should be offered. Empty for a flat library.
    [[nodiscard]] std::vector<juce::String> folders() const;

    void setFavourite(const juce::File& file, bool favourite);
    [[nodiscard]] bool isFavourite(const juce::File& file) const;
    /** Records a response as used, most recent first. Bounded at `maximumRecents`.

        Recents are kept by path rather than by index into the scan, so a response that has since
        been moved or deleted simply stops appearing instead of pointing at whatever took its place.
    */
    void noteUsed(const juce::File& file);
    /// Most recent first, filtered to files that still exist.
    [[nodiscard]] std::vector<CabinetLibraryEntry> recents() const;

    static constexpr std::size_t maximumEntries = 4000;
    static constexpr std::size_t maximumRecents = 12;

private:
    void load();
    void save() const;

    juce::File preferences;
    juce::File root;
    std::vector<CabinetLibraryEntry> scanned;
    bool scanStopped {};
    std::vector<std::string> favourites;
    std::vector<std::string> recentPaths;
};
} // namespace nts::ir
