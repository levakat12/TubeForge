#pragma once

#include "Capture.h"

#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace nts::nam
{
/** One converted capture, as the library knows it.

    Everything here is read back from the artifact's own sidecar rather than remembered in a
    separate index, so the library cannot disagree with what is on disk: deleting a folder
    removes a capture, and nothing has to be told about it.
*/
struct LibraryEntry
{
    /// The artifact directory name. Stable, and keyed by the source capture's digest.
    std::string id;
    std::string displayName;
    std::string gearMake, gearModel, toneType, modeledBy;
    GearKind gear { GearKind::unknown };
    Tier tier { Tier::standard };
    int sampleRate {};
    /// WaveNet width. The whole difference between the two tiers, and a fair proxy for cost.
    int channels {};
    /// The capture author's own reported error, or a negative value when the file omits it.
    double validationEsr { -1.0 };
    /// The archive or file this was imported from, for the user's benefit rather than the code's.
    std::string source;
    juce::File artifact;

    [[nodiscard]] juce::String summary() const;
};

struct ImportReport
{
    int converted {}, reused {}, failed {}, cancelled {};
    /// One line per capture that could not be converted, naming the capture and the reason.
    std::vector<std::string> messages;

    [[nodiscard]] int total() const noexcept { return converted + reused + failed; }
};

/** The user's collection of converted captures.

    Import takes a `.zip`, a `.nam`, or a folder of either and converts **everything** it finds.
    That is the only arrangement that makes the real corpora usable: a single pedal archive holds
    up to eighty-nine captures of the same box at different settings, so "extract it and load the
    one you want" is not a question with an answer at import time. Converting the lot up front
    turns choosing one into browsing a list, which is a thing a person can actually do.

    Conversion is keyed by the source capture's SHA-256, so re-importing an archive you already
    have is cheap and cannot produce duplicates. Archive entries are read into memory and written
    only to a directory named from that digest -- no entry's own path is ever used as a
    destination, so a hostile archive cannot write outside the library.
*/
class CaptureLibrary
{
public:
    /// `root` is created on demand; it is a directory of artifact directories and nothing else.
    explicit CaptureLibrary(juce::File root);

    /** Converts every capture in `source` into the library.

        Runs on a worker: it decompresses, parses, and runs each converted model once to produce
        its verification vectors. `progress` is called with (done, total, label) as it goes, and
        `stopToken` is honoured between captures.
    */
    ImportReport import(const juce::File& source, Tier tier, const std::stop_token& stopToken,
                        const std::function<void(int, int, const juce::String&)>& progress);

    /// Rescans the root. Cheap enough to call whenever the page is opened.
    void refresh();
    [[nodiscard]] std::vector<LibraryEntry> entries() const;
    [[nodiscard]] std::optional<LibraryEntry> find(const std::string& id) const;
    /** Orders entries for a destination: matching gear kind first, then by name.

        Sorting rather than filtering, because `gear_type` is the capture author's label and not
        a rule -- a full-rig capture in a pedal slot is a strange thing to want and a legitimate
        one to try.
    */
    [[nodiscard]] std::vector<LibraryEntry> orderedFor(GearKind preferred) const;
    /// Deletes an artifact directory. Returns false and sets `error` if it could not be removed.
    bool remove(const std::string& id, std::string& error);
    [[nodiscard]] juce::File rootDirectory() const { return root; }
    /// Whether an entry's declared gear kind sits oddly in a given destination.
    [[nodiscard]] static bool isMismatch(GearKind entry, GearKind destination) noexcept;

private:
    /** Converts one capture's JSON text, or returns nullopt with a named reason.

        `reused` comes back true when the digest already resolved to an artifact on disk and
        nothing was written, which is what makes re-importing a familiar archive cheap.
    */
    std::optional<LibraryEntry> convert(const juce::String& json, const juce::String& sourceName,
                                        const juce::String& archiveName, Tier tier, bool& reused,
                                        std::string& error);
    [[nodiscard]] std::optional<LibraryEntry> readSidecar(const juce::File& directory) const;

    juce::File root;
    std::vector<LibraryEntry> cached;
};
} // namespace nts::nam
