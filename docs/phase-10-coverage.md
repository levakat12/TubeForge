# Phase 10 coverage

Windows-first Phase 10 implementation coverage is **86%**. macOS packaging/notarization is excluded from this
percentage because it was explicitly deferred. Coverage measures delivered, locally verifiable behavior—not
lines of code or document count.

| Area | Weight | Covered | Status |
|---|---:|---:|---|
| `.ntone` format and hostile-input security | 25% | 24% | Format 1, UUID/compatibility/assets, bounded allowlist, SHA-256, trusted RSASSA-PKCS1-v1_5 signatures (2048-bit minimum), rig/model/operator/memory/embedded-vector validation, no execution |
| Profile browser and file-based sharing | 15% | 14% | Search, instrument/favorite filters, author/tags, compatibility/trust warnings, import/export/apply, favorites and last-used state; gain-category and sample-rate filters exist in the API but not yet as UI controls |
| Versioning, licenses and privacy | 12% | 12% | Independent matrix, IR redistribution gate, no source audio, opt-in allowlisted telemetry, sanitized diagnostics, policies/notices |
| Windows distribution and update path | 15% | 10% | Reproducible staged VST3/standalone/docs/PDB/hash bundle, component installer source, repair/uninstall/collector, signed-manifest standalone policy; no production certificate or network downloader exercised locally |
| Crash handling | 8% | 6% | Local standalone exception capture and privacy-safe report schema; host-owned plugin crash interception and a consented upload service are not implemented |
| CI/CD and long-term quality | 10% | 9% | Debug/Release, C++/Python/parity/package/wrapper/performance tests, dependency audit/updates, artifacts/reports/symbols/installer, scheduled hour soak |
| Host compatibility | 7% | 3% | Automated real VST3 scan/instantiate/state/automation/rates/buffers/multi-instance/editor lifecycle plus manual runner; REAPER and VST3PluginTestHost release evidence still requires those third-party hosts |
| Guides, release process and community foundation | 8% | 8% | User/developer/research/package/release/host/privacy/security docs and server-independent local sharing |

## Verified in this implementation

- Optimized Release build produces standalone and VST3 targets.
- Sixteen functional CTest suites pass together; ML runtime and DSP timing gates pass in isolated runs.
- The release staging target produces binaries, documentation, separate symbols, and `SHA256SUMS.txt`.
- PowerShell packaging/host scripts parse successfully.

## Remaining release gates

1. Supply a production Authenticode certificate and compile/verify the Inno Setup installer (`ISCC` is not
   installed on the development machine; CI builds an unsigned test installer).
2. Install/repair/uninstall on clean Windows machines and record REAPER plus VST3PluginTestHost results.
3. Implement the optional standalone download/rollback transport around the existing signed update decision.
4. Add consent UI/backend if crash reports or opt-in telemetry are ever uploaded.
5. Complete a release-candidate one-hour soak; CI is configured to run it weekly/on demand.
