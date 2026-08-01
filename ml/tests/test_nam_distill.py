from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

import numpy as np
from test_nam_reader import build_document, write_document

from nts_ml.datasets import SessionPackage, validate_session
from nts_ml.datasets.audio import write_pcm_wave
from nts_ml.export import validate_artifact
from nts_ml.nam import build_sessions, distill, load_capture, render, write_session
from nts_ml.nam.catalogue import build_catalogue, parse_settings


def di_signal(seed: int, seconds: float = 1.5, sample_rate: int = 48_000) -> np.ndarray:
    """A plucked-note-ish DI: decaying harmonics plus a little noise, well below clipping."""
    random = np.random.default_rng(seed)
    time = np.arange(int(sample_rate * seconds)) / sample_rate
    envelope = np.exp(-2.5 * (time % 0.5))
    tone = sum(0.3 / harmonic * np.sin(2 * np.pi * 110.0 * harmonic * time)
               for harmonic in (1, 2, 3))
    return ((tone * envelope + 0.002 * random.standard_normal(time.size)) * 0.4).astype(np.float32)


class DistillationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = Path(tempfile.mkdtemp())
        self.capture_path = write_document(self.directory, build_document(channels=2), "amp.nam")
        self.capture = load_capture(self.capture_path)
        self.corpus = self.directory / "di"; self.corpus.mkdir()
        for index in range(4):
            write_pcm_wave(self.corpus / f"di-{index}.wav", di_signal(index), 48_000, 24)

    def test_rendered_sessions_pass_the_capture_quality_gate(self) -> None:
        sessions = build_sessions(self.capture, "standard", [self.corpus],
                                  self.directory / "sessions", sessions=2)
        self.assertEqual([session.take_count for session in sessions], [2, 2])
        for session in sessions:
            report = validate_session(SessionPackage(session.root))
            failed = [check.name for check in report.checks if not check.passed]
            self.assertEqual(failed, [], f"{session.root.name} failed: {failed}")
            self.assertTrue(report.accepted)

    def test_session_metadata_records_capture_provenance(self) -> None:
        session = write_session(self.directory / "one", self.capture, "standard",
                                [("take-0001", di_signal(9))])
        metadata = json.loads((session.root / "session.json").read_text(encoding="utf-8"))
        self.assertEqual(metadata["latencySamples"], session.latency_samples)
        self.assertEqual(metadata["targetSettings"]["captureFile"], "amp.nam")
        self.assertEqual(metadata["targetSettings"]["tier"], "standard")
        self.assertEqual(metadata["targetSettings"]["modeledBy"], "tests")
        self.assertIn("permission", metadata["notes"])
        self.assertLess(metadata["inputCalibrationDb"], 0.0)

    def test_declared_latency_matches_what_the_capture_actually_delays_by(self) -> None:
        from nts_ml.alignment import measure_alignment
        from nts_ml.datasets.audio import WaveReader
        from nts_ml.nam.distill import measure_latency
        session = write_session(self.directory / "lat", self.capture, "standard",
                                [("take-0001", di_signal(11))])
        declared = measure_latency(self.capture, "standard")
        self.assertEqual(session.latency_samples, declared)
        source = WaveReader(session.root / "input" / "take-0001.wav").read()
        processed = WaveReader(session.root / "output" / "take-0001.wav").read()
        alignment = measure_alignment(source, processed, 48_000, max_lag=24_000)
        self.assertEqual(alignment.global_latency_samples, declared)
        self.assertEqual(alignment.drift_ppm, 0.0)

    def test_idle_dc_is_recorded_rather_than_written_into_the_noise_floor(self) -> None:
        from nts_ml.datasets.audio import WaveReader
        session = write_session(self.directory / "dc", self.capture, "standard",
                                [("take-0001", di_signal(3))])
        silence = WaveReader(session.root / "calibration" / "silence.wav").read()
        self.assertLess(abs(float(np.mean(silence))), 1.0e-4)
        metadata = json.loads((session.root / "session.json").read_text(encoding="utf-8"))
        self.assertIn("idleDcOffset", metadata["targetSettings"])

    def test_a_single_di_file_is_refused_rather_than_split(self) -> None:
        single = self.directory / "single"; single.mkdir()
        write_pcm_wave(single / "only.wav", di_signal(1), 48_000, 24)
        with self.assertRaises(ValueError) as raised:
            build_sessions(self.capture, "standard", [single], self.directory / "bad", sessions=2)
        self.assertIn("leaks validation", str(raised.exception))

    def test_mismatched_di_sample_rate_is_refused(self) -> None:
        wrong = self.directory / "wrong"; wrong.mkdir()
        for index in range(2):
            write_pcm_wave(wrong / f"di-{index}.wav", di_signal(index, sample_rate=44_100),
                           44_100, 24)
        with self.assertRaises(ValueError) as raised:
            build_sessions(self.capture, "standard", [wrong], self.directory / "bad2", sessions=2)
        self.assertIn("resample", str(raised.exception))

    def test_blocked_rendering_matches_whole_buffer_rendering(self) -> None:
        signal = di_signal(5)
        self.assertLess(float(np.max(np.abs(render(self.capture, "standard", signal, 512)
                                             - render(self.capture, "standard", signal)))), 1.0e-6)

    def test_end_to_end_distillation_produces_a_loadable_artifact(self) -> None:
        config = Path(__file__).resolve().parents[1] / "configs" / "nam-distill-lstm.toml"
        tiny = json.loads(config.read_text(encoding="utf-8")) if config.suffix == ".json" else None
        self.assertIsNone(tiny)  # configs are TOML; guard against a silent format change
        fast = self.directory / "fast.toml"
        fast.write_text(config.read_text(encoding="utf-8")
                        .replace("hidden_size = 48", "hidden_size = 4")
                        .replace("epochs = 40", "epochs = 1")
                        .replace("chunk_samples = 8192", "chunk_samples = 2048")
                        .replace("history_samples = 4096", "history_samples = 512"), encoding="utf-8")
        artifact = distill(self.capture_path, [self.corpus], self.directory / "run", fast,
                           sessions=2, repository=self.directory)
        manifest = validate_artifact(artifact)
        self.assertEqual(manifest.sample_rate, 48_000)
        self.assertLess(manifest.expected_input_rms_db, 0.0)
        self.assertIn("permission", (artifact / "license.txt").read_text(encoding="utf-8"))


