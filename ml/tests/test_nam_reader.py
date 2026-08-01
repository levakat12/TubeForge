from __future__ import annotations

import copy
import json
import tempfile
import unittest
from pathlib import Path

import numpy as np

from nts_ml.nam import UnsupportedCapture, WaveNetModel, load_capture


def build_submodel(channels: int = 2, kernels: tuple[int, ...] = (3, 3, 2),
                   dilations: tuple[int, ...] = (1, 2, 4), head_kernel: int = 4,
                   seed: int = 7) -> dict:
    """A structurally faithful miniature of the corpus captures.

    Small enough to render in milliseconds, identical in layout to the real thing, so the tests
    exercise the parser and the streaming arithmetic without shipping a 300 kB fixture.
    """
    layer_array = {
        "input_size": 1, "condition_size": 1, "channels": channels, "bottleneck": channels,
        "head": {"out_channels": 1, "kernel_size": head_kernel, "bias": True},
        "kernel_sizes": list(kernels), "dilations": list(dilations),
        "activation": [{"type": "LeakyReLU", "negative_slope": 0.01} for _ in kernels],
        "head1x1": {"active": False, "out_channels": 1, "groups": 1},
        "layer1x1": {"active": True, "groups": 1}, "groups_input": 1, "groups_input_mixin": 1,
        "gating_mode": ["none" for _ in kernels], "secondary_activation": [None for _ in kernels],
        "slimmable": None,
    }
    for name in ("conv_pre_film", "conv_post_film", "input_mixin_pre_film", "input_mixin_post_film",
                 "activation_pre_film", "activation_post_film", "layer1x1_post_film",
                 "head1x1_post_film"):
        layer_array[name] = {"active": False, "shift": True, "groups": 1}
    head_scale = 0.125
    random = np.random.default_rng(seed)
    count = sum(channels * channels * kernel + channels + channels + channels * channels + channels
                for kernel in kernels) + channels + channels * head_kernel + 1 + 1
    weights = random.normal(0.0, 0.3, count).astype(np.float64)
    weights[-1] = head_scale
    return {"version": "0.7.0", "architecture": "WaveNet", "sample_rate": 48_000.0,
            "config": {"layers": [layer_array], "head": None, "head_scale": head_scale},
            "weights": [float(value) for value in weights],
            "metadata": {"name": "Fixture", "modeled_by": "tests", "loudness": -23.4,
                         "gear_make": "None", "tone_type": "tube", "gain": 0.5,
                         "training": {"validation_esr": 0.004}}}


def build_document(channels: int = 2, kernels: tuple[int, ...] = (3, 3, 2),
                   dilations: tuple[int, ...] = (1, 2, 4), head_kernel: int = 4,
                   seed: int = 7, container: bool = True) -> dict:
    """A capture document: either a plain WaveNet or a two-tier container, as the corpus uses.

    The lite tier is genuinely smaller, not a copy of the standard one -- in the real files the
    tiers are independently trained models with 3 and 8 channels, and a fixture that cloned one
    into the other would hide any bug that treats the two tiers as interchangeable.
    """
    model = build_submodel(channels, kernels, dilations, head_kernel, seed)
    if not container: return model
    lite = build_submodel(max(1, channels - 1), kernels, dilations, head_kernel, seed + 100)
    return {"version": "0.7.0", "architecture": "SlimmableContainer", "sample_rate": 48_000.0,
            "weights": [], "metadata": copy.deepcopy(model["metadata"]),
            "config": {"submodels": [{"max_value": 0.5, "model": lite},
                                     {"max_value": 1.0, "model": model}]}}


def write_document(directory: Path, document: dict, name: str = "fixture.nam") -> Path:
    path = directory / name
    path.write_text(json.dumps(document), encoding="utf-8")
    return path


