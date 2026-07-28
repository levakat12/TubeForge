#include <nts/ecosystem/TonePackage.h>

#include <nts/amp/TraditionalAmp.h>
#include <nts/ml/PackedTanhModel.h>

#include <juce_cryptography/juce_cryptography.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <fstream>
#include <numeric>
#include <regex>
#include <set>
#include <sstream>

namespace nts::ecosystem
{
namespace
{
using Path = std::filesystem::path;

std::string stringProperty(const juce::DynamicObject& object, const juce::Identifier& name)
{
    return object.hasProperty(name) ? object.getProperty(name).toString().toStdString() : std::string {};
}

std::string normalPath(const Path& path)
{
    return path.generic_u8string().empty() ? std::string {} : path.generic_string();
}

juce::var stringsVar(const std::vector<std::string>& values)
{
    juce::Array<juce::var> result;
    for (const auto& value : values) result.add(juce::String(value));
    return result;
}

juce::var intsVar(const std::vector<int>& values)
{
    juce::Array<juce::var> result;
    for (const auto value : values) result.add(value);
    return result;
}

bool copyStrings(const juce::var& value, std::vector<std::string>& output)
{
    const auto* array = value.getArray();
    if (array == nullptr) return false;
    output.clear();
    for (const auto& item : *array)
    {
        if (! item.isString()) return false;
        output.push_back(item.toString().toStdString());
    }
    return true;
}

bool copyInts(const juce::var& value, std::vector<int>& output)
{
    const auto* array = value.getArray();
    if (array == nullptr) return false;
    output.clear();
    for (const auto& item : *array)
    {
        const auto number = static_cast<int>(item);
        if (number < 8000 || number > 384000) return false;
        output.push_back(number);
    }
    return true;
}

juce::var manifestVar(const TonePackageManifest& manifest, bool includeSignature)
{
    auto* root = new juce::DynamicObject();
    root->setProperty("packageFormat", manifest.packageFormat);
    root->setProperty("packageId", juce::String(manifest.packageId));
    root->setProperty("name", juce::String(manifest.name));
    root->setProperty("author", juce::String(manifest.author));
    root->setProperty("instrument", juce::String(manifest.instrument));
    root->setProperty("createdWith", juce::String(manifest.createdWith));
    root->setProperty("minimumRuntime", juce::String(manifest.minimumRuntime));
    root->setProperty("createdUtc", juce::String(manifest.createdUtc));
    root->setProperty("qualityTier", juce::String(manifest.qualityTier));
    root->setProperty("tags", stringsVar(manifest.tags));
    root->setProperty("sampleRates", intsVar(manifest.sampleRates));
    root->setProperty("modelOperators", stringsVar(manifest.modelOperators));
    root->setProperty("packageLicense", juce::String(manifest.packageLicense));
    root->setProperty("sourceAudioIncluded", manifest.sourceAudioIncluded);
    juce::Array<juce::var> assets;
    for (const auto& asset : manifest.assets)
    {
        auto* value = new juce::DynamicObject();
        value->setProperty("path", juce::String(asset.path));
        value->setProperty("kind", juce::String(asset.kind));
        value->setProperty("sha256", juce::String(asset.sha256));
        value->setProperty("sizeBytes", static_cast<juce::int64>(asset.sizeBytes));
        value->setProperty("license", toString(asset.license));
        value->setProperty("redistributionAllowed", asset.redistributionAllowed);
        assets.add(juce::var(value));
    }
    root->setProperty("assets", assets);
    if (includeSignature)
    {
        root->setProperty("signerId", juce::String(manifest.signerId));
        root->setProperty("signatureAlgorithm", juce::String(manifest.signatureAlgorithm));
        root->setProperty("signature", juce::String(manifest.signature));
    }
    return juce::var(root);
}

bool validUuid(std::string_view value)
{
    static const std::regex pattern("^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[1-5][0-9a-fA-F]{3}-[89abAB][0-9a-fA-F]{3}-[0-9a-fA-F]{12}$");
    return std::regex_match(value.begin(), value.end(), pattern);
}

bool validSha(std::string_view value)
{
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](unsigned char c)
    { return std::isxdigit(c) != 0; });
}

