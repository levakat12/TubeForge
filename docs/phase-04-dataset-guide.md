# Phase 4 paired-dataset guide

## Create and populate a session

From `D:\TubeForge`, with `PYTHONPATH` set to `ml`:

```powershell
python ml/scripts/create_session.py D:\captures\session-a `
  --input-device "Generic interface" --target-type plugin --target-name OriginalTarget
```

Record matching files such as `input/take-0001.wav` and `output/take-0001.wav`. Keep the dry DI and
target output at the same sample rate, bit depth, and channel count. Add `calibration/silence.wav`,
`sweep.wav`, and `impulse.wav`. Commercial brand names are not required; use neutral target names and
record rights/provenance in the required `provenance.json` file when the session will be counted as
real-corpus evidence. Start from `ml/templates/provenance.json`; include a stable performance ID,
category, creator, license/release references, the release document path, and the exact input WAV
SHA-256 for every take.

## Validate and inspect alignment

```powershell
python ml/scripts/validate_dataset.py D:\captures\session-a
python ml/scripts/alignment_report.py `
  D:\captures\session-a\reports\validation.json `
  D:\captures\session-a\reports\alignment.html
```

Training must reject a report whose `accepted` field is false. Default limits reject clipping,
missing calibration, signal gaps, undocumented inversion, confidence below 0.70, drift above 20 ppm,
or latency variation above two samples. Limits are typed and can be tightened for production data.

## Train, export, and evaluate

```powershell
python ml/scripts/train.py --config ml/configs/tiny.toml --output D:\runs `
  --session D:\captures\train-a --session D:\captures\validation-a

python ml/scripts/export_model.py D:\runs\run-id\best-checkpoint.npz D:\models\amp-a\1.0.0 `
  --license D:\captures\model-license.txt

python ml/scripts/generate_eval_clips.py D:\evaluation
python ml/scripts/evaluate.py --checkpoint D:\runs\run-id\best-checkpoint.npz `
  --dataset D:\evaluation --report D:\runs\run-id\evaluation.json
```

The deterministic evaluation generator establishes the required filenames and test signals. For a
release model, replace or supplement those inputs with rights-cleared guitar and bass performances
and record the matching target outputs. Never put adjacent chunks from one performance into different
splits.

## Audit production corpus evidence

Run the corpus audit across every production session before treating the dataset as Phase 4 evidence:

```powershell
python ml/scripts/audit_corpus.py `
  --session D:\captures\guitar-clean --session D:\captures\guitar-crunch `
  --session D:\captures\guitar-heavy --session D:\captures\guitar-lead `
  --session D:\captures\bass-clean --session D:\captures\bass-picked `
  --session D:\captures\bass-heavy --session D:\captures\bass-slap `
  --report D:\captures\corpus-audit.json
```

The audit rejects missing or mismatched hashes, duplicate DI audio, synthetic clips, uncleared rights,
missing release documents, unknown takes, missing guitar/bass coverage, fewer than eight independent
performances, or absent categories. A passing audit is necessary for coverage but does not replace the
normal signal-quality validation for each paired session.

## Artifact contract

Every registered artifact must contain:

```text
model.bin
manifest.json
normalization.json
test-vectors/input.f32
test-vectors/output.f32
test-vectors/metadata.json
license.txt
```

`validate_artifact()` checks completeness, manifest ranges, normalization fields, paired vector
lengths, and the SHA-256 digest before registration or deployment.
