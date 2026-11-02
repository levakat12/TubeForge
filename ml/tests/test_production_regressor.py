from __future__ import annotations

import unittest

import numpy as np

from nts_ml.export import export_regressor, pack_regressor, validate_artifact
from nts_ml.models import (
    FEATURE_NAMES,
    TARGET_NAMES,
    ProductionRegressor,
    deterministic_baseline,
    fit,
    group_split,
)
from nts_ml.schemas.model import ModelManifest


def _corpus(examples: int = 600, renders: int = 30, seed: int = 5
            ) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """A corpus whose targets are a knowable function of the features, plus a per-render offset.

    Deliberately *not* random noise. The point of the tests below is that fitting recovers a
    relationship and that a render-disjoint split measures generalisation rather than
    memorisation, and neither claim can be checked against labels nothing predicts. The per-render
    offset is the part a row-wise split would leak: it is constant within a render, so a model that
    has seen a render can read it off and a model that has not cannot.
    """
    random = np.random.default_rng(seed)
    identifiers = np.arange(examples) % renders
    features = random.normal(0.0, 1.0, (examples, len(FEATURE_NAMES))).astype(np.float32)
    offsets = random.normal(0.0, 0.05, renders)
    targets = np.empty((examples, len(TARGET_NAMES)), dtype=np.float32)
    for row in range(examples):
        base = 0.5 + 0.2 * np.tanh(features[row, :4])
        targets[row] = np.clip(base + offsets[identifiers[row]], 0.0, 1.0)
    return features, targets, identifiers


class ProductionRegressorTests(unittest.TestCase):
    def test_predict_matches_a_hand_evaluated_network(self) -> None:
        """The forward pass, checked against the arithmetic written out by hand.

        The C++ evaluator is held to *this* function by `production_parity_driver.py`, so if this
        one were wrong the parity gate would happily certify two implementations of the same
        mistake. Somewhere the maths has to be stated twice.
        """
        model = ProductionRegressor.initialise(inputs=3, hidden=2, outputs=2, seed=1)
        model.input_mean = np.array([1.0, -2.0, 0.5], dtype=np.float32)
        model.input_scale = np.array([2.0, 4.0, 0.25], dtype=np.float32)
        model.hidden_weight = np.array([[0.5, -0.25], [1.0, 0.75], [-0.5, 0.2]], dtype=np.float32)
        model.hidden_bias = np.array([0.1, -0.3], dtype=np.float32)
        model.output_weight = np.array([[1.5, -0.5], [0.25, 2.0]], dtype=np.float32)
        model.output_bias = np.array([-0.2, 0.4], dtype=np.float32)

        features = np.array([[3.0, 2.0, 1.0]], dtype=np.float32)
        standardised = (features[0] - model.input_mean) / model.input_scale
        hidden = np.tanh(standardised @ model.hidden_weight + model.hidden_bias)
        expected = 1.0 / (1.0 + np.exp(-(hidden @ model.output_weight + model.output_bias)))
        np.testing.assert_allclose(model.predict(features)[0], expected, rtol=1e-6, atol=1e-6)

    def test_a_wrong_width_vector_is_refused(self) -> None:
        model = ProductionRegressor.initialise(inputs=4, hidden=2, outputs=2, seed=1)
        with self.assertRaises(ValueError):
            model.predict(np.zeros((1, 3), dtype=np.float32))

    def test_fitting_beats_the_deterministic_baseline_it_replaces(self) -> None:
        """The claim P10 rests on, in miniature and on a split that holds out whole renders.

        `deterministic_baseline` is what `estimateProductionProfile` reports without a model --
        four feature columns clamped to 0..1 -- so this compares the two estimators the way the
        training script does: same rows, same labels, only the function differs.
        """
        features, targets, renders = _corpus()
        model, report = fit(features, targets, seed=11, groups=renders, epochs=120)
        self.assertTrue(report.grouped_split)
        self.assertGreater(report.training_examples, report.validation_examples)

        held = group_split(renders)
        # No render may appear on both sides, or the comparison measures memorisation.
        self.assertFalse(set(renders[held].tolist()) & set(renders[~held].tolist()))

        baseline_error = float(np.mean(np.abs(deterministic_baseline(features[held]) - targets[held])))
        learned_error = float(np.mean(np.abs(model.predict(features[held]) - targets[held])))
        self.assertLess(learned_error, baseline_error)

    def test_a_split_with_too_few_groups_is_refused(self) -> None:
        features, targets, _ = _corpus(examples=100, renders=3)
        with self.assertRaises(ValueError):
            fit(features, targets, seed=1, groups=np.arange(100) % 3, epochs=1)


class ProductionArtifactTests(unittest.TestCase):
    def test_the_packed_payload_is_exactly_the_declared_geometry(self) -> None:
        """A header that disagrees with its payload is the failure the C++ loader refuses.

        Checked from this side too, because the loader can only reject what the exporter produces
        -- and an exporter that wrote a short payload would make every artifact unloadable with an
        error naming the reader rather than the writer.
        """
        model = ProductionRegressor.initialise(inputs=57, hidden=32, outputs=4, seed=3)
        packed = pack_regressor(model)
        header = 4 + 5 * 4
        floats = 57 * 2 + 57 * 32 + 32 + 32 * 4 + 4
        self.assertEqual(len(packed), header + floats * 4)
        self.assertEqual(packed[:4], b"NTSM")

    def test_an_oversized_model_is_refused_before_it_is_written(self) -> None:
        model = ProductionRegressor.initialise(inputs=57, hidden=1000, outputs=4, seed=3)
        with self.assertRaises(ValueError):
            pack_regressor(model)

    def test_the_artifact_round_trips_through_validation(self) -> None:
        import tempfile
        from pathlib import Path

        model = ProductionRegressor.initialise(inputs=57, hidden=8, outputs=4, seed=7)
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / "estimator"
            manifest = export_regressor(model, destination)
            self.assertEqual(manifest.architecture, "production_regressor")
            self.assertEqual(manifest.model_format_version, 4)
            self.assertEqual(manifest.input_channels, 57)
            self.assertEqual(manifest.output_channels, 4)
            # The rows are 57 wide against 4 wide, so `validate_artifact` has to pair them by row
            # count. An element-count comparison -- which is what it did before this format
            # existed -- would reject a correct artifact.
            validate_artifact(destination)

            # A tampered payload must fail the hash rather than load with different weights.
            binary = destination / "model.bin"
            corrupt = bytearray(binary.read_bytes())
            corrupt[-1] ^= 0xFF
            binary.write_bytes(bytes(corrupt))
            with self.assertRaises(ValueError):
                validate_artifact(destination)

    def test_the_manifest_still_refuses_an_unknown_architecture(self) -> None:
        manifest = ModelManifest(model_format_version=4, architecture="production_regressor",
                                 input_channels=57, output_channels=4, state_size=32)
        manifest.validate()
        manifest.architecture = "something_else"
        with self.assertRaises(ValueError):
            manifest.validate()


if __name__ == "__main__":
    unittest.main()