std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c)
    { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string nowUtc()
{
    return juce::Time::getCurrentTime().toISO8601(true).toStdString();
}

std::string createUuid()
{
    const auto raw = juce::Uuid().toString().toStdString();
    if (raw.size() != 32) return raw;
    return raw.substr(0, 8) + "-" + raw.substr(8, 4) + "-4" + raw.substr(13, 3)
         + "-a" + raw.substr(17, 3) + "-" + raw.substr(20, 12);
}

bool semverAtLeast(std::string_view current, std::string_view required)
{
    const auto split = [](std::string_view text)
    {
        std::array<int, 3> values {};
        std::stringstream stream { std::string(text) };
        char separator {};
        if (! (stream >> values[0])) return std::array<int, 3> { -1, -1, -1 };
        for (int index = 1; index < 3; ++index)
            if (! (stream >> separator >> values[index]) || separator != '.') return std::array<int, 3> { -1, -1, -1 };
        return values;
    };
    const auto have = split(current), need = split(required);
    return have[0] >= 0 && need[0] >= 0 && have >= need;
}

std::uint64_t limitFor(const PackageAsset& asset, const PackageLimits& limits)
{
    if (asset.path == "rig.json") return limits.rigBytes;
    if (asset.path == "model.bin") return limits.modelBytes;
    if (asset.path == "model-test.json") return limits.rigBytes;
    if (asset.path.starts_with("cabinet/")) return limits.impulseResponseBytes;
    if (asset.path.starts_with("preview/")) return limits.previewBytes;
    return limits.rigBytes;
}

bool pathKindAllowed(const PackageAsset& asset)
{
    const auto extension = lower(Path(asset.path).extension().string());
    if (asset.path == "rig.json") return asset.kind == "rig";
    if (asset.path == "model.bin") return asset.kind == "neural-model";
    if (asset.path == "model-test.json") return asset.kind == "model-test-vector";
    if (asset.path == "license.txt") return asset.kind == "license";
    if (asset.path.starts_with("cabinet/"))
        return asset.kind == "cabinet-ir" && (extension == ".wav" || extension == ".aif" || extension == ".aiff");
    if (asset.path.starts_with("preview/"))
        return asset.kind == "preview" && (extension == ".wav" || extension == ".flac" || extension == ".ogg");
    return false;
}

std::optional<PackageAsset> declaredAsset(const TonePackageManifest& manifest, std::string_view path)
{
    const auto found = std::find_if(manifest.assets.begin(), manifest.assets.end(), [&](const auto& asset)
    { return asset.path == path; });
    if (found == manifest.assets.end()) return std::nullopt;
    return *found;
}

bool copyAsset(const Path& source, const Path& destination, std::string& error)
{
    std::error_code code;
    std::filesystem::create_directories(destination.parent_path(), code);
    if (code) { error = "Unable to create package asset directory"; return false; }
    std::filesystem::copy_file(source, destination, std::filesystem::copy_options::none, code);
    if (code) { error = "Unable to copy package asset: " + normalPath(source.filename()); return false; }
    return true;
}

bool addExportedAsset(TonePackageManifest& manifest, const Path& source, const Path& staging,
                      const std::string& relative, const std::string& kind, std::string& error)
{
    if (! std::filesystem::is_regular_file(source)) { error = "Package asset is not a regular file"; return false; }
    if (! copyAsset(source, staging / Path(relative), error)) return false;
    auto metadata = declaredAsset(manifest, relative).value_or(PackageAsset {});
    metadata.path = relative; metadata.kind = kind;
    metadata.sizeBytes = std::filesystem::file_size(source);
    metadata.sha256 = sha256File(source, error);
    if (! error.empty()) return false;
    const auto found = std::find_if(manifest.assets.begin(), manifest.assets.end(), [&](const auto& value)
    { return value.path == relative; });
    if (found == manifest.assets.end()) manifest.assets.push_back(std::move(metadata)); else *found = std::move(metadata);
    return true;
}

bool writeText(const Path& path, std::string_view text, std::string& error)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (! stream) { error = "Unable to create " + normalPath(path.filename()); return false; }
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (! stream) { error = "Unable to write " + normalPath(path.filename()); return false; }
    return true;
}
} // namespace

