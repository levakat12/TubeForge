from __future__ import annotations

from pathlib import Path
import json
import tempfile
import unittest

import numpy as np

from _fixtures import create_session
from nts_ml.capture import CaptureConfig, CaptureWizard
from nts_ml.datasets.audio import WaveReader
from nts_ml.evaluation import generate_quality_report, render_ab_comparison
from nts_ml.export import export_model, validate_artifact
from nts_ml.models import CausalTcn, ConditionedGru, ConditionedLstm, load_model_checkpoint
from nts_ml.training import ExperimentConfig, train
from nts_ml.datasets.streaming import ChunkReference, PairedChunk


def make_chunk(seed: int, history: int = 8) -> PairedChunk:
    random = np.random.default_rng(seed); source = random.normal(0, 0.08, 64).astype(np.float32)
    target = (0.7 * np.tanh(2.0 * source)).astype(np.float32)
    mask = np.zeros(source.size, dtype=np.bool_); mask[history:] = True
    return PairedChunk(source, target, mask, ChunkReference("session", f"take-{seed}",
        Path("input"), Path("output"), 0, 0, source.size, history))


class Phase5CaptureTests(unittest.TestCase):
    def test_models_are_conditioned_stateful_and_block_independent(self) -> None:
        signal = np.random.default_rng(8).normal(0.0, 0.08, 257).astype(np.float32)
        for model in (ConditionedLstm(4, seed=1), ConditionedGru(4, seed=2),
                      CausalTcn(4, seed=3, layers=3, kernel_size=3)):
            model.set_controls([0.1, -0.2, 0.3, 0.0, 1.0]); model.reset(); whole = model.process(signal)
            model.reset(); partitioned = np.concatenate((model.process(signal[:13]), model.process(signal[13:91]),
                                                         model.process(signal[91:])))
            np.testing.assert_allclose(whole, partitioned, atol=2.0e-7)
            model.reset(); model.set_controls([-0.7, 0.8, -0.3, 1.0, -1.0]); changed = model.process(signal)
            self.assertGreater(float(np.max(np.abs(whole - changed))), 1.0e-5)
            model.reset(); silence = model.process(np.zeros(512, dtype=np.float32))
            self.assertTrue(np.all(np.isfinite(silence)))
            self.assertLess(float(np.max(np.abs(silence))), 2.0)

    def test_lstm_training_export_quality_and_comparison(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); config_path = root / "config.toml"
            config_path.write_text("""
[model]
type="conditioned_lstm"
hidden_size=3
layers=1
kernel_size=3
[data]
sample_rate=48000
chunk_samples=56
history_samples=8
[training]
batch_size=1
optimizer="adamw"
learning_rate=0.001
weight_decay=0.0
epochs=2
seed=51
[loss]
time_l1=1.0
multi_resolution_stft=0.0
silence_stability=0.05
""", encoding="utf-8")
            config = ExperimentConfig.load(config_path); model = ConditionedLstm(3, seed=51)
            result = train(model, [make_chunk(1), make_chunk(2)], [make_chunk(3)], config, root / "run")
            trained = load_model_checkpoint(result.best_checkpoint)
            self.assertIsInstance(trained, ConditionedLstm)
            artifact = root / "artifact"; manifest = export_model(trained, artifact)
            self.assertEqual(manifest.model_format_version, 2); self.assertEqual(validate_artifact(artifact).architecture, "conditioned_lstm")
            seconds = 1.0; rate = 48_000; time = np.arange(int(rate * seconds)) / rate
            source = (0.08 * np.sin(2 * np.pi * 110 * time)).astype(np.float32)
            trained.reset(); target = trained.process(source)
            quality = generate_quality_report(trained, [("notes", source, target),
                ("silence", np.zeros_like(source), np.zeros_like(source))], root / "quality.json")
            self.assertIn("transientError", quality); self.assertIn("callbackBudgetPercent", quality["cpu"])
            comparison = render_ab_comparison(trained, source, target, root / "comparison", rate)
            self.assertTrue((root / "comparison" / "null-difference.wav").is_file())
            self.assertIn("modelTrimDb", comparison)

    def test_capture_wizard_safety_training_and_atomic_activation(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); wizard = CaptureWizard(root / "wizard")
            wizard.initialize(CaptureConfig(feedback_routing_acknowledged=True))
            self.assertTrue((root / "wizard" / "prepared-input" / "log-sweep.wav").is_file())
            create_session(root / "wizard" / "session")
            safety = wizard.inspect_capture(); self.assertTrue(safety["accepted"])
            train_session = create_session(root / "train")
            validation_session = create_session(root / "validation")
            config_path = root / "tiny.toml"
            config_path.write_text("""
[model]
type="tiny_tanh_rnn"
hidden_size=2
layers=1
[data]
sample_rate=48000
chunk_samples=512
history_samples=128
[training]
batch_size=2
optimizer="adamw"
learning_rate=0.001
weight_decay=0.0
epochs=1
seed=12
[loss]
time_l1=1.0
multi_resolution_stft=0.0
silence_stability=0.05
""", encoding="utf-8")
            artifact, quality = wizard.train_capture([train_session], [validation_session], config_path)
            self.assertTrue(validate_artifact(artifact)); self.assertIn("confidenceRating", quality)
            pointer = wizard.activate(artifact); active = json.loads(pointer.read_text(encoding="utf-8"))
            self.assertEqual(active["sha256"], validate_artifact(artifact).sha256)


if __name__ == "__main__": unittest.main()
