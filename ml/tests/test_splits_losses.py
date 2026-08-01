from __future__ import annotations

import unittest

import numpy as np

from nts_ml.datasets import SplitItem, assert_no_phrase_leakage, split_by_performance
from nts_ml.losses import (
    combined_loss,
    dc_penalty,
    loudness_difference,
    multi_resolution_stft_loss,
    perceptual_embedding_loss,
    pre_emphasized_loss,
    spectral_convergence,
    waveform_l1,
    waveform_l2,
)


class SplitLossTests(unittest.TestCase):
    def test_performance_split_has_no_phrase_leakage(self) -> None:
        items = [SplitItem(f"session-{index // 4}", f"phrase-{index}") for index in range(100)]
        splits = split_by_performance(items, seed=417)
        assert_no_phrase_leakage(splits)
        self.assertEqual(sum(map(len, splits.values())), len(items))
        self.assertTrue(all(splits[name] for name in ("train", "validation", "test")))
        self.assertEqual(splits, split_by_performance(items, seed=417))

    def test_loss_suite(self) -> None:
        time = np.arange(4096) / 48_000.0
        target = np.sin(2 * np.pi * 440 * time).astype(np.float32) * 0.1
        prediction = target * 0.9 + 0.001
        self.assertGreater(waveform_l1(prediction, target), 0.0)
        self.assertGreater(waveform_l2(prediction, target), 0.0)
        self.assertGreater(pre_emphasized_loss(prediction, target), 0.0)
        self.assertGreater(multi_resolution_stft_loss(prediction, target), 0.0)
        self.assertGreater(spectral_convergence(prediction, target), 0.0)
        self.assertGreater(dc_penalty(prediction), 0.0)
        self.assertGreater(loudness_difference(prediction, target), 0.0)
        embedding = lambda audio: np.array([np.mean(audio), np.std(audio)])
        self.assertGreater(perceptual_embedding_loss(prediction, target, embedding), 0.0)
        total, components = combined_loss(prediction, target, {"time_l1": 1.0, "dc_penalty": 0.1})
        self.assertGreater(total, 0.0)
        self.assertEqual(len(components), 9)
        self.assertIn("silence_stability", components)
        self.assertIn("transient_weighted", components)


class TrainingGradientTests(unittest.TestCase):
    def test_pre_emphasis_gradient_matches_finite_differences(self) -> None:
        """An adjoint that is subtly wrong still trains, just towards the wrong thing."""
        from nts_ml.training.trainer import _error_signal

        rng = np.random.default_rng(5)
        outputs = rng.normal(0.0, 0.3, 48).astype(np.float32)
        target = rng.normal(0.0, 0.3, 48).astype(np.float32)
        history = 8
        for weight in (0.0, 0.5, 1.0):
            _loss, _active, gradient = _error_signal(outputs, target, history, weight)
            numerical = np.zeros_like(gradient)
            step = 1e-3
            for index in range(outputs.size):
                up = outputs.astype(np.float64).copy(); up[index] += step
                down = outputs.astype(np.float64).copy(); down[index] -= step
                loss_up, _, _ = _error_signal(up.astype(np.float32), target, history, weight)
                loss_down, _, _ = _error_signal(down.astype(np.float32), target, history, weight)
                numerical[index] = (loss_up - loss_down) / (2.0 * step)
            scale = float(np.max(np.abs(numerical[history:]))) or 1.0
            worst = float(np.max(np.abs(gradient[history:] - numerical[history:])))
            self.assertLess(worst / scale, 2e-3, f"gradient disagrees at weight {weight}")

    def test_zero_pre_emphasis_weight_reproduces_plain_squared_error(self) -> None:
        """The default configuration must be bit-for-bit what it was before pre-emphasis existed."""
        from nts_ml.training.trainer import _error_signal

        rng = np.random.default_rng(6)
        outputs = rng.normal(0.0, 0.2, 32).astype(np.float32)
        target = rng.normal(0.0, 0.2, 32).astype(np.float32)
        history = 4
        loss, active, gradient = _error_signal(outputs, target, history, 0.0)

        errors = (outputs - target).astype(np.float32); errors[:history] = 0.0
        expected_active = max(1, target.size - history)
        np.testing.assert_array_equal(gradient, (2.0 * errors / expected_active).astype(np.float32))
        self.assertEqual(active, expected_active)
        self.assertAlmostEqual(loss, float(np.sum(np.square(errors)) / expected_active), places=9)


if __name__ == "__main__":
    unittest.main()