const char* toString(AssetLicense license) noexcept
{
    switch (license)
    {
        case AssetLicense::userOwned: return "user-owned";
        case AssetLicense::publicDomain: return "public-domain";
        case AssetLicense::permissive: return "permissive";
        case AssetLicense::privateUse: return "private-use";
        case AssetLicense::unknown: return "unknown";
    }
    return "unknown";
}

std::optional<AssetLicense> assetLicenseFromString(std::string_view value) noexcept
{
    if (value == "user-owned") return AssetLicense::userOwned;
    if (value == "public-domain") return AssetLicense::publicDomain;
    if (value == "permissive") return AssetLicense::permissive;
    if (value == "private-use") return AssetLicense::privateUse;
    if (value == "unknown") return AssetLicense::unknown;
    return std::nullopt;
}

bool isSafePackagePath(std::string_view path) noexcept
{
    if (path.empty() || path.size() > 240 || path.find('\\') != std::string_view::npos
        || path.find('\0') != std::string_view::npos || path.front() == '/') return false;
    const Path value(path);
    if (value.is_absolute() || value.has_root_path() || value.lexically_normal() != value) return false;
    for (const auto& component : value)
        if (component == ".." || component == "." || component.empty()) return false;
    return true;
}

std::string packageManifestJson(const TonePackageManifest& manifest, bool includeSignature, bool pretty)
{
    return juce::JSON::toString(manifestVar(manifest, includeSignature), pretty, 8).toStdString();
}

std::optional<TonePackageManifest> parsePackageManifest(std::string_view json, std::string& error)
{
    const auto parsed = juce::JSON::parse(juce::String::fromUTF8(json.data(), static_cast<int>(json.size())));
    const auto* object = parsed.getDynamicObject();
    if (object == nullptr) { error = "Package manifest root must be an object"; return std::nullopt; }
    TonePackageManifest result;
    result.packageFormat = static_cast<int>(object->getProperty("packageFormat"));
    result.packageId = stringProperty(*object, "packageId"); result.name = stringProperty(*object, "name");
    result.author = stringProperty(*object, "author"); result.instrument = stringProperty(*object, "instrument");
    result.createdWith = stringProperty(*object, "createdWith");
    result.minimumRuntime = stringProperty(*object, "minimumRuntime");
    result.createdUtc = stringProperty(*object, "createdUtc");
    result.qualityTier = stringProperty(*object, "qualityTier");
    result.packageLicense = stringProperty(*object, "packageLicense");
    result.sourceAudioIncluded = static_cast<bool>(object->getProperty("sourceAudioIncluded"));
    result.signerId = stringProperty(*object, "signerId");
    result.signatureAlgorithm = stringProperty(*object, "signatureAlgorithm");
    result.signature = stringProperty(*object, "signature");
    if (result.packageFormat != tonePackageFormatVersion || ! validUuid(result.packageId)
        || result.name.empty() || result.name.size() > 120 || result.author.empty() || result.author.size() > 120
        || (result.instrument != "guitar" && result.instrument != "bass")
        || result.createdWith.empty() || result.minimumRuntime.empty() || result.packageLicense.empty())
    { error = "Package manifest has missing or invalid required metadata"; return std::nullopt; }
    if (! copyStrings(object->getProperty("tags"), result.tags)
        || ! copyInts(object->getProperty("sampleRates"), result.sampleRates)
        || ! copyStrings(object->getProperty("modelOperators"), result.modelOperators))
    { error = "Package manifest contains an invalid metadata array"; return std::nullopt; }
    if (result.tags.size() > 32 || result.sampleRates.empty() || result.modelOperators.size() > 16)
    { error = "Package manifest metadata limits were exceeded"; return std::nullopt; }
    const auto* assets = object->getProperty("assets").getArray();
    if (assets == nullptr || assets->isEmpty()) { error = "Package manifest must declare assets"; return std::nullopt; }
    std::set<std::string> paths;
    for (const auto& value : *assets)
    {
        const auto* assetObject = value.getDynamicObject();
        if (assetObject == nullptr) { error = "Package asset metadata must be an object"; return std::nullopt; }
        PackageAsset asset;
        asset.path = stringProperty(*assetObject, "path"); asset.kind = stringProperty(*assetObject, "kind");
        asset.sha256 = lower(stringProperty(*assetObject, "sha256"));
        const auto sizeValue = static_cast<juce::int64>(assetObject->getProperty("sizeBytes"));
        asset.sizeBytes = sizeValue > 0 ? static_cast<std::uint64_t>(sizeValue) : 0;
        const auto license = assetLicenseFromString(stringProperty(*assetObject, "license"));
        asset.redistributionAllowed = static_cast<bool>(assetObject->getProperty("redistributionAllowed"));
        if (! isSafePackagePath(asset.path) || ! pathKindAllowed(asset) || ! validSha(asset.sha256)
            || asset.sizeBytes == 0 || ! license || ! paths.insert(asset.path).second)
        { error = "Package asset metadata is invalid or duplicated"; return std::nullopt; }
        asset.license = *license; result.assets.push_back(std::move(asset));
    }
    if (! paths.contains("rig.json")) { error = "Package does not contain a declared rig.json"; return std::nullopt; }
    if (result.sourceAudioIncluded) { error = "Source audio is not permitted in a .ntone package"; return std::nullopt; }
    error.clear(); return result;
}

