#pragma once

#include <juce_core/juce_core.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace nts::ecosystem
{
inline constexpr int tonePackageFormatVersion = 1;
inline constexpr const char* currentApplicationVersion = "0.10.0";

enum class AssetLicense
{
    userOwned,
    publicDomain,
    permissive,
    privateUse,
    unknown
};

struct PackageAsset
{
    std::string path;
    std::string kind;
    std::string sha256;
    std::uint64_t sizeBytes {};
    AssetLicense license { AssetLicense::unknown };
    bool redistributionAllowed {};
};

struct TonePackageManifest
{
    int packageFormat { tonePackageFormatVersion };
    std::string packageId;
    std::string name;
    std::string author;
    std::string instrument { "guitar" };
    std::string createdWith { currentApplicationVersion };
    std::string minimumRuntime { currentApplicationVersion };
    std::string createdUtc;
    std::string qualityTier { "standard" };
    std::vector<std::string> tags;
    std::vector<int> sampleRates { 44100, 48000, 88200, 96000 };
    std::vector<std::string> modelOperators;
    std::vector<PackageAsset> assets;
    /** Which cabinet slot each `cabinet-ir` asset belongs to, in the order they were exported.

        Parallel to the cabinet assets rather than folded into their paths, because the exporter
        names them by index and a package that carries only slot B's response would otherwise be
        indistinguishable from one carrying only slot A's. Empty for a package that carries no
        cabinet responses, which is the normal case and every package written before this existed.
    */
    std::vector<int> cabinetSlots;
    std::string packageLicense { "unknown" };
    bool sourceAudioIncluded {};
    std::string signerId;
    std::string signatureAlgorithm;
    std::string signature;
};

struct PackageLimits
{
    std::uint64_t manifestBytes { 1024 * 1024 };
    std::uint64_t rigBytes { 2 * 1024 * 1024 };
    std::uint64_t modelBytes { 64 * 1024 * 1024 };
    std::uint64_t impulseResponseBytes { 16 * 1024 * 1024 };
    std::uint64_t previewBytes { 30 * 1024 * 1024 };
    std::uint64_t totalBytes { 128 * 1024 * 1024 };
    std::size_t fileCount { 128 };
};

struct SignatureKey
{
    std::string signerId;
    std::string encodedPublicKey;
};

struct PackageValidationPolicy
{
    PackageLimits limits;
    std::vector<SignatureKey> trustedSigners;
    bool allowUnsigned { true };
    bool requireRedistributableAssets {};
};

struct PackageValidationResult
{
    bool valid {};
    bool signatureVerified {};
    TonePackageManifest manifest;
    std::vector<std::string> warnings;
    std::string error;
};

struct PackageExportRequest
{
    TonePackageManifest manifest;
    std::string rigJson;
    std::optional<std::filesystem::path> modelFile;
    std::optional<std::filesystem::path> modelTestFile;
    std::vector<std::filesystem::path> cabinetFiles;
    std::vector<std::filesystem::path> previewFiles;
    std::optional<std::filesystem::path> licenseFile;
    std::string signerId;
    std::string encodedPrivateKey;
};

[[nodiscard]] const char* toString(AssetLicense license) noexcept;
[[nodiscard]] std::optional<AssetLicense> assetLicenseFromString(std::string_view value) noexcept;
[[nodiscard]] bool isSafePackagePath(std::string_view path) noexcept;
[[nodiscard]] std::string packageManifestJson(const TonePackageManifest& manifest,
                                              bool includeSignature,
                                              bool pretty = true);
[[nodiscard]] std::optional<TonePackageManifest> parsePackageManifest(std::string_view json,
                                                                      std::string& error);
[[nodiscard]] std::string sha256File(const std::filesystem::path& file, std::string& error);
[[nodiscard]] std::string signPackageManifest(const TonePackageManifest& manifest,
                                              std::string_view encodedPrivateKey,
                                              std::string& error);
[[nodiscard]] bool verifyPackageManifestSignature(const TonePackageManifest& manifest,
                                                  std::string_view encodedPublicKey,
                                                  std::string& error);
[[nodiscard]] bool exportTonePackage(const std::filesystem::path& destination,
                                     PackageExportRequest request,
                                     std::string& error);
[[nodiscard]] PackageValidationResult validateTonePackage(const std::filesystem::path& package,
                                                          const PackageValidationPolicy& policy = {});

struct ProfileQuery
{
    std::string searchText;
    std::string instrument;
    std::string qualityTier;
    std::optional<int> sampleRate;
    bool favoritesOnly {};
    bool compatibleOnly {};
};

struct ProfileRecord
{
    std::filesystem::path packagePath;
    TonePackageManifest manifest;
    bool favorite {};
    std::string lastUsedUtc;
    bool compatible { true };
    bool signatureVerified {};
    std::vector<std::string> warnings;
};

class ProfileLibrary
{
public:
    explicit ProfileLibrary(std::filesystem::path rootDirectory,
                            PackageValidationPolicy validationPolicy = {});

    bool refresh(std::string& error);
    [[nodiscard]] std::vector<ProfileRecord> search(const ProfileQuery& query) const;
    bool importPackage(const std::filesystem::path& source, std::string& error);
    bool exportPackage(const std::filesystem::path& destination,
                       PackageExportRequest request,
                       std::string& error) const;
    bool setFavorite(std::string_view packageId, bool favorite, std::string& error);
    bool markUsed(std::string_view packageId, std::string& error);
    [[nodiscard]] std::optional<ProfileRecord> find(std::string_view packageId) const;
    [[nodiscard]] const std::filesystem::path& root() const noexcept { return rootDirectory; }

private:
    bool loadIndex(std::string& error);
    bool saveIndex(std::string& error) const;
    [[nodiscard]] bool isRuntimeCompatible(const TonePackageManifest& manifest) const;

    struct IndexEntry { bool favorite {}; std::string lastUsedUtc; };
    std::filesystem::path rootDirectory;
    PackageValidationPolicy policy;
    std::vector<ProfileRecord> records;
    std::map<std::string, IndexEntry, std::less<>> index;
};
} // namespace nts::ecosystem
