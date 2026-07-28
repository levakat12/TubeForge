from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Any
import csv
import hashlib
import json
import math
import random
import shutil

import numpy as np

from ..datasets.audio import WaveReader, write_pcm_wave


LISTENING_CATEGORIES = (
    "clean-guitar-arpeggio",
    "crunch-guitar-open-chords",
    "high-gain-guitar-palm-mutes",
    "lead-guitar-bends-sustain",
    "clean-fingerstyle-bass",
    "picked-bass",
    "distorted-bass-sustained-fundamentals",
    "slap-bass-transients",
)
SCORE_FIELDS = ("pickResponse", "lowEndStability", "chordSeparation", "sustain",
                "noise", "harshness", "overallPreference")


@dataclass(frozen=True, slots=True)
class AbxSource:
    category: str
    source_id: str
    candidate_a: Path
    candidate_b: Path


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while block := source.read(1024 * 1024):
            digest.update(block)
    return digest.hexdigest()


def _biquad(audio: np.ndarray, coefficients: tuple[float, ...]) -> np.ndarray:
    b0, b1, b2, a1, a2 = coefficients
    output = np.empty_like(audio, dtype=np.float64)
    for channel in range(audio.shape[1]):
        z1 = z2 = 0.0
        for index, value in enumerate(audio[:, channel]):
            result = b0 * float(value) + z1
            z1 = b1 * float(value) - a1 * result + z2
            z2 = b2 * float(value) - a2 * result
            output[index, channel] = result
    return output


def _k_weighting_coefficients(sample_rate: int) -> tuple[tuple[float, ...], tuple[float, ...]]:
    frequency, gain_db, q = 1681.974450955533, 3.999843853973347, 0.7071752369554196
    k = math.tan(math.pi * frequency / sample_rate)
    vh, denominator = 10.0 ** (gain_db / 20.0), 1.0 + k / q + k * k
    vb = vh ** 0.4996667741545416
    shelf = ((vh + vb * k / q + k * k) / denominator,
             2.0 * (k * k - vh) / denominator,
             (vh - vb * k / q + k * k) / denominator,
             2.0 * (k * k - 1.0) / denominator,
             (1.0 - k / q + k * k) / denominator)
    frequency, q = 38.13547087602444, 0.5003270373238773
    k = math.tan(math.pi * frequency / sample_rate)
    denominator = 1.0 + k / q + k * k
    high_pass = (1.0 / denominator, -2.0 / denominator, 1.0 / denominator,
                 2.0 * (k * k - 1.0) / denominator,
                 (1.0 - k / q + k * k) / denominator)
    return shelf, high_pass


def integrated_loudness(audio: np.ndarray, sample_rate: int, discard_seconds: float = 0.5) -> float:
    """Gated integrated BS.1770 loudness with 400 ms blocks and 100 ms hops."""
    values = np.asarray(audio, dtype=np.float64)
    if values.ndim == 1:
        values = values[:, None]
    block, hop = int(round(0.4 * sample_rate)), int(round(0.1 * sample_rate))
    shelf, high_pass = _k_weighting_coefficients(sample_rate)
    weighted = _biquad(_biquad(values, shelf), high_pass)
    weighted = weighted[int(round(discard_seconds * sample_rate)):]
    if len(weighted) < block:
        raise ValueError("Audio is too short for loudness measurement after warm-up discard")
    energies: list[float] = []
    for offset in range(0, len(weighted) - block + 1, hop):
        energy = float(np.sum(np.mean(np.square(weighted[offset:offset + block]), axis=0)))
        loudness = -0.691 + 10.0 * math.log10(max(energy, 1.0e-15))
        if loudness >= -70.0:
            energies.append(energy)
    if not energies:
        raise ValueError("Audio is below the BS.1770 absolute gate")
    relative_gate = -0.691 + 10.0 * math.log10(float(np.mean(energies))) - 10.0
    gated = [energy for energy in energies
             if -0.691 + 10.0 * math.log10(energy) >= max(-70.0, relative_gate)]
    if not gated:
        raise ValueError("Audio has no blocks above the BS.1770 relative gate")
    return -0.691 + 10.0 * math.log10(float(np.mean(gated)))


