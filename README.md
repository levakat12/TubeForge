# TubeForge

TubeForge is a low-latency guitar and bass tone application for Windows, available as a standalone app and a VST3 plug-in. It combines traditional amplifier modelling, neural captures, a pedalboard and song-based tone matching on one real-time C++20 engine.

**Status: pre-release, not shipped.** The features below are implemented, but the installer, update and host-validation work needed for a public release is still open.

## Features

- **Amplifiers:** seven original topologies with guitar and bass variants, each with its own drawn faceplate, plus dual-IR cabinets.
- **Pedals:** sixty-two pedals across nine DSP engines on a four-slot board, browsable by character of sound.
- **Neural captures:** load Neural Amp Modeler `.nam` files and `.zip` packs directly in the plug-in, with no command line or Python needed, or train your own capture with the included tools.
- **Tone matching:** analyze a clip, or separate a song locally into stems and get several playable, editable rigs that approach its guitar or bass tone.
- **Tone assistant:** live diagnosis of input, gain, routing and tone problems, with suggested changes you can preview, accept or undo.
- **Profiles:** shareable `.ntone` packages with hash and optional signature checks, and a local library to search and manage them.
- **Local by default:** audio is processed on your machine and telemetry is off unless you turn it on. See the [privacy policy](PRIVACY.md).

Traditional modes work without any ML assets; neural features activate only after a validated model is loaded.

See the [user guide](USER_GUIDE.md) for how to use it.

## Build on Windows

Requirements:

- Visual Studio 2022 Build Tools with Desktop development with C++
- CMake 3.22+
- Git

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

JUCE is fetched at the pinned `8.0.13` tag. Build products are created below `build/TubeForge_artefacts/`; automatic installation to the system VST3 folder remains disabled.

### ASIO

WASAPI is available through the normal Windows JUCE build. To compile the ASIO variant:

```powershell
cmake -S . -B build-asio -G "Visual Studio 17 2022" -A x64 `
  -DTUBEFORGE_ENABLE_ASIO=ON
```

Set `TUBEFORGE_ASIO_SDK_PATH=C:\SDKs\asiosdk` to use an external SDK whose root contains `common\iasiodrv.h`. ASIO source is subject to Steinberg's proprietary ASIO license or GPLv3; compiling support does not by itself settle the license required for distribution.

## Repository layout

| Path | Contents |
|---|---|
| `Source/` | Plug-in processor, editor and UI pages |
| `apps/` | Standalone host |
| `engine/` | Framework-independent engine libraries: DSP, amplifier, pedals, captures, analysis, reconstruction, assistant and state |
| `ml/` | Python tools for datasets, training and model export |
| `Tests/` | Unit, integration, device, host and soak tests |
| `packaging/` | Windows release staging and installer |
| `scripts/` | Setup helpers |

## Licensing

JUCE is dual-licensed. Distribution must use a JUCE license compatible with the intended product. The Steinberg ASIO SDK has separate terms and is never fetched automatically. Asset redistribution is governed by per-package metadata and underlying terms; see [third-party notices](THIRD_PARTY_NOTICES.md).

To report a vulnerability, see the [security policy](SECURITY.md).
