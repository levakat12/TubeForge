# TubeForge

TubeForge is a C++20 VST3/standalone guitar and bass amplifier built with JUCE. The signal path is deliberately modular so trained amp models, tone stacks, cabinet impulse responses, and quality modes can evolve independently.

## Current signal path

```text
input trim -> 25 Hz DC/high-pass -> recurrent nonlinear model
           -> bass/mid/treble/presence EQ -> dry/wet mix
           -> output trim -> safety soft clip
```

The recurrent core is an allocation-free, stateful neural-network inference path. Its embedded starter weights are provisional voicing weights, not a captured amplifier. A training/export pipeline and atomic model swapping are the next ML milestone.

## Build on Windows

Requirements:

- Visual Studio 2022 Build Tools with Desktop development with C++
- CMake 3.22+
- Git

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

JUCE is fetched at the pinned `8.0.13` tag during configuration. Build products are placed below `build/TubeForge_artefacts/`. Automatic installation to the system VST3 folder is intentionally disabled.

## Real-time rules

- No allocation, file I/O, locks, or model parsing on the audio thread.
- Every channel owns its recurrent and filter state.
- Parameters are host-automatable; gain, drive, and mix changes are smoothed.
- Model loading will prepare immutable model state off-thread and swap it at a block boundary.

## Roadmap

1. Define a versioned model format and Python capture/training/export tools.
2. Add off-thread model loading and validation with a known-safe fallback model.
3. Add oversampling around the nonlinear core and report exact host latency.
4. Add cabinet IR convolution and guitar/bass factory voicings.
5. Replace the generic editor with the production UI and add preset management.
6. Validate with `pluginval`, multiple DAWs, sample rates, and block sizes.

## Licensing note

JUCE is dual-licensed. Confirm that the JUCE license selected for distribution matches the way the finished plug-in will be released.
