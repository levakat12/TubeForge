from __future__ import annotations

from pathlib import Path
import json
import sqlite3
import tempfile
import unittest

import numpy as np

from nts_ml.datasets.streaming import ChunkReference, PairedChunk
from nts_ml.evaluation import REQUIRED_CLIPS, evaluate, generate_evaluation_inputs
from nts_ml.export import ModelRegistry, export_model, validate_artifact
from nts_ml.models import TinyTanhRnn
from nts_ml.training import ExperimentConfig, ExperimentTracker, train
from nts_ml.datasets.audio import WaveReader, write_pcm_wave


def chunk(seed: int, history: int = 16) -> PairedChunk:
    random = np.random.default_rng(seed)
    source = random.normal(0, 0.08, 96).astype(np.float32)
    target = (0.6 * np.tanh(1.8 * source)).astype(np.float32)
    mask = np.zeros(source.size, dtype=np.bool_); mask[history:] = True
    reference = ChunkReference("session", f"take-{seed}", Path("input"), Path("output"),
                               0, 0, source.size, history)
    return PairedChunk(source, target, mask, reference)


class TrainingExportEvaluationTests(unittest.TestCase):
    def test_tiny_training_is_seed_deterministic_and_resolved(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            config_path = root / "tiny.toml"
            config_path.write_text("""
[model]
type="tiny_tanh_rnn"
hidden_size=3
layers=1
[data]
sample_rate=48000
chunk_samples=80
history_samples=16
[training]
batch_size=1
optimizer="adamw"
learning_rate=0.001
weight_decay=0.0
epochs=2
seed=41
[loss]
time_l1=1.0
""", encoding="utf-8")
            config = ExperimentConfig.load(config_path)
            outputs = []
            for run in range(2):
                model = TinyTanhRnn(3, 48_000, 41)
                result = train(model, [chunk(1), chunk(2)], [chunk(3)], config, root / f"run-{run}")
                outputs.append(TinyTanhRnn.load_checkpoint(result.best_checkpoint).parameters())
                self.assertTrue((root / f"run-{run}" / "resolved-config.json").is_file())
            for name in outputs[0]:
                np.testing.assert_array_equal(outputs[0][name], outputs[1][name])

    def test_experiment_tracking_export_registry_and_evaluation(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            model = TinyTanhRnn(4, 48_000, 17)
            artifact = root / "artifact"
            manifest = export_model(model, artifact)
            self.assertEqual(validate_artifact(artifact).sha256, manifest.sha256)
            registry = ModelRegistry(root / "registry")
            registered = registry.register("original-amp", "1.0.0", artifact)
            self.assertEqual(registry.versions("original-amp"), ["1.0.0"])
            self.assertEqual(registry.resolve("original-amp", "1.0.0")[0], registered)

            database = root / "experiments.sqlite3"
            with ExperimentTracker(database) as tracker:
                record = tracker.start("run-1", root, {"seed": 17}, 17, "dataset-hash")
                tracker.finish(record, 1.25, root / "checkpoint.npz", {"l1": 0.1}, manifest.sha256)
            connection = sqlite3.connect(database)
            try:
                row = connection.execute(
                    "SELECT random_seed, dataset_version, exported_model_hash FROM experiments").fetchone()
            finally:
                connection.close()
            self.assertEqual(row, (17, "dataset-hash", manifest.sha256))

            evaluation_root = root / "evaluation"
            generate_evaluation_inputs(evaluation_root, seconds=0.05)
            clips = []
            for name in REQUIRED_CLIPS:
                source = WaveReader(evaluation_root / "input" / f"{name}.wav").read()[:, 0]
                model.reset(); target = model.process(source)
                write_pcm_wave(evaluation_root / "output" / f"{name}.wav", target, 48_000, 24)
                clips.append((name, source, target))
            report_path = root / "evaluation-report.json"
            report = evaluate(model, clips, report_path)
            self.assertEqual(len(report["clips"]), 14)
            self.assertTrue(report_path.is_file())
            self.assertLess(report["aggregate"]["maximumAbsoluteError"], 1.0e-7)


if __name__ == "__main__":
    unittest.main()
