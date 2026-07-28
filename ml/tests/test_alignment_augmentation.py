from __future__ import annotations

import unittest

import numpy as np

from nts_ml.alignment import (align_pair, correct_clock_drift, estimate_latency,
                              impulse_marker_latency, measure_alignment)
from nts_ml.augmentation import AugmentationConfig, PairedAugmenter


class AlignmentAugmentationTests(unittest.TestCase):
    def test_all_alignment_methods_and_polarity(self) -> None:
        random = np.random.default_rng(73)
        source = random.normal(0, 0.1, 32_768).astype(np.float32)
        target = np.zeros_like(source); target[183:] = -source[:-183]
        for method in ("cross_correlation", "gcc_phat"):
            latency, confidence, polarity = estimate_latency(source, target, 512, method)
            self.assertEqual(latency, 183)
            self.assertGreater(confidence, 0.99)
            self.assertEqual(polarity, -1)
        result = measure_alignment(source, target, 48_000, window_seconds=0.1, max_lag=512)
        self.assertEqual(result.global_latency_samples, 183)
        self.assertLess(abs(result.drift_ppm), 1.0)
        aligned_source, aligned_target = align_pair(source, target, result.global_latency_samples, result.polarity)
        self.assertLess(float(np.max(np.abs(aligned_source - aligned_target))), 1.0e-7)

    def test_impulse_marker(self) -> None:
        source = np.zeros(2048); target = np.zeros(2048)
        source[21] = 0.8; target[99] = 0.5
        latency, confidence, polarity = impulse_marker_latency(source, target)
        self.assertEqual(latency, 78)
        self.assertGreaterEqual(confidence, 0.5)
        self.assertEqual(polarity, 1)

    def test_clock_drift_correction(self) -> None:
        source = np.sin(np.linspace(0.0, 40.0, 100_000)).astype(np.float32)
        drifted = np.interp(np.linspace(0.0, source.size - 1, 100_100),
                            np.arange(source.size), source).astype(np.float32)
        corrected = correct_clock_drift(drifted, 1000.0)
        self.assertLessEqual(abs(corrected.size - source.size), 1)
        self.assertLess(float(np.sqrt(np.mean(np.square(corrected[:source.size] - source[:corrected.size])))), 1.0e-3)

    def test_paired_augmentations_are_deterministic_and_physical(self) -> None:
        source = np.linspace(-0.2, 0.2, 1024, dtype=np.float32)
        target = source * 0.7
        config = AugmentationConfig(-3.0, 3.0, 1.0, None, 3)
        first = PairedAugmenter(config, 9).apply(source, target)
        second = PairedAugmenter(config, 9).apply(source, target)
        np.testing.assert_array_equal(first[0], second[0])
        np.testing.assert_array_equal(first[1], second[1])
        self.assertEqual(first[2], second[2])
        np.testing.assert_allclose(first[1], first[0] * 0.7, atol=2.0e-7)


if __name__ == "__main__":
    unittest.main()