class CatalogueTests(unittest.TestCase):
    def test_settings_are_recovered_from_the_corpus_naming_conventions(self) -> None:
        cases = {
            "JCM800 2203 - P5 B5 M5 T5 MV6 G9 - AZG - 700": {
                "controls": {"presence": 5, "bass": 5, "mid": 5, "treble": 5, "master": 6, "gain": 9}},
            "Fender Super Reverb_ EQ Flat, Volume 3, sm57": {
                "controls": {"volume": 3}, "microphones": ["SM57"]},
            "Full Rig Peavey 5150 MXR Mesa OS SM58 - jp_is_out_of_tune": {
                "boost": "MXR", "microphones": ["SM58"], "fullRig": True},
            "[AMP] Mes.BADLND-S 100W BOLD CRUSH Divine Sheep #01 - DI": {
                "stage": "amp", "watts": 100, "channel": "crush", "directOut": True},
            "Roland JC 120B Jazz Chorus_ Bright Off, SM57": {"bright": False, "microphones": ["SM57"]},
        }
        for name, expected in cases.items():
            with self.subTest(name=name):
                settings = parse_settings(name)
                for key, value in expected.items():
                    self.assertEqual(settings.get(key), value, f"{key} in {name}: {settings}")

    def test_unparsed_names_yield_no_invented_settings(self) -> None:
        self.assertEqual(parse_settings("Reverb"), {})

    def test_catalogue_indexes_captures_and_records_rejections(self) -> None:
        directory = Path(tempfile.mkdtemp())
        write_document(directory, build_document(channels=2), "Amp - P5 B5 M5 T5 MV6 G3.nam")
        broken = build_document(); broken["version"] = "0.5.2"
        write_document(directory, broken, "broken.nam")
        catalogue = build_catalogue([directory])
        self.assertEqual(catalogue["summary"]["count"], 1)
        self.assertEqual(catalogue["summary"]["rejected"], 1)
        entry = catalogue["captures"][0]
        self.assertEqual(entry["settings"]["controls"]["gain"], 3)
        self.assertEqual(sorted(entry["tiers"]), ["lite", "standard"])
        self.assertEqual(entry["tiers"]["standard"]["channels"], 2)
        self.assertEqual(len(entry["sha256"]), 64)


if __name__ == "__main__":
    unittest.main()
