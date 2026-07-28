# Privacy policy for TubeForge

TubeForge processes audio locally. Telemetry is off by default and produces no record until the user explicitly
opts in. The allowlisted telemetry schema can contain an anonymous installation ID, app/runtime/OS version,
coarse uptime, package-format result, crash occurrence, and update acceptance. It cannot contain audio,
waveforms, source or song names, filesystem paths, project names, preset contents, profile contents, email, or
account identity.

Crash reports contain app version, standalone/plugin mode, host and OS, model ID, graph schema, exception code,
sanitized stack trace, and bounded recent diagnostic event names. Path and identity patterns are redacted.
Diagnostics are shown locally before any future upload; no upload endpoint is implemented in 0.10.0.
User-provided analysis/reconstruction audio remains in local processing/cache and is never included in `.ntone`
exports. Deleting TubeForge's application-data directory removes local logs, caches, preferences, and profiles.
