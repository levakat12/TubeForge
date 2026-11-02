"""Trains the production estimator and writes it as a packed artifact.

    python ml/scripts/train_production.py <nts_production_recovery> <destination> [--count 1600]

**The loop this closes.** `nts_ml.production.generator` samples plausible mixes; the C++ harness
renders a guitar through its own amplifier, applies the *same* chain (held to the Python one by
`ml/tests/production_parity_driver.py`) and extracts the features the plug-in would actually see;
this script fits a small regressor to them and exports it through the packed format so the runtime
inherits the hash and the test vectors. Nothing here reimplements a component that already exists
on the other side of the boundary, which is the only reason the numbers mean anything.

**What it predicts, and why only four things.** Level and spectral balance are already recovered
deterministically to 0.000 dB and 0.031 dB RMS -- there is nothing for a model to add and every
reason not to put a learned approximation in front of an exact answer. The four 0..1 amounts are
where the deterministic path fails: room ranks 0.59 against a 0.5 chance baseline, and delay and
doubling rank near 1 while reading badly wrong magnitudes. Those are calibration failures, which is
what a regression is for.

Outputs are on the same 0..1 scale the deterministic estimator produces, so
`applyProductionProfile` never learns which path filled the profile in.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from nts_ml.export import export_regressor, validate_artifact  # noqa: E402
from nts_ml.models import (  # noqa: E402
    FEATURE_NAMES,
    TARGET_NAMES,
    deterministic_baseline,
    fit,
    group_split,
)
from nts_ml.production import ProductionRanges, sample_settings  # noqa: E402

#: How each generator field is mapped onto the 0..1 the model predicts. The divisor is the top of
#: that field's sampled range, so a corpus spans the whole output interval and the sigmoid is not
#: being asked to spend its range on values that never occur.
TARGET_SCALES = {
    "roomAmount": ("reverb_amount", ProductionRanges().reverb_amount[1]),
    "delayAmount": ("delay_amount", ProductionRanges().delay_amount[1]),
    "doubling": ("doubling", ProductionRanges().doubling[1]),
    "compression": ("compression", ProductionRanges().compression[1]),
}


def pool_size(harness: Path, bass: bool) -> int:
    """How many dry renders the harness builds for this target.

    Asked rather than assumed. Guitar is searched over seven voicings and bass over all thirteen,
    so the pools are different sizes -- and a wrong count here would not fail, it would silently
    address only part of the pool and hold out renders the model had trained on.
    """
    command = [str(harness)] + (["--bass"] if bass else []) + ["--pool"]
    printed = subprocess.run(command, check=True, capture_output=True, text=True).stdout
    return int(printed.strip().splitlines()[-1])


def build_corpus(harness: Path, count: int, seed: int, render_count: int, bass: bool
                 ) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Samples settings in Python, extracts features in C++, returns the pair."""
    random = np.random.default_rng(seed)
    ranges = ProductionRanges()
    settings = [sample_settings(random, ranges) for _ in range(count)]

    # Renders are cycled rather than drawn at random so every amplifier appears about equally
    # often; a corpus that over-sampled one voicing would teach the model that voicing's spectrum
    # as if it were a property of mixes.
    matrix = np.empty((count, 9), dtype="<f4")
    for row, setting in enumerate(settings):
        matrix[row] = (row % render_count, setting.level_offset_db, setting.tilt_db,
                       setting.tilt_hz, setting.reverb_amount, setting.delay_amount,
                       setting.delay_ms, setting.doubling, setting.compression)

    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        settings_path, features_path = root / "settings.f32", root / "features.f32"
        matrix.tofile(settings_path)
        command = [str(harness)] + (["--bass"] if bass else []) \
                + ["--features", str(settings_path), str(features_path)]
        subprocess.run(command, check=True)
        features = np.fromfile(features_path, dtype="<f4").reshape(count, len(FEATURE_NAMES))

    targets = np.empty((count, len(TARGET_NAMES)), dtype="<f4")
    for column, name in enumerate(TARGET_NAMES):
        field, ceiling = TARGET_SCALES[name]
        targets[:, column] = [min(getattr(setting, field) / ceiling, 1.0) for setting in settings]

    # A row the analysis could not produce is written as zeros by the harness. Keeping it would
    # teach the model that a silent feature vector means "no production", which is a lesson drawn
    # from a failure rather than from a mix.
    usable = np.any(features != 0.0, axis=1)
    if not usable.all():
        print(f"dropping {int((~usable).sum())} rows whose analysis failed")
    # The render index goes back to the caller so the split can hold out whole renders. Splitting
    # by row would put the same amplifier on both sides and report memorisation as generalisation.
    return features[usable], targets[usable], matrix[usable, 0].astype(np.int32)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("harness", type=Path, help="path to nts_production_recovery")
    parser.add_argument("destination", type=Path, help="artifact directory to write")
    parser.add_argument("--count", type=int, default=1600)
    parser.add_argument("--seed", type=int, default=20260823)
    parser.add_argument("--hidden", type=int, default=32)
    parser.add_argument("--epochs", type=int, default=400)
    parser.add_argument("--bass", action="store_true",
                        help="train the bass estimator instead of the guitar one")
    arguments = parser.parse_args()

    # The residual's 41 bands cover 70 Hz-8 kHz for guitar and 30 Hz-8 kHz for bass, so the same
    # feature column stands for a different frequency on each. That is why there is a model per
    # instrument rather than one with an instrument flag: the inputs are not the same inputs.
    renders_available = pool_size(arguments.harness, arguments.bass)
    print(f"target: {'bass' if arguments.bass else 'guitar'}, {renders_available} dry renders")
    features, targets, renders = build_corpus(arguments.harness, arguments.count, arguments.seed,
                                              renders_available, arguments.bass)
    print(f"corpus {features.shape[0]} examples x {features.shape[1]} features "
          f"over {len(set(renders.tolist()))} renders")

    model, report = fit(features, targets, seed=arguments.seed, groups=renders,
                        hidden=arguments.hidden, epochs=arguments.epochs)
    print(f"split {report.training_examples} train / {report.validation_examples} held out"
          f" ({'by render' if report.grouped_split else 'BY ROW -- optimistic'})")
    print(f"training loss {report.training_loss:.5f}  validation loss {report.validation_loss:.5f}")
    # The comparison P10 turns on: both estimators, the same held-out rows, the same labels.
    #
    # Measuring the deterministic path from a harness run and the model from a training log would
    # compare two different sets of attempts, and any difference could be attributed to the split
    # as easily as to the estimator. Here the only thing that differs is which function turned the
    # features into a prediction.
    held = group_split(renders)
    baseline = deterministic_baseline(features[held])
    predicted = model.predict(features[held])
    truth = targets[held]
    print(f"\n{'field':<14}{'deterministic':>14}{'learned':>10}   verdict")
    improved = 0
    for column, name in enumerate(TARGET_NAMES):
        before = float(np.mean(np.abs(baseline[:, column] - truth[:, column])))
        after = float(np.mean(np.abs(predicted[:, column] - truth[:, column])))
        improved += after < before
        print(f"  {name:<12}{before:>14.4f}{after:>10.4f}   "
              f"{'better' if after < before else 'WORSE'}")
    print(f"\nthe learned estimator improves {improved} of {len(TARGET_NAMES)} fields on "
          f"{int(held.sum())} held-out examples from {len(set(renders[held].tolist()))} unseen renders")

    # The exported vectors are drawn from the corpus itself as well as from the default spread, so
    # the parity check covers the range the model will actually be asked about rather than only
    # the extremes that catch a missing standardisation.
    sample = features[:: max(1, features.shape[0] // 16)][:16]
    export_regressor(model, arguments.destination, test_features=sample)
    validate_artifact(arguments.destination)
    print(f"wrote {arguments.destination}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
