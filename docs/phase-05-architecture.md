# Phase 5 neural amplifier capture architecture

## End-to-end path

The developer capture wizard creates calibrated excitation material and a versioned paired session.
It refuses to validate until feedback routing is acknowledged and the Phase 4 signal, calibration,
alignment, drift, polarity, and missing-output checks pass. Training always receives separate training
and validation sessions. The best checkpoint is evaluated, packed with test vectors and calibration
metadata, and published through an atomic activation pointer.

```text
DI and prepared excitation -> external target -> paired capture -> alignment/quality gate
    -> LSTM/GRU/TCN training -> fixed evaluation -> quality report -> packed artifact
    -> background plug-in validation -> block-boundary activation -> click-free crossfade
```

## Models and controls

`ConditionedLstm` is the primary trainable recurrent baseline. It uses input, forget, candidate, and
output gates, full truncated BPTT, a linear projection, and an optional residual path. `ConditionedGru`
is the smaller comparison and `CausalTcn` is a dilated streaming alternative with explicit receptive
field state. The comparison models train their output projection while retaining deterministic seeded
feature weights in this first version.

Every conditioned model accepts the normalized vector `gain, tone, master, channel, instrument_mode`.
Session `targetSettings.controlVector` is carried into each streaming chunk, so training and inference
use the same order. The packed v2 format stores architecture, controls, dimensions, auxiliary TCN
shape, and float32 weights.

## Real-time runtime

The C++ runtime implements tanh RNN, LSTM, GRU, and causal TCN inference without Python. All state and
scratch memory is prepared outside the callback. A worker validates the manifest schema, SHA-256,
packed dimensions, finite weights, sample rate, and exported test vector before it publishes a pending
slot. The audio thread resets the new slot and crossfades old/new output for 1024 samples.

Input calibration uses a 400 ms RMS estimate and peak tracker. A mismatch greater than 3 dB produces a
warning. Compensation is disabled by default and, when explicitly enabled, is bounded to +/-12 dB.
Host transport starts and discontinuities request deterministic recurrent-state reset.

## Sample-rate policy

Artifacts run only when their sample rate exactly matches the audio device or host. This avoids hidden
resampling and unreported latency. Train and export another artifact for every required sample rate.

## Evaluation

The automatic quality report includes waveform error, spectral convergence, loudness difference,
low-frequency error, transient error, silence noise, callback cost, parameter memory, supported sample
rate, expected input RMS, and a deterministic confidence rating. Offline comparison renders target,
model, bypass DI, loudness-matched model, and null difference. Phase 3's player supplies blinded ABX.
