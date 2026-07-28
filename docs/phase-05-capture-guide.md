# Phase 5 capture and activation guide

From the repository root, make `ml` importable and initialize a capture:

```powershell
$env:PYTHONPATH = "ml"
python ml/scripts/capture_wizard.py init D:\captures\amp-a `
  --target-type amplifier --target-name "Amp A channel 1" `
  --sample-rate 48000 --input-channel 0 --return-channel 1 `
  --expected-rms-db -21 --duration 120 --acknowledge-feedback-risk
```

The wizard writes logarithmic sweep, impulse, multitone, and stepped-noise files under
`prepared-input`. Record those plus the four musical categories listed in `musical-takes.json` through
the target. Put dry files in `session/input`, returned files with identical stems in `session/output`,
and record silence, sweep, and impulse calibration files. Never route the target return back into its
send.

Inspect safety, alignment, drift, polarity, and signal quality:

```powershell
python ml/scripts/capture_wizard.py inspect D:\captures\amp-a
```

Train on separate performances and validate on unseen sessions:

```powershell
python ml/scripts/capture_wizard.py train D:\captures\amp-a `
  --config ml/configs/phase5-lstm.toml `
  --train-session D:\captures\train-a --train-session D:\captures\train-b `
  --validation-session D:\captures\validation-a
```

The command prints the artifact and complete quality report. Activate its immutable pointer after
reviewing the report:

```powershell
python ml/scripts/capture_wizard.py activate D:\captures\amp-a D:\captures\amp-a\models\RUN\artifact
```

In TubeForge, open **Neural Capture**, choose the artifact directory, and switch Engine mode to
**Neural capture** after the status reads Ready. The loader checks SHA-256 and test-vector parity on a
worker. Use Model, Bypass DI, or Loudness matched monitoring. Input compensation remains opt-in; fix
recording gain when practical instead of relying on compensation.
