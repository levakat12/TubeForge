# Phase 1 architecture

## Thread ownership

The audio callback owns only fixed storage, per-instance engine history, and atomic reads/writes. It calls `CoreEngine::process(AudioProcessContext&) noexcept`, publishes meter values, pushes compact `AudioEvent` values to an SPSC ring, and records callback timing. It never uses the project serializer, logger, file system, worker queue, or UI objects.

The UI thread edits JUCE host parameters, opens file choosers, validates project state, samples process memory, and polls downsampled meter/diagnostic atomics at 20 Hz. The diagnostics logger converts compact audio events to structured messages outside the callback.

Background workers own file decoding and future model/IR preparation. Jobs receive a `std::stop_token`; exceptions are caught at the worker boundary and surfaced through a failure handler.

## Processing contract

`nts::audio::IAudioProcessor` is JUCE-independent. A caller prepares it with a sample rate, maximum block size, and channel counts, then supplies non-owning `std::span` channel views through `AudioProcessContext`. `process()` is `noexcept`.

Simple controls cross the thread boundary through `RuntimeParameters` atomics. Larger future graphs use `ImmutableSnapshotExchange`: the UI publishes an immutable object, audio readers hold a non-allocating read guard, and retired objects are reclaimed only by the owning non-audio thread.

## State activation

Project files use schema version 2 and contain `applicationVersion`, `engine`, `device`, `graph`, `ui`, and `assets`. The graph section now persists the complete selected physical-circuit JSON in addition to latency and enabled state. Deserialization follows this order:

1. Parse JSON without changing live state.
2. Reject unknown future schema versions.
3. Run the explicit v0-to-v1 migration when required.
4. Validate numeric ranges and project-relative asset paths.
5. Apply parameters only after complete validation.

An invalid state therefore leaves the last known good live state untouched.

## Device ownership

The custom JUCE standalone application owns the `AudioDeviceManager`, `AudioProcessorPlayer`, and persistent device settings. Its embedded **Audio settings** tab hosts `AudioDeviceSelectorComponent`, covering device enumeration, channel maps, rates, buffer sizes, close/reopen/re-prepare, and fallback. VST3 delegates the same responsibilities to the host. WASAPI is compiled by default on Windows; ASIO is an explicit SDK-backed build option; CoreAudio is selected by JUCE on macOS builds.

## Latency

`LatencyBudget` tracks host, oversampling, convolution, neural receptive-field, lookahead, and resampling latency separately. Diagnostics display the processing total. Only processing latency is reported to a plug-in host; the host already knows its own buffer latency.
