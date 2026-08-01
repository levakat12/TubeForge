"""Trains an LSTM with the torch backend, then replays it through the compiled C++ runtime.

The torch backend is only safe because nothing downstream of training can tell which backend
produced a checkpoint. This asserts exactly that, at the same tolerances the NumPy parity
driver uses -- no allowance is made for the backend.

Skips cleanly when torch is absent, so it can sit in CTest on machines without it.
"""
from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

from nts_ml.export import export_model, validate_artifact
from nts_ml.models import ConditionedLstm
from nts_ml.training import ExperimentConfig, torch_backend
from nts_ml.training.trainer import train

# Same thresholds as parity_driver.py. If these ever diverge, the point of the test is lost.
MAXIMUM_ERROR = 1.0e-5
MAXIMUM_RMS = 2.0e-6
MAXIMUM_DRIFT = 1.0e-4


def main() -> int:
    if not torch_backend.is_available():
        print("torch parity skipped: " + (torch_backend.unavailable_reason() or "torch not installed"))
        return 0

    runtime = Path(sys.argv[1])
    rng = np.random.default_rng(23)

    def make_chunks(count: int, samples: int = 2048):
        from nts_ml.datasets.streaming import ChunkReference, PairedChunk
        chunks = []
        for index in range(count):
            audio = rng.normal(0.0, 0.15, samples).astype(np.float32)
            mask = np.ones(samples, dtype=np.bool_)
            mask[:256] = False
            reference = ChunkReference(session_id="torch-parity", take_id=f"take-{index}",
                                       input_path=Path("in.wav"), output_path=Path("out.wav"),
                                       start=0, output_start=0, total_samples=samples,
                                       history_samples=256, controls=(0.2, -0.1, 0.4, 0.0, 1.0))
            chunks.append(PairedChunk(audio, np.tanh(3.0 * audio).astype(np.float32), mask, reference))
        return chunks

    config = ExperimentConfig()
    config.training.backend = "torch"
    config.training.epochs = 6
    config.training.batch_size = 4
    config.training.learning_rate = 0.01
    # A weighted composite objective, so the differentiable loss is exercised rather than
    # falling through to a plain error term.
    config.loss.pre_emphasis = 0.3
    config.loss.multi_resolution_stft = 0.5

    model = ConditionedLstm(16, 48_000, seed=5)
    model.set_controls([0.2, -0.1, 0.4, 0.0, 1.0])

    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        result = train(model, make_chunks(8), make_chunks(3), config, root / "run")
        if not result.epoch_losses or result.epoch_losses[-1] >= result.epoch_losses[0]:
            raise AssertionError("torch training did not reduce the loss, so nothing was learned")

        restored = ConditionedLstm.load_checkpoint(result.best_checkpoint)
        vector = np.random.default_rng(311).normal(0.0, 0.12, 8192).astype(np.float32)
        artifact = root / "exported"
        export_model(restored, artifact, vector)
        validate_artifact(artifact)

        actual_path = root / "actual.f32"
        subprocess.run([str(runtime), str(artifact / "model.bin"),
                        str(artifact / "test-vectors" / "input.f32"), str(actual_path)], check=True)

        expected = np.fromfile(artifact / "test-vectors" / "output.f32", dtype="<f4")
        actual = np.fromfile(actual_path, dtype="<f4")
        error = actual.astype(np.float64) - expected.astype(np.float64)
        maximum = float(np.max(np.abs(error)))
        rms = float(np.sqrt(np.mean(np.square(error))))
        drift = float(abs(np.sum(error)))
        if maximum > MAXIMUM_ERROR or rms > MAXIMUM_RMS or drift > MAXIMUM_DRIFT:
            raise AssertionError(f"torch-trained model fails C++ parity: "
                                 f"max={maximum} rms={rms} drift={drift}")
        print(f"torch parity max={maximum:.3e} rms={rms:.3e} drift={drift:.3e} "
              f"loss {result.epoch_losses[0]:.4f} -> {result.epoch_losses[-1]:.4f}")
    return 0


raise SystemExit(main())
