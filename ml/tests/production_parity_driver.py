"""Holds `nts_ml.production.chain` to the C++ copy in `Tests/ProductionChain.h`.

**Why this exists at all.** There are two implementations of the same production chain, and they
serve opposite ends of the same loop: the C++ one applies it in `nts_production_recovery` to score
the deterministic estimator, and the Python one applies it to generate training data for the model
meant to beat that score. If they drift, the model learns one mix and the benchmark measures
another, and *nothing reports a problem* -- the numbers stay plausible and the model is simply
worse in the field than on paper. That is the hardest kind of defect to attribute after the fact,
so it gets a gate instead of a comment.

Run it the way the other parity drivers are run, pointing at a built harness::

    python ml/tests/production_parity_driver.py build/nts_production_recovery_artefacts/Release/nts_production_recovery.exe

Not part of `pytest` collection, because it needs a compiled binary that CI builds in a different
job -- the same reason `parity_driver.py` and `nam_parity_driver.py` sit beside their test files
rather than inside them.
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from nts_ml.production import ProductionSettings, apply_chain  # noqa: E402

SAMPLE_RATE = 48000.0

#: Settings that each exercise one stage alone, plus one that runs all of them at once.
#:
#: One-at-a-time because a mismatch in a combined case cannot be attributed, and the whole point
#: of a parity gate is that a failure names the stage that drifted. The combined case is still
#: there because stage *order* is part of the contract and only a combined signal can catch a
#: swap.
CASES: dict[str, ProductionSettings] = {
    "level": ProductionSettings(level_offset_db=-7.5),
    "tilt-up": ProductionSettings(tilt_db=6.0, tilt_hz=1200.0),
    "tilt-down": ProductionSettings(tilt_db=-5.0, tilt_hz=800.0),
    "reverb": ProductionSettings(reverb_amount=0.55),
    "delay": ProductionSettings(delay_amount=0.4, delay_ms=180.0),
    "doubling": ProductionSettings(doubling=0.7),
    "compression": ProductionSettings(compression=0.65),
    "everything": ProductionSettings(level_offset_db=-3.0, tilt_db=4.0, tilt_hz=1500.0,
                                     reverb_amount=0.3, delay_amount=0.25, delay_ms=240.0,
                                     doubling=0.5, compression=0.4),
}

#: Tolerances, not equality. The two implementations accumulate in float32 in different orders --
#: numpy advances a comb a whole stride at a time where the C++ loop walks samples -- so bit
#: equality would fail on a difference nobody could hear or act on. These bounds are tight enough
#: that a wrong constant, a swapped stage or a sign error cannot hide under them.
MAXIMUM_ABSOLUTE = 2.0e-4
MAXIMUM_RMS = 2.0e-5


def _dry_signal(samples: int = 24000) -> np.ndarray:
    """A deterministic broadband test signal with transients and sustain.

    Not a sine: a single tone would let a reverb, a delay and a doubling delay all read as a phase
    shift, so three stages could be wrong in the same way and still agree. The plucks give the
    compressor an envelope to act on and the delay a transient to repeat.
    """
    time = np.arange(samples, dtype=np.float64) / SAMPLE_RATE
    value = np.zeros(samples, dtype=np.float64)
    for partial, frequency in enumerate((110.0, 220.0, 447.0, 923.0, 1897.0), start=1):
        value += np.sin(2.0 * np.pi * frequency * time) / (partial * partial)
    envelope = np.exp(-np.mod(time, 0.25) * 8.0) * (1.0 - np.exp(-np.mod(time, 0.25) * 700.0))
    return (value * envelope * 0.3).astype(np.float32)


def check(harness: Path) -> str:
    dry = _dry_signal()
    failures: list[str] = []
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        dry_path = root / "dry.f32"
        dry.tofile(dry_path)
        for name, settings in CASES.items():
            produced_path = root / f"{name}.f32"
            command = [str(harness), "--produce", str(dry_path), str(produced_path),
                       str(settings.level_offset_db), str(settings.tilt_db), str(settings.tilt_hz),
                       str(settings.reverb_amount), str(settings.delay_amount),
                       str(settings.delay_ms), str(settings.doubling), str(settings.compression)]
            subprocess.run(command, check=True)
            interleaved = np.fromfile(produced_path, dtype="<f4")
            expected_left, expected_right = apply_chain(dry, SAMPLE_RATE, settings)
            actual_left = interleaved[0::2]
            actual_right = interleaved[1::2]
            if actual_left.size != expected_left.size:
                failures.append(f"{name}: {actual_left.size} samples, expected {expected_left.size}")
                continue
            for side, actual, expected in (("left", actual_left, expected_left),
                                           ("right", actual_right, expected_right)):
                error = actual.astype(np.float64) - expected.astype(np.float64)
                maximum = float(np.max(np.abs(error)))
                rms = float(np.sqrt(np.mean(np.square(error))))
                if maximum > MAXIMUM_ABSOLUTE or rms > MAXIMUM_RMS:
                    failures.append(f"{name} {side}: max {maximum:.3e} rms {rms:.3e}")
    if failures:
        raise AssertionError("production chain parity failed:\n  " + "\n  ".join(failures))
    return f"production chain parity: {len(CASES)} cases within {MAXIMUM_ABSOLUTE:.0e}"


def check_ranges(harness: Path) -> str:
    """Asserts the C++ `productionCeilings` and the Python `ProductionRanges` still agree.

    They are the divisor that puts a raw chain amount on the 0..1 both estimators report, so a
    disagreement means the model is trained against one normalisation and scored against another.
    Neither half would look wrong on its own: each would be internally consistent and they would
    simply disagree about what 0.5 means, which shows up as a model that is worse than the
    deterministic path for no visible reason.
    """
    from nts_ml.production import ProductionRanges

    printed = subprocess.run([str(harness), "--ranges"], check=True, capture_output=True,
                             text=True).stdout.strip().splitlines()[-1].split()
    if len(printed) != 4:
        raise AssertionError(f"--ranges printed {len(printed)} values, expected 4")
    ranges = ProductionRanges()
    expected = (ranges.reverb_amount[1], ranges.delay_amount[1], ranges.doubling[1],
                ranges.compression[1])
    names = ("reverb", "delay", "doubling", "compression")
    for name, actual, wanted in zip(names, printed, expected, strict=True):
        if abs(float(actual) - wanted) > 1.0e-6:
            raise AssertionError(f"ceiling for {name}: C++ says {actual}, Python says {wanted}")
    return "range ceilings agree: " + ", ".join(
        f"{name}={value}" for name, value in zip(names, expected, strict=True))


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: production_parity_driver.py <nts_production_recovery> [nts_ml_runtime_cli]",
              file=sys.stderr)
        return 2
    print(check(Path(sys.argv[1])))
    print(check_ranges(Path(sys.argv[1])))
    # Optional, because the two halves are built by different targets and a caller may have only
    # one to hand. Silence here means the regressor was not checked, not that it passed.
    if len(sys.argv) > 2:
        print(check_regressor(Path(sys.argv[2])))
    return 0




def check_regressor(runtime: Path) -> str:
    """Holds the C++ `PackedFeatureRegressor` to the Python model that trained the weights.

    This is the gate that makes the packed format worth using for a model that never touches the
    audio thread. Without it the C++ evaluator could transpose a weight matrix, skip the
    standardisation, or read the layers in the wrong order, and none of those would crash -- they
    would produce a plausible number in 0..1 and the production layer would apply it. The vectors
    deliberately sit several standard deviations out, because near the training mean a missing
    standardisation barely changes the answer and would slip through.
    """
    import numpy as np

    from nts_ml.export import export_regressor, validate_artifact
    from nts_ml.models import ProductionRegressor

    random = np.random.default_rng(2718)
    model = ProductionRegressor.initialise(inputs=57, hidden=32, outputs=4, seed=31)
    # Weights and standardisation given real spread, so a transpose or a skipped step is visible.
    model.input_mean = random.normal(0.0, 2.0, 57).astype(np.float32)
    model.input_scale = (0.5 + random.random(57) * 3.0).astype(np.float32)
    model.hidden_bias = random.normal(0.0, 0.5, 32).astype(np.float32)
    model.output_bias = random.normal(0.0, 0.5, 4).astype(np.float32)

    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        artifact = root / "regressor"
        export_regressor(model, artifact)
        validate_artifact(artifact)
        actual_path = root / "actual.f32"
        subprocess.run([str(runtime), "--regressor", str(artifact / "model.bin"),
                        str(artifact / "test-vectors" / "input.f32"), str(actual_path)], check=True)
        expected = np.fromfile(artifact / "test-vectors" / "output.f32", dtype="<f4")
        actual = np.fromfile(actual_path, dtype="<f4")
        if actual.size != expected.size:
            raise AssertionError(f"regressor parity produced {actual.size}, expected {expected.size}")
        error = actual.astype(np.float64) - expected.astype(np.float64)
        maximum = float(np.max(np.abs(error)))
        rms = float(np.sqrt(np.mean(np.square(error))))
        if maximum > 1.0e-5 or rms > 2.0e-6:
            raise AssertionError(f"regressor parity failed: max {maximum:.3e} rms {rms:.3e}")
    return f"regressor parity: {expected.size // 4} rows within 1e-05"


if __name__ == "__main__":
    raise SystemExit(main())
