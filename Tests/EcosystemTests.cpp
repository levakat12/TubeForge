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

// Fixed key material. Generating a 2048-bit pair per run costs seconds, and a
// pinned pair keeps signature tests deterministic. Test-only: never ship these.
constexpr const char* testPublicKey2048 =
    "10001,a8b1dab8240912250af2f384b7f0555e51cca6b3b9a073476fbada3c1d3564b331a9fc5dc2deff54caf9b37d3d38559b"
    "ad60b59c9c0164036b7f87e04c447b361f7f8706a0c8532fb89f0d043e828dfe1e0519f42f41e75d56872a34e7555e49950117"
    "3df5c0bcc7f7d0bc0dc69b48b0e8fbab624c6d025bb4e29ccc0177211d31df999f4b85d000162e4b58d6754c2bc5452a66c36f"
    "6de6d8d8485c8b852d4d6a15750b32330c0b8372cdffa9d1f08dddbb23214ec5b05619f5c6af8ab32713e7cb33ef9fb453694f"
    "7423e7784f931541fe563c86a36f99a5463ed277e2a154e191353fc1d55920f4e09cfb0094c86f3a276c9070ce57ceb9785dda"
    "2a36f177";
constexpr const char* testPrivateKey2048 =
    "48d4703f2b9f96ebd32c4ca231df5a9bdbe8e4d796fe848684e040b3cf506be7493bc02ea2a12fcee5caa4091fd320729389bb"
    "1e9fe3a1d0302ae0f89f34b9950648427d4410f88913c5e712bc0215576e92794008df050a30ff2de3a5341ffed88b9e032c04"
    "47fc752af2e567a746dcc9d94d86a4c4334e706b9d921dd7d6bd5656413b8a0fe72996689722f74b4b6c0aad46895cf1957a0d"
    "95ddafa483ecfaba1e29ff1ea00a44cf29f15533d11621e0a9ce9baf20cea51231df6c336ddb1ec2b9ee7409310151badc296b"
    "b269d617c3588350affdd34f553d270cab5eb19abdd7a3e31c73307632ba1e3009a13fdb6930b627dba204ad121841527144f3"
    "51,"
    "a8b1dab8240912250af2f384b7f0555e51cca6b3b9a073476fbada3c1d3564b331a9fc5dc2deff54caf9b37d3d38559bad60b5"
    "9c9c0164036b7f87e04c447b361f7f8706a0c8532fb89f0d043e828dfe1e0519f42f41e75d56872a34e7555e499501173df5c0"
    "bcc7f7d0bc0dc69b48b0e8fbab624c6d025bb4e29ccc0177211d31df999f4b85d000162e4b58d6754c2bc5452a66c36f6de6d8"
    "d8485c8b852d4d6a15750b32330c0b8372cdffa9d1f08dddbb23214ec5b05619f5c6af8ab32713e7cb33ef9fb453694f7423e7"
    "784f931541fe563c86a36f99a5463ed277e2a154e191353fc1d55920f4e09cfb0094c86f3a276c9070ce57ceb9785dda2a36f1"
    "77";
constexpr const char* testPrivateKey1024 =
    "bd993a347443cf3d7f1db6950915e16631decedfdabd75c25033a3047c9ef1e348b446c7eeebcc64035959360b25dea45dab10"
    "afa44e90b3606aeb0a9d1d6a5a9da3f2e7629cd1114d1d55b71814e56180b41234613e4c268d81b0867e62640e9a7e88cf9537"
    "9d4a9c95ede567eb0be8e812a7a905de664324b327e8a7d2c281,"
    "f9e4d0b9bf413cfc0ee299d952c7efab6b02896f70d1a56d86fb22c88a13e2c4b961cbf5fac9d5c83096660498200304a7e071"
    "27d552177212fa84f0a8947a4b0ad9d56076c132b0e0a12e02714f166c72666a516c39b656b1d1169dad013b5b76c690a00e93"
    "9b3a0b2c10b4632b50a15199cc10f2dda50ffe9b775b1b61a60d";

std::string signUpdate(const nts::ecosystem::UpdateManifest& manifest)
{
    std::string error;
    return nts::ecosystem::signCanonicalText(
        nts::ecosystem::updateManifestJson(manifest, false, false), testPrivateKey2048, error);
}

