from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
from test_nam_reader import build_document

from nts_ml.export import export_model, export_wavenet, validate_artifact
from nts_ml.models import ConditionedLstm, RandomFeatureGru, RandomFeatureTcn, TinyTanhRnn
from nts_ml.nam import load_capture


def check_wavenet(runtime: Path, root: Path) -> str:
    """Hold the C++ WaveNet to the vectors `nts_ml.nam` exports for it.

    This is the gate that keeps the two implementations honest. The Python side is itself checked
    against the reference implementation in `test_nam_reader.ReferenceParityTests`, so a chain of
    two enforced comparisons ties the runtime to upstream NAM without CI needing torch.

    The vectors encode a *primed* state, so a runtime that zeroed its buffers on reset instead of
    restoring the silent steady state fails here rather than in someone's ears.
    """
    document = build_document(channels=3, kernels=(3, 3, 2), dilations=(1, 2, 4), seed=41)
    capture_path = root / "capture.nam"
    capture_path.write_text(json.dumps(document), encoding="utf-8")
    artifact = root / "wavenet"
    export_wavenet(load_capture(capture_path), artifact)
    validate_artifact(artifact)
    tolerances = json.loads((artifact / "test-vectors" / "metadata.json").read_text(encoding="utf-8"))
    actual_path = root / "wavenet-actual.f32"
    command = [str(runtime), str(artifact / "model.bin"),
               str(artifact / "test-vectors" / "input.f32"), str(actual_path)]
    subprocess.run(command, check=True)
    expected = np.fromfile(artifact / "test-vectors" / "output.f32", dtype="<f4")
    actual = np.fromfile(actual_path, dtype="<f4")
    if actual.size != expected.size:
        raise AssertionError(f"wavenet parity produced {actual.size} samples, expected {expected.size}")
    error = actual.astype(np.float64) - expected.astype(np.float64)
    maximum = float(np.max(np.abs(error)))
    rms = float(np.sqrt(np.mean(np.square(error))))
    if maximum > tolerances["maximumAbsoluteErrorTolerance"] or rms > tolerances["rmsErrorTolerance"]:
        raise AssertionError(f"wavenet parity failed: max={maximum} rms={rms}")
    second_path = root / "wavenet-second.f32"
    subprocess.run(command[:-1] + [str(second_path)], check=True)
    reset_error = float(np.max(np.abs(np.fromfile(second_path, dtype="<f4") - actual)))
    if reset_error > 1.0e-7:
        raise AssertionError(f"wavenet state reset mismatch: {reset_error}")
    return f"wavenet:max={maximum:.3e},rms={rms:.3e},reset={reset_error:.3e}"


def main() -> int:
    runtime = Path(sys.argv[1])
    summaries = []
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        vector = np.random.default_rng(311).normal(0.0, 0.12, 8192).astype(np.float32)
        for name, model in (("tanh", TinyTanhRnn(5, 48_000, 991)),
                            ("lstm", ConditionedLstm(5, 48_000, 992)),
                            ("gru", RandomFeatureGru(5, 48_000, 993)),
                            ("tcn", RandomFeatureTcn(5, 48_000, 994, layers=3, kernel_size=3))):
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
        summaries.append(check_wavenet(runtime, root))
    print("parity " + " ".join(summaries))
    return 0


raise SystemExit(main())