std::string sha256File(const Path& file, std::string& error)
{
    if (! std::filesystem::is_regular_file(file)) { error = "Hash target is not a regular file"; return {}; }
    const juce::File juceFile(juce::String(file.wstring().c_str()));
    const auto hash = juce::SHA256(juceFile).toHexString().toLowerCase();
    if (hash.length() != 64) { error = "Unable to hash package asset"; return {}; }
    error.clear(); return hash.toStdString();
}

std::string signPackageManifest(const TonePackageManifest& manifest, std::string_view encodedPrivateKey,
                                std::string& error)
{
    juce::RSAKey privateKey { juce::String::fromUTF8(encodedPrivateKey.data(), static_cast<int>(encodedPrivateKey.size())) };
    if (! privateKey.isValid()) { error = "Invalid package signing key"; return {}; }
    const auto canonical = packageManifestJson(manifest, false, false);
    const auto digest = juce::SHA256(canonical.data(), canonical.size()).toHexString();
    juce::BigInteger value; value.parseString(digest, 16);
    if (! privateKey.applyToValue(value)) { error = "Unable to sign package manifest"; return {}; }
    error.clear(); return value.toString(16).toStdString();
}

bool verifyPackageManifestSignature(const TonePackageManifest& manifest, std::string_view encodedPublicKey,
                                    std::string& error)
{
    if (manifest.signatureAlgorithm != "rsa-sha256-raw-v1" || manifest.signature.empty())
    { error = "Unsupported or missing package signature"; return false; }
    juce::RSAKey publicKey { juce::String::fromUTF8(encodedPublicKey.data(), static_cast<int>(encodedPublicKey.size())) };
    juce::BigInteger signedValue; signedValue.parseString(juce::String(manifest.signature), 16);
    if (! publicKey.isValid() || signedValue.isZero() || ! publicKey.applyToValue(signedValue))
    { error = "Package signature could not be verified"; return false; }
    const auto canonical = packageManifestJson(manifest, false, false);
    juce::BigInteger expected;
    expected.parseString(juce::SHA256(canonical.data(), canonical.size()).toHexString(), 16);
    if (signedValue != expected) { error = "Package signature does not match its manifest"; return false; }
    error.clear(); return true;
}

