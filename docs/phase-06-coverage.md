# Phase 6 coverage

Overall implementation coverage: **91%** of the Phase 6 plan.

The percentage is a requirement-weighted implementation estimate, not a claim of analog hardware accuracy.
Automated tests verify software behavior; no physical amplifier measurement set was supplied for hardware
equivalence testing.

| Plan area | Coverage | Evidence |
|---|---:|---|
| Typed circuit graph and stable schema | 100% | All ten node types, stable IDs, typed connections/ports, JSON round-trip, v0 migration |
| Immutable graph compilation/publication | 100% | Validation, cycle handling, channel resolution, latency, preallocation, ordering, prepare, atomic block-boundary swap, crossfade |
| Component library and tube identity | 95% | Electrical tube values and reusable filter/triode/tone/PI/power/NFB/transformer/cabinet components |
| Triode backends and operating point | 94% | Static, gray-box, bounded numerical, neural surrogate; plate current, headroom, gain, balance, clipping and harmonic estimates |
| Passive tone stack | 90% | Capacitor/resistor/pot parameters, coefficient generation, response query, graph-swap automation; initial solver is a stable gray-box reduction |
| Phase inverter, power, supply, feedback, transformer | 91% | Stateful bounded models and all planned simplified topologies; no expensive hysteresis or general implicit-loop solver |
| Hybrid neural component | 100% | Common physical interface, model/rate validation, arbitrary blocks, zero-latency allocation-free inference |
| Simple mode | 100% | Eight macros clamp into a valid ten-node graph; plugin knobs rebuild the physical engine off-thread |
| Engineering UI | 78% | Schematic, component summary, response view, advanced values, stage levels, validity/compiler warnings; transfer and harmonic plots are estimates rather than a full lab analyzer |
| Validation | 94% | Missing endpoints, IDs, ports, ranges, tubes, neural assets/rate, forward cycles, feedback target/amount, reachability and cabinet path |
| Tests and acceptance | 88% | Compile, tube substitution, bounds, migration, state reset, sag, rate rejection, graph swap, exact save/restore, hybrid, telemetry and allocation tests |

## Acceptance status

- Valid amplifier graphs can be constructed through the public component graph API.
- Tube substitution changes nonlinear/dynamic processing.
- Invalid graphs are rejected before publication.
- Graph swaps use a tested 2048-sample crossfade.
- Physical graph parameters round-trip exactly with stable IDs.
- Simple macros clamp into supported component ranges.
- A packed neural component runs through the real-time component interface.
- UI telemetry is copied from atomic snapshots and does not inspect component state directly.

## Deliberately incomplete

- The first release uses a fixed plugin topology; arbitrary drag-and-drop graph editing is API-ready but
  not exposed as a visual patch editor.
- The numerical backend is a bounded reduced equation, not a full WDF/SPICE solver.
- Arbitrary implicit feedback networks and expensive transformer hysteresis are outside this iteration.
- No licensed amplifier schematics, bench captures, or reference circuit-solver corpus were provided, so
  measured hardware-equivalence, listening validation, and generalized accuracy remain unclaimed.
