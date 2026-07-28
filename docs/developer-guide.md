# TubeForge developer guide

TubeForge is a C++20 CMake project built on JUCE 8. Engine libraries are wrapper-independent; the VST3 and
standalone targets share `Source/PluginProcessor.*`. Never allocate, lock, perform file/network work, parse
JSON, log, or destroy heavyweight state in `processBlock`. Publish prepared immutable state at a block
boundary and crossfade audible transitions.

Configure and validate on Windows:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
cmake --build build --config Release --target nts_release_bundle
```

Tests cover unit/integration DSP, IRs, amp, neural parity/performance, circuit, analysis, reconstruction,
assistant, package security, wrappers, and a real JUCE VST3 scan/instance/process/state/editor lifecycle.
Timing acceptance tests should also be run individually on an otherwise idle machine. Python tests require
`python -m pip install -e ml`.

Format changes require an independent version, bounds validation, migration or explicit rejection, fixtures,
and compatibility-matrix update. A package parser must use the allowlist and limits in
`engine/ecosystem`; do not extract arbitrary archive paths. New telemetry fields require privacy review and
must fit the compile-time allowlist. Pull requests should include tests and must pass Debug and Release CI.
