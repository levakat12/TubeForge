# Phase 3 traditional amp architecture

## Runtime graph

`nts_amp` is a reusable C++20 library built on the allocation-free Phase 2 DSP layer. The same
`TraditionalAmpProcessor` is used by the VST3, standalone application, automated audio tests, and
offline renderer.

```text
profile-aware calibration meter -> manual trim -> gate
    -> pre-EQ/tightness -> 2..4 responsive oversampled preamp stages
    -> coupled/active/bass tone stack -> phase inverter -> sagging power amp
    -> presence/resonance -> dual-IR cabinet -> post EQ -> loudness match
```

Guitar presets use stronger pre-distortion low cuts, more nonlinear stages, and a cabinet by
default. Bass presets use a phase-aligned Linkwitz-Riley split: the low path remains clean or
compressed while the high path passes through the driven amp and cabinet graph. The low path can
be mono-compatible, lightly saturated, and blended independently.

## Responsive nonlinear stages

Each `ResponsivePreampStage` owns pre/post filtering, smoothed gain and trim, prepared 1x/2x/4x/8x
polyphase oversamplers, envelope and bias memories, high-frequency emphasis state, transient gain
reduction, asymmetric transfer, and slow recovery. All oversampling modes are prepared up front so
automation does not allocate on the audio thread.

The phase inverter applies frequency shaping, differential imbalance, limited asymmetric headroom,
and local feedback. The power stage maintains a bounded virtual supply from 0.32 to 1.0; signal
energy lowers its available headroom and independently configurable attack/recovery constants bring
the supply back after a transient. Presence and resonance operate around this level-dependent stage.

## Cabinets and presets

`CabinetSection` accepts two mono or stereo IRs with independent mic metadata, cross-blends them,
supports polarity reversal and sample delay alignment, applies cabinet low/high cuts, and offers
bypass and bass-DI blend. The built-in original IRs make the product usable without external assets;
the public load API accepts responses decoded by the Phase 2 asynchronous IR loader.

Two complete original topology families are included: **Tight Modern** and **Vintage Bloom**, each
with guitar and bass variants and its own input calibration profile. Presets serialize with schema
version 1 and are loaded into an inactive amp voice; the old and new graphs then run in parallel
during an allocation-free output crossfade.

## Product integration

The VST3 parameter tree exposes a simple page (gain, bass, mid, treble, presence, resonance, master,
cabinet, instrument) and an advanced page (per-stage gain, bias, filtering, oversampling, sag,
feedback, crossover, clean blend, cabinet alignment, tightness, pick emphasis, and topology).
Parameters are host-automatable and their values are included in project state. The graph reports
the nonlinear-stage latency and shares the Phase 1 diagnostics and wrapper infrastructure.
