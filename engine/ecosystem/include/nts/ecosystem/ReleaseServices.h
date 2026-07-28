#pragma once

#include <nts/ecosystem/TonePackage.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace nts::ecosystem
{
enum class RuntimeMode { standalone, plugin };

struct UpdateManifest
{
    int schemaVersion { 1 };
    std::string version;
    std::string downloadUrl;
    std::string sha256;
    std::uint64_t sizeBytes {};
    std::string minimumOs;
    std::string signerId;
    std::string signature;
};

struct UpdateDecision
{
    bool validManifest {};
    bool updateAvailable {};
    bool offerAfterShutdown {};
    std::string reason;
};

[[nodiscard]] std::string updateManifestJson(const UpdateManifest& manifest,
                                             bool includeSignature,
                                             bool pretty = true);
[[nodiscard]] std::optional<UpdateManifest> parseUpdateManifest(std::string_view json,
                                                                std::string& error);
[[nodiscard]] UpdateDecision evaluateUpdate(const UpdateManifest& manifest,
                                            RuntimeMode mode,
                                            std::string_view currentVersion,
                                            const std::vector<SignatureKey>& trustedSigners);

struct CrashContext
{
    std::string reportId;
    std::string timestampUtc;
    std::string applicationVersion { currentApplicationVersion };
    RuntimeMode mode { RuntimeMode::standalone };
    std::string pluginHost;
    std::string operatingSystem;
    std::string modelId;
    int graphSchemaVersion {};
    std::string exceptionCode;
    std::string stackTrace;
    std::vector<std::string> recentEvents;
    bool userConsentedToDiagnostics {};
};

[[nodiscard]] std::string sanitizeDiagnosticText(std::string_view text);
[[nodiscard]] std::string crashReportJson(const CrashContext& context, bool pretty = true);

enum class TelemetryEvent { applicationStarted, applicationExited, crash, packageImport, updateAccepted };

struct TelemetrySettings
{
    bool optedIn {};
    std::string anonymousInstallationId;
};

struct TelemetryRecord
{
    TelemetryEvent event { TelemetryEvent::applicationStarted };
    std::string timestampUtc;
    std::map<std::string, std::string, std::less<>> fields;
};

[[nodiscard]] std::optional<std::string> telemetryJson(const TelemetrySettings& settings,
                                                       const TelemetryRecord& record,
                                                       std::string& error);

struct FormatVersions
{
    std::string application { currentApplicationVersion };
    int tonePackage { tonePackageFormatVersion };
    int packedModelMinimum { 1 };
    int packedModelCurrent { 2 };
    int circuitGraph { 1 };
    int datasetSession { 1 };
    int toneAnalysis { 1 };
    int sourceSeparation { 1 };
};
} // namespace nts::ecosystem
