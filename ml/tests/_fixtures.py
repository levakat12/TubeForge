from __future__ import annotations

from pathlib import Path
import json
import uuid

import numpy as np

from nts_ml.datasets.audio import write_pcm_wave


def create_session(root: Path, session_id: str | None = None, latency: int = 37,
                   duration: float = 1.2, sample_rate: int = 48_000,
                   performance_scale: float = 1.0) -> Path:
    for directory in ("input", "output", "calibration", "reports"):
        (root / directory).mkdir(parents=True, exist_ok=True)
    identifier = session_id or str(uuid.uuid4())
    metadata = {
        "schemaVersion": 1,
        "sessionId": identifier,
        "sampleRate": sample_rate,
        "bitDepth": 24,
        "instrument": "guitar",
        "inputDevice": "Generic interface",
        "targetType": "plugin",
        "targetName": "OriginalTarget",
        "targetSettings": {},
        "inputCalibrationDb": -3.2,
        "outputCalibrationDb": -7.1,
        "latencySamples": latency,
        "notes": "Synthetic test fixture only",
    }
    (root / "session.json").write_text(json.dumps(metadata), encoding="utf-8")
    samples = int(sample_rate * duration)
    random = np.random.default_rng(int(uuid.UUID(identifier)) & 0xffffffff)
    source = random.normal(0.0, 0.08 * performance_scale, samples).astype(np.float32)
    source = np.convolve(source, np.ones(5) / 5.0, mode="same").astype(np.float32)
    transformed = (0.75 * np.tanh(2.2 * source)).astype(np.float32)
    output = np.zeros_like(source)
    output[latency:] = transformed[:-latency]
    write_pcm_wave(root / "input" / "take-0001.wav", source, sample_rate, 24)
    write_pcm_wave(root / "output" / "take-0001.wav", output, sample_rate, 24)
    silence = random.normal(0.0, 1.0e-5, sample_rate // 2).astype(np.float32)
    write_pcm_wave(root / "calibration" / "silence.wav", silence, sample_rate, 24)
    impulse = np.zeros(sample_rate // 4, dtype=np.float32); impulse[32] = 0.5
    write_pcm_wave(root / "calibration" / "impulse.wav", impulse, sample_rate, 24)
    write_pcm_wave(root / "calibration" / "sweep.wav",
                   0.1 * np.sin(np.linspace(0.0, 300.0 * np.pi, sample_rate // 2)), sample_rate, 24)
    return root
