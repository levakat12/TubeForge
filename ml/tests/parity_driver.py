from __future__ import annotations

from pathlib import Path
import subprocess
import sys
import tempfile

import numpy as np

from nts_ml.export import export_model, validate_artifact
from nts_ml.models import CausalTcn, ConditionedGru, ConditionedLstm, TinyTanhRnn


def main() -> int:
    runtime = Path(sys.argv[1])
    summaries = []
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        vector = np.random.default_rng(311).normal(0.0, 0.12, 8192).astype(np.float32)
        for name, model in (("tanh", TinyTanhRnn(5, 48_000, 991)),
                            ("lstm", ConditionedLstm(5, 48_000, 992)),
                            ("gru", ConditionedGru(5, 48_000, 993)),
                            ("tcn", CausalTcn(5, 48_000, 994, layers=3, kernel_size=3))):
            artifact = root / name; export_model(model, artifact, vector); validate_artifact(artifact)
            actual_path = root / f"{name}-actual.f32"
            command = [str(runtime), str(artifact / "model.bin"),
                       str(artifact / "test-vectors" / "input.f32"), str(actual_path)]
            subprocess.run(command, check=True)
            expected = np.fromfile(artifact / "test-vectors" / "output.f32", dtype="<f4")
            actual = np.fromfile(actual_path, dtype="<f4")
            error = actual.astype(np.float64) - expected.astype(np.float64)
            maximum = float(np.max(np.abs(error))); rms = float(np.sqrt(np.mean(np.square(error))))
            drift = float(abs(np.sum(error)))
            if maximum > 1.0e-5 or rms > 2.0e-6 or drift > 1.0e-4:
                raise AssertionError(f"{name} parity failed: max={maximum} rms={rms} drift={drift}")
            second_path = root / f"{name}-second.f32"; subprocess.run(command[:-1] + [str(second_path)], check=True)
            reset_error = float(np.max(np.abs(np.fromfile(second_path, dtype="<f4") - actual)))
            if reset_error > 1.0e-7: raise AssertionError(f"{name} state reset mismatch: {reset_error}")
            summaries.append(f"{name}:max={maximum:.3e},rms={rms:.3e},drift={drift:.3e},reset={reset_error:.3e}")
    print("parity " + " ".join(summaries))
    return 0


raise SystemExit(main())