def _load_pair(source: AbxSource) -> tuple[np.ndarray, np.ndarray, int, int]:
    reader_a, reader_b = WaveReader(source.candidate_a), WaveReader(source.candidate_b)
    if reader_a.info.sample_rate != reader_b.info.sample_rate:
        raise ValueError(f"Sample-rate mismatch for {source.source_id}")
    if reader_a.info.channels != reader_b.info.channels:
        raise ValueError(f"Channel-count mismatch for {source.source_id}")
    if reader_a.info.frames != reader_b.info.frames:
        raise ValueError(f"Length mismatch for {source.source_id}")
    if reader_a.info.bit_depth != reader_b.info.bit_depth:
        raise ValueError(f"Bit-depth mismatch for {source.source_id}")
    return reader_a.read(), reader_b.read(), reader_a.info.sample_rate, reader_a.info.bit_depth


def create_abx_pack(sources: list[AbxSource], destination: Path, trials_per_source: int = 12,
                    seed: int = 0x54554245, tolerance_lu: float = 0.2) -> dict[str, Any]:
    if trials_per_source < 12:
        raise ValueError("The listening protocol requires at least 12 trials per source")
    unknown = {source.category for source in sources} - set(LISTENING_CATEGORIES)
    if unknown:
        raise ValueError(f"Unknown listening categories: {sorted(unknown)}")
    if len({source.source_id for source in sources}) != len(sources):
        raise ValueError("sourceId values must be unique")
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=False)
    rng = random.Random(seed)
    public_trials: list[dict[str, Any]] = []
    answers: list[dict[str, Any]] = []
    loudness_records: list[dict[str, Any]] = []
    trial_number = 0
    for source in sources:
        candidate_a, candidate_b, sample_rate, bit_depth = _load_pair(source)
        loudness_a = integrated_loudness(candidate_a, sample_rate)
        loudness_b = integrated_loudness(candidate_b, sample_rate)
        target = min(loudness_a, loudness_b)
        gain_a, gain_b = 10.0 ** ((target - loudness_a) / 20.0), 10.0 ** ((target - loudness_b) / 20.0)
        matched_a, matched_b = candidate_a * gain_a, candidate_b * gain_b
        measured_a, measured_b = integrated_loudness(matched_a, sample_rate), integrated_loudness(matched_b, sample_rate)
        if abs(measured_a - measured_b) > tolerance_lu:
            raise ValueError(f"Loudness match failed for {source.source_id}")
        if max(float(np.max(np.abs(matched_a))), float(np.max(np.abs(matched_b)))) >= 1.0:
            raise ValueError(f"Loudness matching would clip {source.source_id}")
        loudness_records.append({"sourceId": source.source_id, "category": source.category,
            "originalLufs": {"candidateA": loudness_a, "candidateB": loudness_b},
            "matchedLufs": {"candidateA": measured_a, "candidateB": measured_b},
            "trimDb": {"candidateA": 20.0 * math.log10(gain_a), "candidateB": 20.0 * math.log10(gain_b)}})
        for _ in range(trials_per_source):
            trial_number += 1
            trial_id = f"trial-{trial_number:04d}"
            trial_directory = destination / "audio" / trial_id
            public_a_is_original_a = bool(rng.getrandbits(1))
            public_a = matched_a if public_a_is_original_a else matched_b
            public_b = matched_b if public_a_is_original_a else matched_a
            answer = "A" if bool(rng.getrandbits(1)) else "B"
            paths = {name: trial_directory / f"{name}.wav" for name in ("A", "B", "X")}
            for name, audio in (("A", public_a), ("B", public_b),
                                ("X", public_a if answer == "A" else public_b)):
                write_pcm_wave(paths[name], audio, sample_rate, bit_depth)
            public_trials.append({"trialId": trial_id, "sourceId": source.source_id,
                "category": source.category, "audio": {name: str(path.relative_to(destination)).replace("\\", "/")
                for name, path in paths.items()}})
            answers.append({"trialId": trial_id, "answer": answer,
                "assignment": {"A": "candidateA" if public_a_is_original_a else "candidateB",
                               "B": "candidateB" if public_a_is_original_a else "candidateA"},
                "hashes": {name: _sha256(path) for name, path in paths.items()}})
    rng.shuffle(public_trials)
    manifest = {"schemaVersion": 1, "seed": seed, "trialsPerSource": trials_per_source,
                "toleranceLu": tolerance_lu, "scoreFields": list(SCORE_FIELDS), "trials": public_trials}
    (destination / "trials.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    (destination / "answer-key.json").write_text(json.dumps(
        {"schemaVersion": 1, "loudness": loudness_records, "answers": answers}, indent=2) + "\n", encoding="utf-8")
    shutil.copyfile(Path(__file__).with_name("abx-player.html"), destination / "player.html")
    fields = ["participantId", "trialId", "selection", "confidence", *SCORE_FIELDS, "comments"]
    with (destination / "responses.csv").open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=fields)
        writer.writeheader()
        for trial in public_trials:
            writer.writerow({"trialId": trial["trialId"]})
    return manifest


