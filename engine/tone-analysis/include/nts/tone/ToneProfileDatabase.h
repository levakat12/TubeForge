#pragma once

#include "ToneAnalysis.h"

#include <optional>
#include <string>
#include <vector>

namespace nts::tone
{
struct ToneProfile
{
    std::string profileId;
    std::string name;
    ToneEmbedding embedding;
    ToneFeatures features;
    ToneReport report;
    Instrument instrument { Instrument::unknown };
    SourceType sourceType { SourceType::userRecording };
    float captureQuality {};
    std::string modelVersion { currentAnalysisVersion };
    std::vector<std::string> userTags;
    std::string licensingMetadata;
};

struct SimilarityBreakdown
{
    float score {};
    float embedding {};
    float spectral {};
    float dynamics {};
    float transient {};
    float lowFrequency {};
    float nonlinear {};
};

struct SearchResult
{
    std::string profileId;
    std::string name;
    SimilarityBreakdown similarity;
};

class ToneSimilarity
{
public:
    [[nodiscard]] static SimilarityBreakdown compare(const ToneProfile& left,
                                                     const ToneProfile& right) noexcept;
};

class ToneProfileDatabase
{
public:
    bool addOrReplace(ToneProfile profile, std::string& error);
    [[nodiscard]] std::optional<ToneProfile> find(std::string_view profileId) const;
    [[nodiscard]] std::vector<SearchResult> search(const ToneProfile& query, std::size_t limit = 8) const;
    [[nodiscard]] std::size_t size() const noexcept { return profiles.size(); }
    [[nodiscard]] const std::vector<ToneProfile>& all() const noexcept { return profiles; }
    [[nodiscard]] std::string serialize(bool pretty = true) const;
    bool deserialize(std::string_view json, std::string& error);

private:
    std::vector<ToneProfile> profiles;
};
} // namespace nts::tone
