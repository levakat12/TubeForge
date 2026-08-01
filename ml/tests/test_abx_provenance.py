from __future__ import annotations

import csv
import json
import tempfile
import unittest
import uuid
from pathlib import Path

import numpy as np
from _fixtures import create_session

from nts_ml.datasets import SessionPackage, audit_real_corpus, file_sha256, write_pcm_wave
from nts_ml.evaluation import (
    LISTENING_CATEGORIES,
    AbxSource,
    analyze_abx_results,
    create_abx_pack,
    integrated_loudness,
)


class AbxProvenanceTests(unittest.TestCase):
    def test_loudness_matched_blinded_pack_and_statistics(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            rate = 48_000
            time = np.arange(rate, dtype=np.float32) / rate
            first = (0.10 * np.sin(2.0 * np.pi * 220.0 * time)).astype(np.float32)
            second = (0.22 * np.tanh(2.0 * np.sin(2.0 * np.pi * 220.0 * time))).astype(np.float32)
            write_pcm_wave(root / "a.wav", first, rate, 24)
            write_pcm_wave(root / "b.wav", second, rate, 24)
            self.assertGreater(abs(integrated_loudness(first, rate) - integrated_loudness(second, rate)), 3.0)
            pack = root / "pack"
            manifest = create_abx_pack([
                AbxSource(LISTENING_CATEGORIES[0], "source-1", root / "a.wav", root / "b.wav")
            ], pack, seed=123)
            self.assertEqual(len(manifest["trials"]), 12)
            self.assertTrue((pack / "player.html").is_file())
            key = json.loads((pack / "answer-key.json").read_text(encoding="utf-8"))
            self.assertLessEqual(abs(key["loudness"][0]["matchedLufs"]["candidateA"]
                                     - key["loudness"][0]["matchedLufs"]["candidateB"]), 0.2)
            answers = {item["trialId"]: item["answer"] for item in key["answers"]}
            response_path = pack / "responses.csv"
            with response_path.open(newline="", encoding="utf-8") as source:
                rows, fields = list(csv.DictReader(source)), None
            fields = ["participantId", "trialId", "selection", "confidence", "pickResponse",
                      "lowEndStability", "chordSeparation", "sustain", "noise", "harshness",
                      "overallPreference", "comments"]
            for row in rows:
                row.update({"participantId": "listener-1", "selection": answers[row["trialId"]],
                            "confidence": "5", "overallPreference": "6"})
            with response_path.open("w", newline="", encoding="utf-8") as output:
                writer = csv.DictWriter(output, fieldnames=fields); writer.writeheader(); writer.writerows(rows)
            report = analyze_abx_results(pack, response_path, root / "report.json")
            category = report["categories"][LISTENING_CATEGORIES[0]]
            self.assertEqual(category["correct"], 12)
            self.assertLess(category["binomialP"], 0.05)
            self.assertTrue(category["identifiable"])
            self.assertFalse(report["complete"])

    def test_corpus_audit_rejects_synthetic_and_accepts_complete_provenance(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            packages: list[SessionPackage] = []
            for index, category in enumerate(LISTENING_CATEGORIES):
                session_root = create_session(root / f"session-{index}", str(uuid.uuid4()),
                                              duration=1.2, sample_rate=8_000)
                metadata_path = session_root / "session.json"
                metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
                metadata["instrument"] = "guitar" if "guitar" in category else "bass"
                metadata_path.write_text(json.dumps(metadata), encoding="utf-8")
                (session_root / "releases").mkdir()
                (session_root / "releases" / "performer-release.txt").write_text(
                    "Test-only rights release fixture", encoding="utf-8")
                input_path = session_root / "input" / "take-0001.wav"
                provenance = {"schemaVersion": 1, "takes": [{
                    "takeId": "take-0001", "performanceId": f"performance-{index}",
                    "category": category, "creator": f"Test performer {index}",
                    "licenseId": "LicenseRef-Test", "licenseUri": "https://example.invalid/license",
                    "releaseDocument": "releases/performer-release.txt", "rightsCleared": True,
                    "synthetic": False, "inputSha256": file_sha256(input_path),
                }]}
                (session_root / "provenance.json").write_text(json.dumps(provenance), encoding="utf-8")
                packages.append(SessionPackage(session_root))
            report = audit_real_corpus(packages, root / "corpus-report.json")
            self.assertTrue(report["accepted"], report["gaps"])
            provenance_path = packages[0].root / "provenance.json"
            provenance = json.loads(provenance_path.read_text(encoding="utf-8"))
            provenance["takes"][0]["synthetic"] = True
            provenance_path.write_text(json.dumps(provenance), encoding="utf-8")
            rejected = audit_real_corpus(packages)
            self.assertFalse(rejected["accepted"])
            self.assertTrue(any("synthetic" in gap for gap in rejected["gaps"]))


if __name__ == "__main__":
    unittest.main()