def _binomial_tail(correct: int, total: int) -> float:
    return sum(math.comb(total, index) for index in range(correct, total + 1)) / (2.0 ** total)


def _wilson_interval(correct: int, total: int) -> tuple[float, float]:
    if total == 0:
        return 0.0, 0.0
    z, proportion = 1.959963984540054, correct / total
    denominator = 1.0 + z * z / total
    center = (proportion + z * z / (2.0 * total)) / denominator
    margin = z * math.sqrt(proportion * (1.0 - proportion) / total + z * z / (4.0 * total * total)) / denominator
    return center - margin, center + margin


def analyze_abx_results(pack: Path, responses: Path, report_path: Path) -> dict[str, Any]:
    pack = Path(pack)
    manifest = json.loads((pack / "trials.json").read_text(encoding="utf-8"))
    key = json.loads((pack / "answer-key.json").read_text(encoding="utf-8"))
    trials = {trial["trialId"]: trial for trial in manifest["trials"]}
    answers = {answer["trialId"]: answer["answer"] for answer in key["answers"]}
    with Path(responses).open(newline="", encoding="utf-8-sig") as source:
        rows = [row for row in csv.DictReader(source) if row.get("selection", "").strip()]
    grouped: dict[str, list[dict[str, str]]] = {}
    seen: set[tuple[str, str]] = set()
    for row in rows:
        trial_id, participant = row.get("trialId", "").strip(), row.get("participantId", "").strip()
        if trial_id not in trials or row.get("selection", "").strip().upper() not in ("A", "B") or not participant:
            raise ValueError(f"Invalid response row for trial {trial_id!r}")
        if (participant, trial_id) in seen:
            raise ValueError(f"Duplicate response for participant/trial {(participant, trial_id)}")
        seen.add((participant, trial_id))
        grouped.setdefault(trials[trial_id]["category"], []).append(row)

    def summarize(group: list[dict[str, str]]) -> dict[str, Any]:
        total = len(group)
        correct = sum(row["selection"].strip().upper() == answers[row["trialId"].strip()] for row in group)
        lower, upper = _wilson_interval(correct, total)
        means: dict[str, float | None] = {}
        for field in SCORE_FIELDS:
            values = [float(row[field]) for row in group if row.get(field, "").strip()]
            if any(not 1.0 <= value <= 7.0 for value in values):
                raise ValueError(f"{field} scores must be between 1 and 7")
            means[field] = float(np.mean(values)) if values else None
        probability = _binomial_tail(correct, total) if total else 1.0
        return {"trials": total, "correct": correct, "accuracy": correct / total if total else 0.0,
                "binomialP": probability, "identifiable": total >= 12 and probability < 0.05,
                "wilson95": [lower, upper], "scoreMeans": means}

    categories = {category: summarize(grouped.get(category, [])) for category in LISTENING_CATEGORIES}
    report = {"schemaVersion": 1, "participants": sorted({row["participantId"] for row in rows}),
              "aggregate": summarize(rows), "categories": categories,
              "complete": all(categories[category]["trials"] >= 12 for category in LISTENING_CATEGORIES)}
    report_path = Path(report_path)
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return report


def load_abx_sources(manifest_path: Path) -> list[AbxSource]:
    path = Path(manifest_path)
    data, base = json.loads(path.read_text(encoding="utf-8")), path.parent
    return [AbxSource(str(item["category"]), str(item["sourceId"]),
                      (base / item["candidateA"]).resolve(), (base / item["candidateB"]).resolve())
            for item in data["sources"]]