// The scheme this replaced: a bare SHA-256 digest handed straight to the RSA
// primitive with no padding. JUCE picks e=3 for most generated keys, which makes
// an unpadded 256-bit digest inside a 2048-bit modulus recoverable by integer
// cube root, so signatures could be forged without the private key at all.
std::string rawDigestSignature(std::string_view canonical, const char* encodedPrivateKey)
{
    juce::RSAKey key { juce::String(encodedPrivateKey) };
    juce::BigInteger value;
    value.parseString(juce::SHA256(canonical.data(), canonical.size()).toHexString(), 16);
    key.applyToValue(value);
    return value.toString(16).toStdString();
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

    auto signedRequest = requestFor("Signed Bass"); signedRequest.manifest.instrument = "bass";
    signedRequest.signerId = "release-test"; signedRequest.encodedPrivateKey = testPrivateKey2048;
    const auto signedPackage = root / "signed.ntone";
    tests.expect(nts::ecosystem::exportTonePackage(signedPackage, signedRequest, error), "signed package exports: " + error);
    nts::ecosystem::PackageValidationPolicy signedPolicy;
    signedPolicy.allowUnsigned = false;
    signedPolicy.trustedSigners.push_back({ "release-test", testPublicKey2048 });
    validation = nts::ecosystem::validateTonePackage(signedPackage, signedPolicy);
    tests.expect(validation.valid && validation.signatureVerified, "trusted RSA package signature verifies: " + validation.error);
    tests.expectEqual(validation.manifest.signatureAlgorithm, std::string(nts::ecosystem::signatureAlgorithmId),
                      "signed packages record the padded signature algorithm");

    {
        // Padding is what makes the signature unforgeable, so prove the padded
        // path accepts nothing that the old unpadded scheme would have accepted.
        const auto canonical = nts::ecosystem::packageManifestJson(validation.manifest, false, false);
        const auto& good = validation.manifest.signature;
        std::string signatureError;

        tests.expect(nts::ecosystem::verifyCanonicalText(canonical, good, testPublicKey2048, signatureError),
                     "reference signature verifies: " + signatureError);
        tests.expectEqual(good.size(), std::size_t { 512 },
                          "signature is the fixed 2048-bit modulus width");
        auto raw = rawDigestSignature(canonical, testPrivateKey2048);
        tests.expect(! nts::ecosystem::verifyCanonicalText(canonical, raw, testPublicKey2048, signatureError),
                     "unpadded raw-digest signature is rejected");
        // Widened to the canonical size so the rejection comes from the padding
        // comparison itself rather than from the fixed-width length check.
        if (raw.size() < 512) raw.insert(0, std::string(512 - raw.size(), '0'));
        tests.expect(! nts::ecosystem::verifyCanonicalText(canonical, raw, testPublicKey2048, signatureError),
                     "width-corrected raw-digest signature is still rejected");
        tests.expect(! nts::ecosystem::verifyCanonicalText(canonical, "00" + good, testPublicKey2048, signatureError),
                     "zero-padded wider signature is rejected");
        tests.expect(! nts::ecosystem::verifyCanonicalText(canonical, good.substr(2), testPublicKey2048, signatureError),
                     "truncated signature is rejected");
        tests.expect(! nts::ecosystem::verifyCanonicalText(canonical + " ", good, testPublicKey2048, signatureError),
                     "signature does not cover a modified manifest");
        tests.expect(! nts::ecosystem::verifyCanonicalText(canonical, std::string(512, '0'),
                                                           testPublicKey2048, signatureError),
                     "zero signature is rejected");

        std::string shortKeyError;
        tests.expect(nts::ecosystem::signCanonicalText(canonical, testPrivateKey1024, shortKeyError).empty(),
                     "signing refuses a modulus below 2048 bits");
    }

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
    update.signerId = "release-test"; update.signature = signUpdate(update);
    auto decision = nts::ecosystem::evaluateUpdate(update, nts::ecosystem::RuntimeMode::standalone, "0.10.0",
                                                   signedPolicy.trustedSigners);
    tests.expect(decision.validManifest && decision.updateAvailable && decision.offerAfterShutdown,
                 "standalone accepts a trusted update only for post-shutdown installation");
    decision = nts::ecosystem::evaluateUpdate(update, nts::ecosystem::RuntimeMode::plugin, "0.10.0",
                                              signedPolicy.trustedSigners);
    tests.expect(! decision.updateAvailable, "plug-in never self-updates inside a host");

    {
        // A manifest that keeps a valid signature but renames the algorithm must
        // not slip through, or the padded scheme could be downgraded in transit.
        auto downgraded = update; downgraded.signatureAlgorithm = "rsa-sha256-raw-v1";
        decision = nts::ecosystem::evaluateUpdate(downgraded, nts::ecosystem::RuntimeMode::standalone,
                                                  "0.10.0", signedPolicy.trustedSigners);
        tests.expect(! decision.validManifest, "update signature algorithm cannot be downgraded");

        auto forged = update; forged.version = "9.9.9";
        decision = nts::ecosystem::evaluateUpdate(forged, nts::ecosystem::RuntimeMode::standalone,
                                                  "0.10.0", signedPolicy.trustedSigners);
        tests.expect(! decision.validManifest, "update signature covers the advertised version");

        std::string parseError;
        tests.expect(! nts::ecosystem::parseUpdateManifest(
                         nts::ecosystem::updateManifestJson(downgraded, true, false), parseError).has_value(),
                     "parsing rejects an unsupported update signature algorithm");
    }

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
