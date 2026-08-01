from __future__ import annotations

import json
import math
import time
from collections.abc import Iterable
from pathlib import Path

import numpy as np
from numpy.typing import NDArray

from .suite import evaluate


def _low_frequency_error(prediction: np.ndarray, target: np.ndarray, sample_rate: int) -> float:
    count = min(len(prediction), len(target)); size = 1 << max(6, count - 1).bit_length()
    predicted = np.abs(np.fft.rfft(prediction[:count], size)); expected = np.abs(np.fft.rfft(target[:count], size))
    frequencies = np.fft.rfftfreq(size, 1.0 / sample_rate); low = frequencies <= 200.0
    return float(np.linalg.norm(predicted[low] - expected[low]) / max(np.linalg.norm(expected[low]), 1.0e-12))


def _transient_error(prediction: np.ndarray, target: np.ndarray) -> float:
    first, second = np.diff(prediction, prepend=prediction[0]), np.diff(target, prepend=target[0])
    return float(np.mean(np.abs(first - second)))


def _parameter_memory(model) -> int:
    return sum(value.nbytes for value in vars(model).values() if isinstance(value, np.ndarray))


def generate_quality_report(model,
                            clips: Iterable[tuple[str, NDArray[np.floating], NDArray[np.floating]]],
                            report_path: Path | None = None,
                            expected_input_rms_db: float = -21.0,
                            block_size: int = 64) -> dict[str, object]:
    material = [(name, np.asarray(source, dtype=np.float32).reshape(-1),
                 np.asarray(target, dtype=np.float32).reshape(-1)) for name, source, target in clips]
    base = evaluate(model, material)
    low_errors, transient_errors, silence_levels = [], [], []
    for name, source, target in material:
        model.reset(); prediction = model.process(source)
        low_errors.append(_low_frequency_error(prediction, target, model.sample_rate))
        transient_errors.append(_transient_error(prediction, target))
        if name == "silence" or float(np.sqrt(np.mean(np.square(target)))) < 1.0e-5:
            rms = max(float(np.sqrt(np.mean(np.square(prediction, dtype=np.float64)))), 1.0e-12)
            silence_levels.append(20.0 * math.log10(rms))
    benchmark = np.random.default_rng(51).normal(0.0, 0.08, block_size).astype(np.float32)
    iterations = 300; model.reset(); started = time.perf_counter()
    for _ in range(iterations): model.process(benchmark)
    average_seconds = (time.perf_counter() - started) / iterations
    budget_seconds = block_size / model.sample_rate
    aggregate = base["aggregate"]
    confidence_score = max(0.0, min(1.0, 1.0
        - float(aggregate["waveformL1"]) * 3.0
        - float(np.mean(low_errors)) * 0.15
        - float(np.mean(transient_errors)) * 2.0
        - max(0.0, average_seconds / budget_seconds - 0.5) * 0.25))
    report = {"schemaVersion": 1, "architecture": model.architecture,
              "supportedSampleRate": model.sample_rate, "expectedInputRmsDb": expected_input_rms_db,
              "validation": aggregate, "lowFrequencyError": float(np.mean(low_errors)),
              "transientError": float(np.mean(transient_errors)),
              "silenceNoiseDbfs": max(silence_levels, default=-240.0),
              "cpu": {"blockSize": block_size, "averageCallbackUs": average_seconds * 1.0e6,
                      "callbackBudgetPercent": average_seconds / budget_seconds * 100.0},
              "memoryBytes": _parameter_memory(model), "confidence": confidence_score,
              "confidenceRating": "high" if confidence_score >= 0.8 else (
                  "medium" if confidence_score >= 0.55 else "low"), "clips": base["clips"]}
    if report_path is not None:
        report_path.parent.mkdir(parents=True, exist_ok=True)
        report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return report
