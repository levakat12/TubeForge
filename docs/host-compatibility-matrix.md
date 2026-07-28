# Host compatibility matrix

| Host / wrapper | Status | Evidence |
|---|---:|---|
| JUCE VST3 host | Automated | Scan, instantiate, mono/stereo processing, rates/buffers, state, automation, bypass/latency, multiple instances, repeated editor lifecycle |
| REAPER x64 | Manual release gate | Offline render, automation, state reload, multi-instance, freeze/unfreeze, bypass, mono/stereo, 44.1–192 kHz |
| Steinberg VST3PluginTestHost | Manual release gate | Scan/validator, processing, state and bus layouts |
| TubeForge standalone | Automated + manual | Device setup, processing and persistence; ASIO registration compiled/tested separately |

The external-host rows require locally installed third-party hosts and are not claimed as passing until the
release checklist records host version, OS build, sample rates, buffer sizes, and results. Run
`scripts/validate-hosts.ps1` with `TUBEFORGE_PLUGINVAL` for an additional pluginval pass.