bool exportTonePackage(const Path& destination, PackageExportRequest request, std::string& error)
{
    if (destination.extension() != ".ntone" || destination.empty() || std::filesystem::exists(destination))
    { error = "Choose a new destination with the .ntone extension"; return false; }
    auto& manifest = request.manifest;
    if (manifest.packageId.empty()) manifest.packageId = createUuid();
    if (manifest.createdUtc.empty()) manifest.createdUtc = nowUtc();
    manifest.packageFormat = tonePackageFormatVersion; manifest.createdWith = currentApplicationVersion;
    manifest.sourceAudioIncluded = false;
    const auto requestedMetadata = manifest.assets;
    manifest.assets.clear();
    const auto metadataFor = [&](std::string_view relative) -> std::optional<PackageAsset>
    {
        const auto found = std::find_if(requestedMetadata.begin(), requestedMetadata.end(), [&](const auto& value)
        { return value.path == relative; });
        return found == requestedMetadata.end() ? std::nullopt : std::optional<PackageAsset>(*found);
    };
    std::string presetError;
    if (! nts::amp::deserializePreset(request.rigJson))
    { error = "rig.json is not a valid TubeForge amp preset"; return false; }
    const auto staging = Path(destination.string() + ".staging-" + juce::Uuid().toString().toStdString());
    std::error_code code; std::filesystem::create_directories(staging, code);
    if (code) { error = "Unable to create package staging directory"; return false; }
    if (! writeText(staging / "rig.json", request.rigJson, error)) return false;
    PackageAsset rig { "rig.json", "rig", {}, static_cast<std::uint64_t>(request.rigJson.size()),
                       AssetLicense::userOwned, true };
    rig.sha256 = sha256File(staging / "rig.json", error); manifest.assets.push_back(rig);
    if (! error.empty()) return false;
    if (request.modelFile)
    {
        if (! request.modelTestFile) { error = "Neural model export requires an embedded model-test.json vector"; return false; }
        if (const auto metadata = metadataFor("model.bin")) manifest.assets.push_back(*metadata);
        if (! addExportedAsset(manifest, *request.modelFile, staging, "model.bin", "neural-model", error)) return false;
        if (const auto metadata = metadataFor("model-test.json")) manifest.assets.push_back(*metadata);
        if (! addExportedAsset(manifest, *request.modelTestFile, staging, "model-test.json", "model-test-vector", error)) return false;
    }
    for (std::size_t index = 0; index < request.cabinetFiles.size(); ++index)
    {
        const auto relative = "cabinet/" + std::to_string(index + 1) + request.cabinetFiles[index].extension().string();
        const auto metadata = metadataFor(relative);
        if (! metadata || ! metadata->redistributionAllowed
            || (metadata->license != AssetLicense::userOwned && metadata->license != AssetLicense::publicDomain
                && metadata->license != AssetLicense::permissive))
        { error = "Cabinet IR export requires explicit redistributable license metadata"; return false; }
        manifest.assets.push_back(*metadata);
        if (! addExportedAsset(manifest, request.cabinetFiles[index], staging, relative, "cabinet-ir", error)) return false;
    }
    for (std::size_t index = 0; index < request.previewFiles.size(); ++index)
    {
        const auto relative = "preview/" + std::to_string(index + 1) + request.previewFiles[index].extension().string();
        if (const auto metadata = metadataFor(relative)) manifest.assets.push_back(*metadata);
        if (! addExportedAsset(manifest, request.previewFiles[index], staging, relative, "preview", error)) return false;
    }
    if (request.licenseFile)
    {
        if (const auto metadata = metadataFor("license.txt")) manifest.assets.push_back(*metadata);
        if (! addExportedAsset(manifest, *request.licenseFile, staging, "license.txt", "license", error)) return false;
    }
    if (! request.encodedPrivateKey.empty())
    {
        manifest.signerId = request.signerId; manifest.signatureAlgorithm = "rsa-sha256-raw-v1";
        manifest.signature = signPackageManifest(manifest, request.encodedPrivateKey, error);
        if (! error.empty()) return false;
    }
    if (! writeText(staging / "manifest.json", packageManifestJson(manifest, true, true), error)) return false;
    std::filesystem::rename(staging, destination, code);
    if (code) { error = "Unable to publish package staging directory"; return false; }
    error.clear(); return true;
}

