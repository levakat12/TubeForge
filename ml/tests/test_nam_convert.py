from __future__ import annotations

import json
import struct
import tempfile
import unittest
from pathlib import Path

import numpy as np
from test_nam_reader import build_document, write_document

from nts_ml.export import validate_artifact
from nts_ml.export.packed import WAVENET_ARCHITECTURE, pack_wavenet
from nts_ml.nam import WaveNetModel, load_capture
from nts_ml.nam.convert import artifact_name, convert, convert_all

HEADER = struct.Struct("<4s11I")


def unpack(blob: bytes) -> dict:
    """Read a v3 header and layer table back, as the C++ runtime will have to."""
    fields = HEADER.unpack_from(blob, 0)
    layer_count = fields[8]
    offset = HEADER.size
    layers = [struct.unpack_from("<2I", blob, offset + 8 * index) for index in range(layer_count)]
    payload = blob[offset + 8 * layer_count:]
    return {"magic": fields[0], "version": fields[1], "architecture": fields[2],
            "sampleRate": fields[3], "inputChannels": fields[4], "channels": fields[5],
            "outputChannels": fields[6], "controlCount": fields[7], "layerCount": layer_count,
            "flags": fields[9], "headKernel": fields[10], "reserved": fields[11],
            "layers": layers, "weights": np.frombuffer(payload, dtype="<f4")}


class PackedWaveNetTests(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = Path(tempfile.mkdtemp())
        self.path = write_document(self.directory, build_document(channels=3), "amp.nam")
        self.capture = load_capture(self.path)

    def test_header_and_layer_table_round_trip(self) -> None:
        spec = self.capture.spec("standard")
        parsed = unpack(pack_wavenet(spec))
        self.assertEqual(parsed["magic"], b"NTSM")
        self.assertEqual(parsed["version"], 3)
        self.assertEqual(parsed["architecture"], WAVENET_ARCHITECTURE)
        self.assertEqual(parsed["sampleRate"], 48_000)
        self.assertEqual(parsed["channels"], spec.channels)
        self.assertEqual(parsed["layerCount"], len(spec.layers))
        self.assertEqual(parsed["headKernel"], spec.head_kernel_size)
        self.assertEqual(parsed["flags"], int(spec.head_bias))
        self.assertEqual(parsed["layers"],
                         [(layer.kernel_size, layer.dilation) for layer in spec.layers])
        self.assertEqual(parsed["weights"].size, spec.weights.size)
        self.assertTrue(bool(np.array_equal(parsed["weights"], spec.weights)))

    def test_payload_size_is_exactly_the_geometry(self) -> None:
        spec = self.capture.spec("standard")
        blob = pack_wavenet(spec)
        self.assertEqual(len(blob), HEADER.size + 8 * len(spec.layers) + 4 * spec.weight_count)

    def test_geometry_outside_the_runtime_limits_is_refused(self) -> None:
        from dataclasses import replace
        spec = self.capture.spec("standard")
        for name, mutated in (
            ("dilation", replace(spec, layers=(replace(spec.layers[0], dilation=99_999),))),
            ("kernel", replace(spec, layers=(replace(spec.layers[0], kernel_size=1),))),
            ("activation", replace(spec, layers=(replace(spec.layers[0], activation="Tanh"),))),
            ("condition", replace(spec, condition_size=2)),
        ):
            with self.subTest(field=name), self.assertRaises(ValueError):
                pack_wavenet(mutated)


class ConversionTests(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = Path(tempfile.mkdtemp())
        self.path = write_document(self.directory, build_document(channels=3), "Amp Capture.nam")

    def test_converted_artifact_passes_the_shared_validator(self) -> None:
        result = convert(self.path, self.directory / "artifacts")
        manifest = validate_artifact(result.artifact)
        self.assertEqual(manifest.architecture, "wavenet")
        self.assertEqual(manifest.model_format_version, 3)
        self.assertEqual(manifest.sample_rate, 48_000)
        self.assertAlmostEqual(manifest.expected_input_rms_db, -23.4, places=3)
        self.assertIn("permission", (result.artifact / "license.txt").read_text(encoding="utf-8"))

    def test_test_vectors_are_rendered_from_a_primed_state(self) -> None:
        """The vector must encode the primed state, or the runtime's reset would fail against it."""
        result = convert(self.path, self.directory / "artifacts")
        vectors = result.artifact / "test-vectors"
        source = np.fromfile(vectors / "input.f32", dtype="<f4")
        expected = np.fromfile(vectors / "output.f32", dtype="<f4")
        metadata = json.loads((vectors / "metadata.json").read_text(encoding="utf-8"))
        self.assertTrue(metadata["primedState"])
        spec = load_capture(self.path).spec("standard")
        primed = WaveNetModel(spec); primed.reset()
        self.assertLess(float(np.max(np.abs(primed.process(source) - expected))), 1.0e-6)
        zeroed = WaveNetModel(spec); zeroed.reset(prime=False)
        self.assertGreater(float(np.max(np.abs(zeroed.process(source) - expected))), 1.0e-6)

    def test_conversion_is_idempotent_and_keyed_by_content(self) -> None:
        first = convert(self.path, self.directory / "artifacts")
        second = convert(self.path, self.directory / "artifacts")
        self.assertEqual(first.artifact, second.artifact)
        self.assertEqual(first.manifest.sha256, second.manifest.sha256)
        self.assertIn(artifact_name(self.path, "standard"), str(first.artifact))
        self.assertNotEqual(artifact_name(self.path, "lite"), artifact_name(self.path, "standard"))

    def test_a_folder_converts_without_abandoning_the_readable_captures(self) -> None:
        broken = build_document(); broken["version"] = "0.5.2"
        write_document(self.directory, broken, "broken.nam")
        converted, failed = convert_all([self.directory], self.directory / "batch")
        self.assertEqual(len(converted), 1)
        self.assertEqual(len(failed), 1)
        self.assertIn("0.5.2", failed[0][1])

    def test_tiers_convert_to_distinct_artifacts(self) -> None:
        standard = convert(self.path, self.directory / "artifacts", "standard")
        lite = convert(self.path, self.directory / "artifacts", "lite")
        self.assertNotEqual(standard.artifact, lite.artifact)
        self.assertNotEqual(standard.manifest.sha256, lite.manifest.sha256)


if __name__ == "__main__":
    unittest.main()
