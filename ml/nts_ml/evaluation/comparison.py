from __future__ import annotations

import json
import math
from pathlib import Path
from typing import Any

import numpy as np
from numpy.typing import NDArray

from ..datasets.audio import write_pcm_wave
from .abx import integrated_loudness


def render_ab_comparison(model, source: NDArray[np.floating], target: NDArray[np.floating],
                         destination: Path, sample_rate: int, bit_depth: int = 24) -> dict[str, Any]:
    dry, expected = np.asarray(source, dtype=np.float32).reshape(-1), np.asarray(target, dtype=np.float32).reshape(-1)
    count = min(dry.size, expected.size); dry, expected = dry[:count], expected[:count]
    model.reset(); prediction = model.process(dry)
    discard = 0.5 if count >= int(sample_rate * 0.9) else 0.0
    target_lufs = integrated_loudness(expected, sample_rate, discard)
    model_lufs = integrated_loudness(prediction, sample_rate, discard)
    gain = 10.0 ** ((target_lufs - model_lufs) / 20.0)
    matched = prediction * gain
    peak = float(np.max(np.abs(matched)))
    if peak >= 1.0: matched *= 0.999 / peak
    outputs = {"target": expected, "model": prediction, "bypass-di": dry,
               "model-loudness-matched": matched, "null-difference": expected - matched}
    destination.mkdir(parents=True, exist_ok=True)
    for name, audio in outputs.items(): write_pcm_wave(destination / f"{name}.wav", audio, sample_rate, bit_depth)
    report = {"schemaVersion": 1, "targetLufs": target_lufs, "modelLufs": model_lufs,
              "modelTrimDb": 20.0 * math.log10(gain), "files": {name: f"{name}.wav" for name in outputs}}
    (destination / "comparison.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return report
