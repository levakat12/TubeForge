#include "TestHarness.h"

#include <nts/amp/TraditionalAmp.h>
#include <nts/ecosystem/ReleaseServices.h>
#include <nts/ecosystem/TonePackage.h>

#include <juce_cryptography/juce_cryptography.h>

#include <filesystem>
#include <fstream>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <vector>

namespace
{
std::filesystem::path makeTemporaryRoot()
{
    return std::filesystem::temp_directory_path()
        / ("tubeforge-ecosystem-" + juce::Uuid().toString().toStdString());
}

nts::ecosystem::PackageExportRequest requestFor(std::string name = "Studio Crunch")
{
    nts::ecosystem::PackageExportRequest request;
    request.manifest.name = std::move(name); request.manifest.author = "TubeForge Test";
    request.manifest.instrument = "guitar"; request.manifest.packageLicense = "user-owned";
    request.manifest.tags = { "crunch", "studio" }; request.manifest.qualityTier = "verified";
    request.rigJson = nts::amp::serializePreset(
        nts::amp::makeOriginalPreset(nts::amp::Topology::tightModern, nts::amp::Instrument::guitar));
    return request;
}

std::string signUpdate(const nts::ecosystem::UpdateManifest& manifest, const juce::RSAKey& privateKey)
{
    const auto canonical = nts::ecosystem::updateManifestJson(manifest, false, false);
    juce::BigInteger value;
    value.parseString(juce::SHA256(canonical.data(), canonical.size()).toHexString(), 16);
    auto key = privateKey; key.applyToValue(value); return value.toString(16).toStdString();
}

void appendU32(std::vector<std::byte>& bytes, std::uint32_t value)
{
    const auto offset = bytes.size(); bytes.resize(offset + sizeof(value));
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

void appendFloat(std::vector<std::byte>& bytes, float value)
{
    const auto offset = bytes.size(); bytes.resize(offset + sizeof(value));
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

std::vector<std::byte> tinyModel()
{
    std::vector<std::byte> bytes { std::byte { 'N' }, std::byte { 'T' }, std::byte { 'S' }, std::byte { 'M' } };
    for (const auto value : { 1u, 1u, 48000u, 1u, 1u, 1u }) appendU32(bytes, value);
    for (const auto value : { 0.5f, 0.1f, 0.0f, 1.0f, 0.0f }) appendFloat(bytes, value);
    return bytes;
}
}

int main()
{
    TestHarness tests;
    const auto root = makeTemporaryRoot(); std::filesystem::create_directories(root);
    const auto package = root / "studio-crunch.ntone";
    std::string error;

    tests.expect(! nts::ecosystem::isSafePackagePath("../model.bin"), "package paths reject traversal");
    tests.expect(! nts::ecosystem::isSafePackagePath("C:\\models\\model.bin"), "package paths reject absolute Windows paths");
    tests.expect(! nts::ecosystem::isSafePackagePath("cabinet/../../payload.dll"), "package paths reject normalized traversal");
    tests.expect(nts::ecosystem::isSafePackagePath("cabinet/main.wav"), "package paths allow scoped assets");

    auto request = requestFor();
    tests.expect(nts::ecosystem::exportTonePackage(package, request, error), "valid .ntone package exports: " + error);
    auto validation = nts::ecosystem::validateTonePackage(package);
    tests.expect(validation.valid, "exported .ntone validates: " + validation.error);
    tests.expectEqual(validation.manifest.packageFormat, 1, "package format is independently versioned");
    tests.expect(! validation.warnings.empty(), "unsigned local package is visibly identified");

    {
        std::ofstream stream(package / "undeclared.exe", std::ios::binary); stream << "MZ";
    }
    validation = nts::ecosystem::validateTonePackage(package);
    tests.expect(! validation.valid && validation.error.find("undeclared") != std::string::npos,
                 "undeclared executable content is rejected");
    std::filesystem::remove(package / "undeclared.exe");

    {
        std::ofstream stream(package / "rig.json", std::ios::binary | std::ios::app); stream << "tampered";
    }
    validation = nts::ecosystem::validateTonePackage(package);
    tests.expect(! validation.valid && (validation.error.find("size") != std::string::npos
                                      || validation.error.find("hash") != std::string::npos),
                 "tampered asset is rejected before use");
    std::filesystem::remove_all(package);

    juce::RSAKey publicKey, privateKey;
    juce::RSAKey::createKeyPair(publicKey, privateKey, 512);
    auto signedRequest = requestFor("Signed Bass"); signedRequest.manifest.instrument = "bass";
    signedRequest.signerId = "release-test"; signedRequest.encodedPrivateKey = privateKey.toString().toStdString();
    const auto signedPackage = root / "signed.ntone";
    tests.expect(nts::ecosystem::exportTonePackage(signedPackage, signedRequest, error), "signed package exports: " + error);
    nts::ecosystem::PackageValidationPolicy signedPolicy;
    signedPolicy.allowUnsigned = false;
    signedPolicy.trustedSigners.push_back({ "release-test", publicKey.toString().toStdString() });
    validation = nts::ecosystem::validateTonePackage(signedPackage, signedPolicy);
    tests.expect(validation.valid && validation.signatureVerified, "trusted RSA package signature verifies: " + validation.error);

    const auto modelPath = root / "model.bin"; const auto modelBytes = tinyModel();
    { std::ofstream stream(modelPath, std::ios::binary); stream.write(reinterpret_cast<const char*>(modelBytes.data()),
                                                                      static_cast<std::streamsize>(modelBytes.size())); }
    const auto first = std::tanh(0.05f); const auto secondValue = std::tanh(-0.1f + 0.1f * first);
    const auto vectorPath = root / "model-test.json";
    { std::ofstream stream(vectorPath); stream << "{\"schemaVersion\":1,\"input\":[0.1,-0.2],\"expected\":["
                                                << first << ',' << secondValue << "],\"tolerance\":0.00001}"; }
    auto modelRequest = requestFor("Captured Model"); modelRequest.modelFile = modelPath;
    modelRequest.modelTestFile = vectorPath; modelRequest.manifest.modelOperators = { "tanh-rnn" };
    const auto modelPackage = root / "model.ntone";
    tests.expect(nts::ecosystem::exportTonePackage(modelPackage, modelRequest, error), "model package exports with test vector: " + error);
    validation = nts::ecosystem::validateTonePackage(modelPackage);
    tests.expect(validation.valid, "packaged model schema, memory, operator, hash, and embedded vector validate: " + validation.error);
    auto missingVectorRequest = requestFor("Missing Test"); missingVectorRequest.modelFile = modelPath;
    tests.expect(! nts::ecosystem::exportTonePackage(root / "missing-test.ntone", missingVectorRequest, error),
                 "model package export refuses to omit its embedded test vector");

    const auto sourcePackage = root / "source.ntone";
    tests.expect(nts::ecosystem::exportTonePackage(sourcePackage, requestFor("Favorite Modern"), error),
                 "profile package exports: " + error);
    nts::ecosystem::ProfileLibrary library(root / "library");
    tests.expect(library.importPackage(sourcePackage, error), "profile package imports: " + error);
    auto profiles = library.search({ .searchText = "modern", .instrument = "guitar" });
    tests.expectEqual(profiles.size(), std::size_t { 1 }, "profile browser searches metadata and instrument");
    if (! profiles.empty())
    {
        tests.expect(library.setFavorite(profiles[0].manifest.packageId, true, error), "profile can be favorited: " + error);
        tests.expect(library.markUsed(profiles[0].manifest.packageId, error), "profile last-used timestamp persists: " + error);
        profiles = library.search({ .favoritesOnly = true });
        tests.expectEqual(profiles.size(), std::size_t { 1 }, "favorite filter returns favorite profiles");
    }

    nts::ecosystem::UpdateManifest update;
    update.version = "0.11.0"; update.downloadUrl = "https://updates.tubeforge.invalid/TubeForge.exe";
    update.sha256 = std::string(64, 'a'); update.sizeBytes = 1000; update.minimumOs = "Windows 10";
    update.signerId = "release-test"; update.signature = signUpdate(update, privateKey);
    auto decision = nts::ecosystem::evaluateUpdate(update, nts::ecosystem::RuntimeMode::standalone, "0.10.0",
                                                   signedPolicy.trustedSigners);
    tests.expect(decision.validManifest && decision.updateAvailable && decision.offerAfterShutdown,
                 "standalone accepts a trusted update only for post-shutdown installation");
    decision = nts::ecosystem::evaluateUpdate(update, nts::ecosystem::RuntimeMode::plugin, "0.10.0",
                                              signedPolicy.trustedSigners);
    tests.expect(! decision.updateAvailable, "plug-in never self-updates inside a host");

    nts::ecosystem::CrashContext crash;
    crash.reportId = "report-test"; crash.timestampUtc = "2026-07-18T00:00:00Z";
    crash.stackTrace = "at C:\\Users\\artist\\Songs\\secret.wav";
    crash.recentEvents = { "loaded C:\\Users\\artist\\Presets\\Client.ntone" };
    const auto report = nts::ecosystem::crashReportJson(crash, false);
    tests.expect(report.find("secret.wav") == std::string::npos && report.find("Client.ntone") == std::string::npos,
                 "crash report redacts source and preset paths");
    tests.expect(report.find("path-redacted") != std::string::npos, "crash report labels redacted diagnostics");

    nts::ecosystem::TelemetrySettings telemetry;
    nts::ecosystem::TelemetryRecord event;
    event.timestampUtc = "2026-07-18T00:00:00Z"; event.fields["applicationVersion"] = "0.10.0";
    tests.expect(! nts::ecosystem::telemetryJson(telemetry, event, error), "telemetry is off by default");
    telemetry.optedIn = true; telemetry.anonymousInstallationId = "anonymous-test";
    tests.expect(nts::ecosystem::telemetryJson(telemetry, event, error).has_value(), "opted-in schema emits allowlisted metadata");
    event.fields["audioFile"] = "song.wav";
    tests.expect(! nts::ecosystem::telemetryJson(telemetry, event, error), "telemetry rejects audio or source filename fields");

    std::filesystem::remove_all(root);
    return tests.result();
}
