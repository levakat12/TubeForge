from __future__ import annotations

import json
from collections.abc import Iterable
from dataclasses import asdict, dataclass
from pathlib import Path

import numpy as np
from numpy.typing import NDArray

from ..datasets.audio import WaveReader, write_pcm_wave
from ..losses import dc_penalty, loudness_difference, spectral_convergence, waveform_l1, waveform_l2
from ..models import AudioModel

REQUIRED_CLIPS = (
    "single-notes", "chords", "palm-mutes", "muted-scratches", "bends", "harmonics",
    "clean-bass", "distorted-bass", "pick-attack", "fingerstyle", "silence", "impulse",
    "sine-sweep", "multitone",
)


@dataclass(slots=True)
class ClipMetrics:
    clip: str
    waveform_l1: float
    waveform_l2: float
    maximum_absolute_error: float
    spectral_convergence: float
    dc_penalty: float
    loudness_difference_db: float


def load_evaluation_pairs(root: Path) -> list[tuple[str, NDArray[np.float32], NDArray[np.float32]]]:
    pairs: list[tuple[str, NDArray[np.float32], NDArray[np.float32]]] = []
    missing: list[str] = []
    for name in REQUIRED_CLIPS:
        source_path, target_path = root / "input" / f"{name}.wav", root / "output" / f"{name}.wav"
        if not source_path.is_file() or not target_path.is_file():
            missing.append(name)
            continue
        pairs.append((name, WaveReader(source_path).read()[:, 0], WaveReader(target_path).read()[:, 0]))
    if missing:
        raise ValueError(f"Evaluation dataset is missing clips: {', '.join(missing)}")
    return pairs


def evaluate(model: AudioModel,
             clips: Iterable[tuple[str, NDArray[np.floating], NDArray[np.floating]]],
             report_path: Path | None = None) -> dict[str, object]:
    results: list[ClipMetrics] = []
    for name, source, target in clips:
        model.reset()
        prediction = model.process(source)
        count = min(prediction.size, target.size)
        prediction, expected = prediction[:count], np.asarray(target)[:count]
        results.append(ClipMetrics(
            name, waveform_l1(prediction, expected), waveform_l2(prediction, expected),
            float(np.max(np.abs(prediction - expected))), spectral_convergence(prediction, expected),
            dc_penalty(prediction), loudness_difference(prediction, expected),
        ))
    aggregate = {
        "waveformL1": float(np.mean([item.waveform_l1 for item in results])),
        "waveformL2": float(np.mean([item.waveform_l2 for item in results])),
        "maximumAbsoluteError": max((item.maximum_absolute_error for item in results), default=0.0),
        "spectralConvergence": float(np.mean([item.spectral_convergence for item in results])),
        "dcPenalty": float(np.mean([item.dc_penalty for item in results])),
        "loudnessDifferenceDb": float(np.mean([item.loudness_difference_db for item in results])),
    }
    report: dict[str, object] = {"schemaVersion": 1, "clips": [asdict(item) for item in results],
                                 "aggregate": aggregate}
    if report_path is not None:
        report_path.parent.mkdir(parents=True, exist_ok=True)
        report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return report


def generate_evaluation_inputs(root: Path, sample_rate: int = 48_000,
                               seconds: float = 1.0) -> None:
    samples = max(2048, int(sample_rate * seconds))
    time = np.arange(samples, dtype=np.float64) / sample_rate
    random = np.random.default_rng(904)
    clips: dict[str, NDArray[np.float64]] = {
        "single-notes": 0.2 * np.sin(2 * np.pi * 220 * time) * np.exp(-2 * time),
        "chords": 0.08 * sum(np.sin(2 * np.pi * frequency * time) for frequency in (110, 138.59, 164.81)),
        "palm-mutes": 0.25 * np.sin(2 * np.pi * 82.4 * time) * np.exp(-18 * (time % 0.2)),
        "muted-scratches": random.normal(0, 0.05, samples) * (np.sin(2 * np.pi * 7 * time) > 0),
        "bends": 0.16 * np.sin(2 * np.pi * (180 * time + 70 * time * time)),
        "harmonics": 0.12 * np.sin(2 * np.pi * 660 * time) * np.exp(-3 * time),
        "clean-bass": 0.24 * np.sin(2 * np.pi * 55 * time),
        "distorted-bass": 0.18 * np.tanh(3 * np.sin(2 * np.pi * 65.4 * time)),
        "pick-attack": 0.2 * np.sin(2 * np.pi * 247 * time) * np.exp(-8 * time),
        "fingerstyle": 0.18 * np.sin(2 * np.pi * 98 * time) * np.exp(-4 * time),
        "silence": np.zeros(samples),
        "impulse": np.pad(np.array([0.8]), (0, samples - 1)),
        "sine-sweep": 0.12 * np.sin(2 * np.pi * (20 * time + 0.5 * (18_000 - 20) / seconds * time * time)),
        "multitone": 0.04 * sum(np.sin(2 * np.pi * frequency * time) for frequency in (80, 440, 1200, 5000)),
    }
    for name, clip in clips.items():
        write_pcm_wave(root / "input" / f"{name}.wav", clip, sample_rate, 24)
