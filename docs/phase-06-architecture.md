# Phase 6 physical and hybrid circuit architecture

Phase 6 adds `nts_circuit`, which is separate from the Phase 3 fixed traditional amplifier. Its editable
description is a value graph made from stable node, model, backend, parameter, and connection IDs. No C++
class names enter a preset.

## Compile and publication boundary

The editor produces a `CircuitGraphDescription`. A `std::jthread` validates it and compiles an immutable
runtime outside the audio callback. Compilation checks IDs, ports, parameters, tube definitions, feedback
edges, cycles, channel policy, reachability, cabinet placement, and neural-model sample rate. It then orders
the DAG, creates components, allocates every node buffer, prepares state, and calculates path latency.

`CircuitProcessor` owns two runtime slots and independent mono state for up to two channels. A completed
graph is published with an atomic slot index at a block boundary. Old and new graphs run in parallel during
a 2048-sample linear crossfade. An inactive slot cannot be replaced while it is still the crossfade source.
Processing performs no graph mutation, locking, file access, or heap allocation.

Explicit feedback connections are restricted to feedback nodes and removed from the DAG schedule. The
supported Phase 6 feedback implementation is a component-local, one-sample-delayed loop. That delay adds
phase shift near Nyquist; the amount is bounded to 0.92. Arbitrary implicit circuit loops are rejected.

## Component model

All components implement prepare, reset, in-place process, latency, state-size, and atomically copied
telemetry contracts. Parameter schemas carry units and valid ranges. The initial library contains:

- RC filtering and linear I/O;
- electrical tube definitions for 12AX7, 12AT7, 12AU7, 6V6, and EL34 families;
- static transfer, stateful gray-box, bounded three-iteration numerical, and neural-surrogate triode paths;
- a component-value-derived passive three-band network;
- a differential/asymmetric phase-inverter approximation;
- single-ended, push-pull class-A-like, and push-pull class-AB-like power behavior;
- bounded supply attack/recovery, current draw, sag, rectifier stiffness, and optional ripple;
- local negative feedback/presence;
- an output transformer with low-frequency flux memory, saturation, leakage rolloff, turns ratio validity,
  and damping;
- a reactive cabinet approximation.

`PackedNeuralComponentModel` adapts the Phase 5 packed runtime to `INonlinearComponentModel`. Physical
controls are passed through the same interface, the model sample rate is checked at compile time, and its
audio path remains allocation-free.

## UI and telemetry

Physical Circuit is the third engine mode. Amp controls provide simple macros; the Advanced tab supplies
engineering controls that are mapped into bounded component ranges. Changes request a new worker compile.
The Circuit Engineering tab shows the compiled chain, component-value summary, response visualization,
per-stage level/validity, and compiler warnings.

Components write only atomics during processing. The timer reads a copied `NodeTelemetry` vector, so the UI
does not dereference mutable audio-thread state.

## Modelling boundary

This engine is a stable virtual approximation for tone design. It is not a SPICE replacement, electrical
safety guide, or proof that virtual tube, transformer, voltage, bias, or load substitutions are safe in
physical hardware. Hardware equivalence requires separately licensed reference data and controlled lab
measurement.
