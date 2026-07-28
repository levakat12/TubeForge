from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

import numpy as np

from nts_ml.tone import (CONTROLLED_RIG_FAMILIES, DisentangledToneEncoder, InterpretableToneHeads,
                         LinearToneEncoder, evaluate_tone_encoder, multi_resolution_representation)


class Phase7ToneTests(unittest.TestCase):
    def test_representation_is_loudness_invariant(self) -> None:
        sample_rate = 16_000
        time = np.arange(sample_rate, dtype=np.float32) / sample_rate
        audio = np.sin(2.0 * np.pi * 110.0 * time).astype(np.float32)
        first = multi_resolution_representation(audio, sample_rate)
        second = multi_resolution_representation(audio * 0.08, sample_rate)
        np.testing.assert_allclose(first, second, atol=2.0e-5)

    def test_contrastive_encoder_retrieval_export_and_benchmark(self) -> None:
        self.assertEqual(len(CONTROLLED_RIG_FAMILIES), 9)
        random = np.random.default_rng(3)
        rigs, performances, productions = 9, 6, 3
        rig_centres = random.normal(0.0, 2.0, (rigs, 64))
        performance_centres = random.normal(0.0, 0.18, (performances, 64))
        production_centres = random.normal(0.0, 0.12, (productions, 64))
        features, rig_labels, performance_labels, production_labels = [], [], [], []
        for rig in range(rigs):
            for performance in range(performances):
                production = performance % productions
                features.append(rig_centres[rig] + performance_centres[performance]
                                + production_centres[production] + random.normal(0.0, 0.04, 64))
                rig_labels.append(rig); performance_labels.append(performance)
                production_labels.append(production)
        values = np.asarray(features, dtype=np.float32)
        rig_labels_array = np.asarray(rig_labels)
        family_labels = rig_labels_array // 3
        model = LinearToneEncoder()
        model.fit_contrastive(values, rig_labels_array, "phase7-synthetic-v1")
        embeddings = model.encode(values)
        self.assertEqual(embeddings.shape, (values.shape[0], 128))
        np.testing.assert_allclose(np.linalg.norm(embeddings, axis=1), 1.0, atol=1.0e-5)
        variants = values + random.normal(0.0, 0.025, values.shape)
        benchmark = evaluate_tone_encoder(
            model, values, rig_labels_array, family_labels,
            variants, values * 1.1, values + random.normal(0.0, 0.06, values.shape),
            values + random.normal(0.0, 0.10, values.shape),
            random.normal(12.0, 3.0, (20, 64)), rig_labels_array % 2,
            np.where(rig_labels_array % 2 == 1, 0.9, 0.1))
        self.assertGreater(benchmark.same_rig_retrieval_accuracy, 0.95)
        self.assertGreater(benchmark.family_retrieval_accuracy, 0.98)
        self.assertGreater(benchmark.pitch_robustness, 0.98)
        self.assertGreater(benchmark.loudness_robustness, 0.95)
        self.assertGreater(benchmark.out_of_distribution_rejection, 0.9)
        self.assertGreater(benchmark.instrument_classification_accuracy, 0.95)
        self.assertLess(benchmark.calibration_error, 0.15)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "tone-encoder.npz"
            model.save(path); restored = LinearToneEncoder.load(path)
            np.testing.assert_allclose(restored.encode(values), embeddings, atol=1.0e-6)
            self.assertEqual(restored.version, "phase7-synthetic-v1")

    def test_disentangled_heads_are_independently_versioned(self) -> None:
        random = np.random.default_rng(9)
        features = random.normal(0.0, 1.0, (24, 64)).astype(np.float32)
        rig = np.repeat(np.arange(4), 6)
        performance = np.tile(np.repeat(np.arange(3), 2), 4)
        production = np.tile(np.arange(2), 12)
        model = DisentangledToneEncoder.create()
        model.fit(features, rig, performance, production)
        self.assertTrue(model.tone.version.startswith("tone-"))
        self.assertTrue(model.performance.version.startswith("performance-"))
        self.assertTrue(model.production.version.startswith("production-"))
        embeddings = model.tone.encode(features)
        heads = InterpretableToneHeads(("gain", "brightness", "instrument", "technique"))
        targets = {"gain": np.clip(features[:, 0] * 0.2 + 0.5, 0.0, 1.0),
                   "brightness": np.clip(features[:, 1] * 0.2 + 0.5, 0.0, 1.0),
                   "instrument": (rig % 2).astype(np.float32),
                   "technique": (performance % 2).astype(np.float32)}
        heads.fit(embeddings, targets)
        prediction, uncertainty = heads.predict(embeddings)
        self.assertEqual(set(prediction), set(targets))
        self.assertTrue(np.all((prediction["gain"] >= 0.0) & (prediction["gain"] <= 1.0)))
        self.assertTrue(np.all(uncertainty["brightness"] >= 0.02))


if __name__ == "__main__":
    unittest.main()
