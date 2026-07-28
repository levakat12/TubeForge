#include <nts/ecosystem/ReleaseServices.h>

#include <juce_cryptography/juce_cryptography.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <regex>
#include <set>
#include <sstream>

namespace nts::ecosystem
{
namespace
{
std::string property(const juce::DynamicObject& object, const char* name)
{
    return object.hasProperty(name) ? object.getProperty(name).toString().toStdString() : std::string {};
}

juce::var updateVar(const UpdateManifest& manifest, bool includeSignature)
{
    auto* root = new juce::DynamicObject();
    root->setProperty("schemaVersion", manifest.schemaVersion);
    root->setProperty("version", juce::String(manifest.version));
    root->setProperty("downloadUrl", juce::String(manifest.downloadUrl));
    root->setProperty("sha256", juce::String(manifest.sha256));
    root->setProperty("sizeBytes", static_cast<juce::int64>(manifest.sizeBytes));
    root->setProperty("minimumOs", juce::String(manifest.minimumOs));
    root->setProperty("signerId", juce::String(manifest.signerId));
    // Covered by the signature so the algorithm cannot be downgraded in transit.
    root->setProperty("signatureAlgorithm", juce::String(manifest.signatureAlgorithm));
    if (includeSignature) root->setProperty("signature", juce::String(manifest.signature));
    return juce::var(root);
}

std::optional<std::array<int, 3>> semver(std::string_view text)
{
    const auto dash = text.find('-'); if (dash != std::string_view::npos) text = text.substr(0, dash);
    std::array<int, 3> result {}; std::stringstream stream { std::string(text) }; char dot {};
    if (! (stream >> result[0] >> dot) || dot != '.' || ! (stream >> result[1] >> dot) || dot != '.'
        || ! (stream >> result[2]) || result[0] < 0 || result[1] < 0 || result[2] < 0) return std::nullopt;
    return result;
}

bool validHash(std::string_view value)
{
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](unsigned char c)
    { return std::isxdigit(c) != 0; });
}

bool verifyUpdateSignature(const UpdateManifest& manifest, std::string_view encodedPublicKey)
{
    if (manifest.signatureAlgorithm != signatureAlgorithmId || manifest.signature.empty()) return false;
    std::string error;
    return verifyCanonicalText(updateManifestJson(manifest, false, false), manifest.signature,
                               encodedPublicKey, error);
}

const char* telemetryName(TelemetryEvent event) noexcept
{
    switch (event)
    {
        case TelemetryEvent::applicationStarted: return "application-started";
        case TelemetryEvent::applicationExited: return "application-exited";
        case TelemetryEvent::crash: return "crash";
        case TelemetryEvent::packageImport: return "package-import";
        case TelemetryEvent::updateAccepted: return "update-accepted";
    }
    return "unknown";
}
} // namespace

std::string updateManifestJson(const UpdateManifest& manifest, bool includeSignature, bool pretty)
{
    return juce::JSON::toString(updateVar(manifest, includeSignature), pretty, 8).toStdString();
}

std::optional<UpdateManifest> parseUpdateManifest(std::string_view json, std::string& error)
{
    const auto parsed = juce::JSON::parse(juce::String::fromUTF8(json.data(), static_cast<int>(json.size())));
    const auto* object = parsed.getDynamicObject();
    if (object == nullptr) { error = "Update manifest root must be an object"; return std::nullopt; }
    UpdateManifest result;
    result.schemaVersion = static_cast<int>(object->getProperty("schemaVersion"));
    result.version = property(*object, "version"); result.downloadUrl = property(*object, "downloadUrl");
    result.sha256 = property(*object, "sha256");
    const auto size = static_cast<juce::int64>(object->getProperty("sizeBytes"));
    result.sizeBytes = size > 0 ? static_cast<std::uint64_t>(size) : 0;
    result.minimumOs = property(*object, "minimumOs"); result.signerId = property(*object, "signerId");
    result.signatureAlgorithm = property(*object, "signatureAlgorithm");
    result.signature = property(*object, "signature");
    if (result.signatureAlgorithm != signatureAlgorithmId)
    { error = "Update manifest uses an unsupported signature algorithm"; return std::nullopt; }
    if (result.schemaVersion != 1 || ! semver(result.version) || ! validHash(result.sha256)
        || result.sizeBytes == 0 || result.sizeBytes > 1024ULL * 1024ULL * 1024ULL
        || ! juce::URL(juce::String(result.downloadUrl)).isWellFormed()
        || ! result.downloadUrl.starts_with("https://") || result.signerId.empty() || result.signature.empty())
    { error = "Update manifest has invalid required fields"; return std::nullopt; }
    error.clear(); return result;
}