PackageValidationResult validateTonePackage(const Path& package, const PackageValidationPolicy& policy)
{
    PackageValidationResult result;
    if (package.extension() != ".ntone" || ! std::filesystem::is_directory(package))
    { result.error = ".ntone package must be a directory container"; return result; }
    const auto manifestPath = package / "manifest.json";
    std::error_code code;
    if (! std::filesystem::is_regular_file(manifestPath) || std::filesystem::file_size(manifestPath, code) > policy.limits.manifestBytes)
    { result.error = "Package manifest is missing or exceeds its size limit"; return result; }
    const auto manifestText = juce::File(juce::String(manifestPath.wstring().c_str())).loadFileAsString().toStdString();
    auto parsed = parsePackageManifest(manifestText, result.error);
    if (! parsed) return result;
    result.manifest = std::move(*parsed);
    std::set<std::string> declared { "manifest.json" };
    std::uint64_t total = std::filesystem::file_size(manifestPath, code);
    for (const auto& asset : result.manifest.assets)
    {
        declared.insert(asset.path);
        const auto full = package / Path(asset.path);
        const auto status = std::filesystem::symlink_status(full, code);
        if (code || ! std::filesystem::is_regular_file(status) || std::filesystem::is_symlink(status))
        { result.error = "Package asset is missing, linked, or not a regular file: " + asset.path; return result; }
        const auto actualSize = std::filesystem::file_size(full, code);
        if (code || actualSize != asset.sizeBytes || actualSize > limitFor(asset, policy.limits))
        { result.error = "Package asset size is invalid: " + asset.path; return result; }
        total += actualSize;
        std::string hashError;
        if (sha256File(full, hashError) != lower(asset.sha256))
        { result.error = "Package asset hash mismatch: " + asset.path; return result; }
        if (policy.requireRedistributableAssets && asset.kind == "cabinet-ir" && ! asset.redistributionAllowed)
        { result.error = "Package includes a cabinet IR that is not licensed for redistribution"; return result; }
    }
    std::size_t actualFiles {};
    for (std::filesystem::recursive_directory_iterator iterator(package, std::filesystem::directory_options::skip_permission_denied, code), end;
         iterator != end && ! code; iterator.increment(code))
    {
        const auto status = iterator->symlink_status(code);
        if (code || std::filesystem::is_symlink(status)) { result.error = "Package contains a symbolic link"; return result; }
        if (! std::filesystem::is_regular_file(status)) continue;
        ++actualFiles;
        const auto relative = normalPath(std::filesystem::relative(iterator->path(), package, code));
        if (code || ! declared.contains(relative)) { result.error = "Package contains an undeclared file: " + relative; return result; }
    }
    if (code || actualFiles > policy.limits.fileCount || total > policy.limits.totalBytes)
    { result.error = "Package exceeds file-count or total-size limits"; return result; }
    const auto rigPath = package / "rig.json";
    const auto rig = juce::File(juce::String(rigPath.wstring().c_str())).loadFileAsString().toStdString();
    if (! nts::amp::deserializePreset(rig)) { result.error = "Package rig schema is invalid"; return result; }
    if (declared.contains("model.bin"))
    {
        static const std::set<std::string> operators { "tanh-rnn", "lstm", "gru", "causal-tcn" };
        if (result.manifest.modelOperators.empty()
            || ! std::all_of(result.manifest.modelOperators.begin(), result.manifest.modelOperators.end(), [&](const auto& op)
               { return operators.contains(op); }))
        { result.error = "Package model operator set is missing or unsupported"; return result; }
        nts::ml::PackedTanhModel model; std::string modelError;
        if (! model.loadFile(package / "model.bin", modelError))
        { result.error = "Package neural model failed runtime validation: " + modelError; return result; }
        if (model.memoryBytes() > policy.limits.modelBytes)
        { result.error = "Package neural model exceeds the runtime memory budget"; return result; }
        if (! declared.contains("model-test.json"))
        { result.error = "Package neural model is missing its embedded test vector"; return result; }
        const auto testPath = package / "model-test.json";
        const auto testJson = juce::JSON::parse(juce::File(juce::String(testPath.wstring().c_str())).loadFileAsString());
        const auto* testObject = testJson.getDynamicObject();
        const auto* input = testObject != nullptr ? testObject->getProperty("input").getArray() : nullptr;
        const auto* expected = testObject != nullptr ? testObject->getProperty("expected").getArray() : nullptr;
        const auto tolerance = testObject != nullptr ? static_cast<float>(testObject->getProperty("tolerance")) : 0.0f;
        if (testObject == nullptr || static_cast<int>(testObject->getProperty("schemaVersion")) != 1
            || input == nullptr || expected == nullptr || input->isEmpty() || input->size() != expected->size()
            || input->size() > 4096 || ! std::isfinite(tolerance) || tolerance <= 0.0f || tolerance > 0.01f)
        { result.error = "Package neural model test vector schema is invalid"; return result; }
        std::vector<float> testInput(static_cast<std::size_t>(input->size()));
        std::vector<float> testOutput(testInput.size());
        for (int index = 0; index < input->size(); ++index)
        {
            testInput[static_cast<std::size_t>(index)] = static_cast<float>(input->getReference(index));
            const auto target = static_cast<float>(expected->getReference(index));
            if (! std::isfinite(testInput[static_cast<std::size_t>(index)]) || ! std::isfinite(target))
            { result.error = "Package neural model test vector contains non-finite values"; return result; }
            testOutput[static_cast<std::size_t>(index)] = target;
        }
        std::vector<float> actual(testInput.size()); model.reset();
        if (! model.process(testInput, actual)) { result.error = "Package neural model test vector could not run"; return result; }
        for (std::size_t index = 0; index < actual.size(); ++index)
            if (std::abs(actual[index] - testOutput[index]) > tolerance)
            { result.error = "Package neural model failed its embedded test vector"; return result; }
    }
    if (result.manifest.signature.empty())
    {
        if (! policy.allowUnsigned) { result.error = "Package must be signed"; return result; }
        result.warnings.push_back("Unsigned local package");
    }
    else
    {
        const auto signer = std::find_if(policy.trustedSigners.begin(), policy.trustedSigners.end(), [&](const auto& key)
        { return key.signerId == result.manifest.signerId; });
        if (signer == policy.trustedSigners.end()) { result.error = "Package signer is not trusted"; return result; }
        if (! verifyPackageManifestSignature(result.manifest, signer->encodedPublicKey, result.error)) return result;
        result.signatureVerified = true;
    }
    if (! semverAtLeast(currentApplicationVersion, result.manifest.minimumRuntime))
        result.warnings.push_back("Package requires a newer TubeForge runtime");
    result.valid = true; result.error.clear(); return result;
}

