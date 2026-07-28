# Release process

1. Freeze features, update independent versions and compatibility matrix, and run Debug/Release CI.
2. Run functional tests, Python parity, timing tests in isolation, one-hour soak, and external host matrix.
3. Build `nts_release_bundle`; inspect `SHA256SUMS.txt`, symbols, VST3, standalone, documentation, and privacy text.
4. Build the Inno Setup installer with `packaging/windows/build-installer.ps1 -RequireSigning`. Release signing
   requires `TUBEFORGE_SIGNTOOL` and `TUBEFORGE_CERT_SHA1`; CI pull-request artifacts remain explicitly unsigned.
5. Verify Authenticode, install/repair/uninstall in a clean Windows VM, rescan both target hosts, and smoke-test audio.
6. Publish the installer, detached hashes, symbols to restricted storage, release notes, known issues, and rollback link.
7. Roll out internal → private beta → public beta → stable. Stop rollout on crash, state-loss, audio-corruption,
   signature, or compatibility regressions. The VST3 never updates itself; standalone only offers a verified
   update after audio shutdown.

macOS packaging/notarization is deferred by project decision. Community sharing is file-based in format 1;
future server discovery/moderation/reputation may be added without changing the local package trust boundary.