UpdateDecision evaluateUpdate(const UpdateManifest& manifest, RuntimeMode mode, std::string_view currentVersion,
                              const std::vector<SignatureKey>& trustedSigners)
{
    UpdateDecision result;
    if (mode == RuntimeMode::plugin)
    { result.reason = "Plug-in updates are delivered only through the signed installer"; return result; }
    const auto current = semver(currentVersion), available = semver(manifest.version);
    if (! current || ! available || ! validHash(manifest.sha256))
    { result.reason = "Update manifest version or payload hash is invalid"; return result; }
    const auto signer = std::find_if(trustedSigners.begin(), trustedSigners.end(), [&](const auto& value)
    { return value.signerId == manifest.signerId; });
    if (signer == trustedSigners.end() || ! verifyUpdateSignature(manifest, signer->encodedPublicKey))
    { result.reason = "Update manifest signature is not trusted"; return result; }
    result.validManifest = true; result.updateAvailable = *available > *current;
    result.offerAfterShutdown = result.updateAvailable;
    result.reason = result.updateAvailable ? "Verified update can be offered after audio shutdown"
                                           : "TubeForge is up to date";
    return result;
}

std::string sanitizeDiagnosticText(std::string_view text)
{
    auto value = std::string(text.substr(0, 8192));
    static const std::regex windowsPath(R"(([A-Za-z]:\\|\\\\)[^\s\"<>|]+)");
    static const std::regex unixPath(R"((^|\s)/(Users|home|tmp|var|mnt)/[^\s\"<>|]+)");
    static const std::regex email(R"([A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,})");
    value = std::regex_replace(value, windowsPath, "[path-redacted]");
    value = std::regex_replace(value, unixPath, "$1[path-redacted]");
    value = std::regex_replace(value, email, "[identity-redacted]");
    return value;
}

std::string crashReportJson(const CrashContext& context, bool pretty)
{
    auto* root = new juce::DynamicObject(); root->setProperty("schemaVersion", 1);
    root->setProperty("reportId", juce::String(context.reportId));
    root->setProperty("timestampUtc", juce::String(context.timestampUtc));
    root->setProperty("applicationVersion", juce::String(context.applicationVersion));
    root->setProperty("mode", context.mode == RuntimeMode::standalone ? "standalone" : "plugin");
    root->setProperty("pluginHost", juce::String(sanitizeDiagnosticText(context.pluginHost)));
    root->setProperty("operatingSystem", juce::String(sanitizeDiagnosticText(context.operatingSystem)));
    root->setProperty("modelId", juce::String(sanitizeDiagnosticText(context.modelId)));
    root->setProperty("graphSchemaVersion", context.graphSchemaVersion);
    root->setProperty("exceptionCode", juce::String(sanitizeDiagnosticText(context.exceptionCode)));
    root->setProperty("stackTrace", juce::String(sanitizeDiagnosticText(context.stackTrace)));
    root->setProperty("diagnosticConsent", context.userConsentedToDiagnostics);
    juce::Array<juce::var> events;
    for (const auto& event : context.recentEvents) events.add(juce::String(sanitizeDiagnosticText(event)));
    root->setProperty("recentEvents", events);
    root->setProperty("privacy", "No audio, project names, file paths, or preset contents are collected");
    return juce::JSON::toString(juce::var(root), pretty, 8).toStdString();
}

std::optional<std::string> telemetryJson(const TelemetrySettings& settings, const TelemetryRecord& record,
                                         std::string& error)
{
    if (! settings.optedIn) { error = "Telemetry is disabled until the user explicitly opts in"; return std::nullopt; }
    if (settings.anonymousInstallationId.empty() || settings.anonymousInstallationId.size() > 64)
    { error = "Anonymous installation id is invalid"; return std::nullopt; }
    static const std::set<std::string> allowedFields {
        "applicationVersion", "runtimeMode", "operatingSystem", "packageFormat", "result", "uptimeBucket" };
    auto* root = new juce::DynamicObject(); root->setProperty("schemaVersion", 1);
    root->setProperty("event", telemetryName(record.event));
    root->setProperty("timestampUtc", juce::String(record.timestampUtc));
    root->setProperty("anonymousInstallationId", juce::String(settings.anonymousInstallationId));
    auto* fields = new juce::DynamicObject();
    for (const auto& [key, value] : record.fields)
    {
        if (! allowedFields.contains(key) || value.size() > 80 || sanitizeDiagnosticText(value) != value)
        { delete fields; delete root; error = "Telemetry field is not permitted by the privacy schema"; return std::nullopt; }
        fields->setProperty(juce::Identifier(key), juce::String(value));
    }
    root->setProperty("fields", juce::var(fields));
    error.clear(); return juce::JSON::toString(juce::var(root), false).toStdString();
}
} // namespace nts::ecosystem