ProfileLibrary::ProfileLibrary(Path root, PackageValidationPolicy validationPolicy)
    : rootDirectory(std::move(root)), policy(std::move(validationPolicy)) {}

bool ProfileLibrary::loadIndex(std::string& error)
{
    index.clear(); const auto file = rootDirectory / "library-index.json";
    if (! std::filesystem::exists(file)) { error.clear(); return true; }
    const auto parsed = juce::JSON::parse(juce::File(juce::String(file.wstring().c_str())).loadFileAsString());
    const auto* object = parsed.getDynamicObject();
    if (object == nullptr) { error = "Profile library index is invalid"; return false; }
    for (const auto& property : object->getProperties())
    {
        const auto* entry = property.value.getDynamicObject(); if (entry == nullptr) continue;
        index[property.name.toString().toStdString()] = {
            static_cast<bool>(entry->getProperty("favorite")), stringProperty(*entry, "lastUsedUtc") };
    }
    error.clear(); return true;
}

bool ProfileLibrary::saveIndex(std::string& error) const
{
    auto* rootObject = new juce::DynamicObject();
    for (const auto& [id, value] : index)
    {
        auto* entry = new juce::DynamicObject(); entry->setProperty("favorite", value.favorite);
        entry->setProperty("lastUsedUtc", juce::String(value.lastUsedUtc));
        rootObject->setProperty(juce::Identifier(id), juce::var(entry));
    }
    return writeText(rootDirectory / "library-index.json", juce::JSON::toString(juce::var(rootObject), true).toStdString(), error);
}

bool ProfileLibrary::isRuntimeCompatible(const TonePackageManifest& manifest) const
{
    return semverAtLeast(currentApplicationVersion, manifest.minimumRuntime);
}

