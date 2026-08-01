from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

import numpy as np
from _fixtures import create_session

from nts_ml.alignment import write_alignment_html
from nts_ml.datasets import SessionPackage, StreamingPairedDataset, WaveReader, validate_session
from nts_ml.schemas import SessionMetadata


class SchemaDatasetTests(unittest.TestCase):
    def test_schema_package_quality_and_24_bit_audio(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = create_session(Path(temporary) / "capture")
            package = SessionPackage(root)
            self.assertEqual(package.metadata.instrument, "guitar")
            self.assertEqual(WaveReader(package.takes[0].input_path).info.bit_depth, 24)
            report = validate_session(package)
            self.assertTrue(report.accepted, [check for check in report.checks if not check.passed])
            self.assertTrue((root / "reports" / "validation.json").is_file())
            write_alignment_html(root / "reports" / "validation.json", root / "reports" / "alignment.html")
            self.assertIn("Alignment report", (root / "reports" / "alignment.html").read_text(encoding="utf-8"))
            self.assertGreater(report.alignment.confidence, 0.9)
            self.assertEqual(report.alignment.global_latency_samples, 37)
            self.assertEqual(len(package.dataset_version()), 64)

    def test_invalid_metadata_is_rejected(self) -> None:
        with self.assertRaises(ValueError):
            SessionMetadata.from_dict({"schemaVersion": 1})

    def test_low_confidence_session_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = create_session(Path(temporary) / "capture")
            output_path = root / "output" / "take-0001.wav"
            random = np.random.default_rng(4).normal(0.0, 0.03, 57_600).astype(np.float32)
            from nts_ml.datasets import write_pcm_wave
            write_pcm_wave(output_path, random, 48_000, 24)
            report = validate_session(SessionPackage(root), write_report=False)
            self.assertFalse(report.accepted)
            self.assertTrue(any(check.name.endswith("alignment_confidence") and not check.passed
                                for check in report.checks))

    def test_history_aware_streaming(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            package = SessionPackage(create_session(Path(temporary) / "capture"))
            dataset = StreamingPairedDataset([package], chunk_samples=1024, history_samples=256)
            self.assertGreater(len(dataset), 1)
            chunk = dataset[0]
            self.assertEqual(chunk.input.shape, (1280,))
            self.assertFalse(np.any(chunk.loss_mask[:256]))
            self.assertTrue(np.all(chunk.loss_mask[256:]))
            self.assertEqual(chunk.reference.history_samples, 256)
            self.assertGreater(float(np.corrcoef(chunk.input, chunk.target)[0, 1]), 0.99)


if __name__ == "__main__":
    unittest.main()