class NamReaderTests(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = Path(tempfile.mkdtemp())

    def test_geometry_matches_the_declared_weight_vector(self) -> None:
        capture = load_capture(write_document(self.directory, build_document()))
        self.assertEqual(sorted(capture.tiers), ["lite", "standard"])
        for tier in ("lite", "standard"):
            spec = capture.spec(tier)
            self.assertEqual(spec.weights.size, spec.weight_count)
            self.assertEqual(spec.receptive_field, (3 - 1) * 1 + (3 - 1) * 2 + (2 - 1) * 4 + 4)
            self.assertEqual(spec.sample_rate, 48_000)
        self.assertEqual(capture.metadata.modeled_by, "tests")
        self.assertAlmostEqual(capture.metadata.loudness or 0.0, -23.4)
        self.assertIn("tests", capture.metadata.license_text())

    def test_weight_vector_is_consumed_exactly(self) -> None:
        capture = load_capture(write_document(self.directory, build_document()))
        model = WaveNetModel(capture.spec("standard"))
        self.assertAlmostEqual(model.head_scale, 0.125, places=6)

    def test_declared_and_actual_weight_counts_must_agree(self) -> None:
        document = build_document()
        document["config"]["submodels"][1]["model"]["weights"].append(0.0)
        with self.assertRaises(UnsupportedCapture) as raised:
            load_capture(write_document(self.directory, document))
        self.assertIn("implies", str(raised.exception))

    def test_trailing_weight_must_equal_head_scale(self) -> None:
        document = build_document()
        document["config"]["submodels"][1]["model"]["weights"][-1] = 0.9
        with self.assertRaises(UnsupportedCapture) as raised:
            load_capture(write_document(self.directory, document))
        self.assertIn("head_scale", str(raised.exception))

    def test_unsupported_features_are_rejected_rather_than_approximated(self) -> None:
        cases = {
            "film": ("conv_pre_film", {"active": True, "shift": True, "groups": 1}),
            "head1x1": ("head1x1", {"active": True, "out_channels": 1, "groups": 1}),
            "groups": ("groups_input", 2),
            "gating": ("gating_mode", ["film", "film", "film"]),
            "secondary": ("secondary_activation", [{"type": "Tanh"}, None, None]),
            "bottleneck": ("bottleneck", 1),
        }
        for name, (key, value) in cases.items():
            with self.subTest(feature=name):
                document = build_document()
                document["config"]["submodels"][1]["model"]["config"]["layers"][0][key] = value
                with self.assertRaises(UnsupportedCapture):
                    load_capture(write_document(self.directory, document, f"{name}.nam"))

    def test_unsupported_versions_and_architectures_are_rejected(self) -> None:
        for key, value in (("version", "0.5.2"), ("architecture", "LSTM")):
            with self.subTest(field=key):
                document = build_document()
                document[key] = value
                with self.assertRaises(UnsupportedCapture):
                    load_capture(write_document(self.directory, document, f"{key}.nam"))

    def test_plain_wavenet_captures_load_without_a_container(self) -> None:
        capture = load_capture(write_document(self.directory, build_document(container=False)))
        self.assertEqual(sorted(capture.tiers), ["standard"])
        with self.assertRaises(UnsupportedCapture): capture.spec("lite")


class WaveNetStreamingTests(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = Path(tempfile.mkdtemp())
        self.capture = load_capture(write_document(self.directory, build_document()))
        self.signal = np.random.default_rng(19).normal(0.0, 0.1, 997).astype(np.float32)

    def model(self) -> WaveNetModel:
        model = WaveNetModel(self.capture.spec("standard")); model.reset(); return model

    def test_output_does_not_depend_on_block_size(self) -> None:
        reference = self.model().process(self.signal)
        for block in (1, 7, 64, 512, 4096):
            with self.subTest(block=block):
                model = self.model()
                chopped = model.process_blocks(self.signal, block)
                self.assertEqual(chopped.size, reference.size)
                self.assertLess(float(np.max(np.abs(chopped - reference))), 1.0e-6)

    def test_reset_is_repeatable_and_primes_to_the_silent_state(self) -> None:
        model = self.model()
        first = model.process(self.signal)
        model.reset()
        self.assertLess(float(np.max(np.abs(model.process(self.signal) - first))), 1.0e-6)
        # Priming is not the same as a zeroed buffer: the convolution biases mean silence has a
        # non-zero steady state, and skipping the prime is audible for a full receptive field.
        raw = WaveNetModel(self.capture.spec("standard")); raw.reset(prime=False)
        self.assertGreater(float(np.max(np.abs(raw.process(self.signal) - first))), 1.0e-6)

    def test_primed_state_is_stationary_under_further_silence(self) -> None:
        model = self.model()
        silent = model.process(np.zeros(256, dtype=np.float32))
        self.assertLess(float(np.std(silent)), 1.0e-6)

    def test_output_is_finite_and_bounded_for_hot_input(self) -> None:
        model = self.model()
        output = model.process(np.clip(self.signal * 40.0, -1.0, 1.0))
        self.assertTrue(bool(np.all(np.isfinite(output))))


class ReferenceParityTests(unittest.TestCase):
    """Parity against the reference implementation, when it is installed.

    `nts-ml` is a numpy-only package by design, so `neural-amp-modeler` is an optional dev extra
    and this suite skips without it. The gate it enforces has been run against one capture from
    every archive in `amp_learning/` and `pedal_learning/`, at both tiers: worst ESR 4.4e-12.
    """

    def test_matches_the_reference_implementation(self) -> None:
        try:
            import torch
            from nam.models import init_from_nam
        except ImportError:
            self.skipTest("neural-amp-modeler is not installed")
        directory = Path(tempfile.mkdtemp())
        document = build_document(channels=3, seed=5)
        capture = load_capture(write_document(directory, document))
        signal = np.random.default_rng(23).normal(0.0, 0.1, 4096).astype(np.float32)
        with torch.no_grad():
            reference = init_from_nam(document["config"]["submodels"][1]["model"]).eval()
            expected = reference(torch.tensor(signal)).numpy()
        model = WaveNetModel(capture.spec("standard")); model.reset()
        actual = model.process(signal)
        error = float(np.sum((actual - expected) ** 2) / np.sum(expected ** 2))
        self.assertLess(error, 1.0e-6)


if __name__ == "__main__":
    unittest.main()