bool ProfileLibrary::refresh(std::string& error)
{
    std::error_code code; std::filesystem::create_directories(rootDirectory, code);
    if (code) { error = "Unable to create the local profile library"; return false; }
    if (! loadIndex(error)) return false;
    records.clear();
    for (const auto& entry : std::filesystem::directory_iterator(rootDirectory, code))
    {
        if (code) break;
        if (! entry.is_directory() || entry.path().extension() != ".ntone") continue;
        auto validation = validateTonePackage(entry.path(), policy);
        if (! validation.valid) continue;
        const auto metadata = index.find(validation.manifest.packageId);
        ProfileRecord record { entry.path(), validation.manifest };
        if (metadata != index.end()) { record.favorite = metadata->second.favorite; record.lastUsedUtc = metadata->second.lastUsedUtc; }
        record.compatible = isRuntimeCompatible(record.manifest); record.signatureVerified = validation.signatureVerified;
        record.warnings = std::move(validation.warnings); records.push_back(std::move(record));
    }
    if (code) { error = "Unable to scan the local profile library"; return false; }
    error.clear(); return true;
}

std::vector<ProfileRecord> ProfileLibrary::search(const ProfileQuery& query) const
{
    std::vector<ProfileRecord> output; const auto text = lower(query.searchText);
    for (const auto& record : records)
    {
        const auto searchable = lower(record.manifest.name + " " + record.manifest.author + " "
            + std::accumulate(record.manifest.tags.begin(), record.manifest.tags.end(), std::string {},
                              [](std::string value, const auto& tag) { return value + " " + tag; }));
        if ((! text.empty() && searchable.find(text) == std::string::npos)
            || (! query.instrument.empty() && record.manifest.instrument != query.instrument)
            || (! query.qualityTier.empty() && record.manifest.qualityTier != query.qualityTier)
            || (query.favoritesOnly && ! record.favorite) || (query.compatibleOnly && ! record.compatible)) continue;
        if (query.sampleRate && std::find(record.manifest.sampleRates.begin(), record.manifest.sampleRates.end(), *query.sampleRate)
                                == record.manifest.sampleRates.end()) continue;
        output.push_back(record);
    }
    std::sort(output.begin(), output.end(), [](const auto& left, const auto& right)
    {
        if (left.favorite != right.favorite) return left.favorite > right.favorite;
        if (left.lastUsedUtc != right.lastUsedUtc) return left.lastUsedUtc > right.lastUsedUtc;
        return left.manifest.name < right.manifest.name;
    });
    return output;
}

bool ProfileLibrary::importPackage(const Path& source, std::string& error)
{
    const auto validation = validateTonePackage(source, policy);
    if (! validation.valid) { error = validation.error; return false; }
    std::error_code code; std::filesystem::create_directories(rootDirectory, code);
    const auto destination = rootDirectory / (validation.manifest.packageId + ".ntone");
    if (code || std::filesystem::exists(destination)) { error = "Profile already exists or library is unavailable"; return false; }
    std::filesystem::copy(source, destination, std::filesystem::copy_options::recursive, code);
    if (code) { error = "Unable to copy profile into the local library"; return false; }
    return refresh(error);
}

bool ProfileLibrary::exportPackage(const Path& destination, PackageExportRequest request, std::string& error) const
{
    return exportTonePackage(destination, std::move(request), error);
}

bool ProfileLibrary::setFavorite(std::string_view packageId, bool favorite, std::string& error)
{
    const auto record = std::find_if(records.begin(), records.end(), [&](const auto& value)
    { return value.manifest.packageId == packageId; });
    if (record == records.end()) { error = "Profile was not found"; return false; }
    index[std::string(packageId)].favorite = favorite; record->favorite = favorite; return saveIndex(error);
}

bool ProfileLibrary::markUsed(std::string_view packageId, std::string& error)
{
    const auto record = std::find_if(records.begin(), records.end(), [&](const auto& value)
    { return value.manifest.packageId == packageId; });
    if (record == records.end()) { error = "Profile was not found"; return false; }
    const auto timestamp = nowUtc(); index[std::string(packageId)].lastUsedUtc = timestamp;
    record->lastUsedUtc = timestamp; return saveIndex(error);
}

std::optional<ProfileRecord> ProfileLibrary::find(std::string_view packageId) const
{
    const auto record = std::find_if(records.begin(), records.end(), [&](const auto& value)
    { return value.manifest.packageId == packageId; });
    return record == records.end() ? std::nullopt : std::optional<ProfileRecord>(*record);
}
} // namespace nts::ecosystem
