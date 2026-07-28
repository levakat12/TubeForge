from __future__ import annotations

import unittest

import numpy as np

from nts_ml.datasets import SplitItem, assert_no_phrase_leakage, split_by_performance
from nts_ml.losses import (combined_loss, dc_penalty, loudness_difference,
                           multi_resolution_stft_loss, perceptual_embedding_loss,
                           pre_emphasized_loss, spectral_convergence, waveform_l1, waveform_l2)


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


if __name__ == "__main__":
    unittest.main()
