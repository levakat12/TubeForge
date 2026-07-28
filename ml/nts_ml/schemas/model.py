from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Any
import json
import math


@dataclass(slots=True)
class ModelManifest:
    model_format_version: int = 1
    architecture: str = "tiny_tanh_rnn"
    sample_rate: int = 48_000
    input_channels: int = 1
    output_channels: int = 1
    state_size: int = 8
    latency_samples: int = 0
    expected_input_rms_db: float = -21.0
    parameter_schema: list[dict[str, Any]] = field(default_factory=list)
    sha256: str = ""

    def validate(self) -> None:
        if self.model_format_version not in (1, 2):
            raise ValueError("Unsupported model format version")
        if self.architecture not in ("tiny_tanh_rnn", "conditioned_tanh_rnn", "conditioned_lstm",
                                     "conditioned_gru", "causal_tcn"):
            raise ValueError("Unsupported model architecture")
        if not 8_000 <= self.sample_rate <= 384_000:
            raise ValueError("Invalid model sample rate")
        if self.input_channels < 1 or self.output_channels < 1 or self.state_size < 1:
            raise ValueError("Model channel and state sizes must be positive")
        if self.latency_samples < 0 or not math.isfinite(self.expected_input_rms_db):
            raise ValueError("Invalid model latency or input level")
        if self.sha256 and (len(self.sha256) != 64 or any(c not in "0123456789abcdef" for c in self.sha256)):
            raise ValueError("sha256 must be a lowercase SHA-256 digest")

    def to_dict(self) -> dict[str, Any]:
        return {
            "modelFormatVersion": self.model_format_version,
            "architecture": self.architecture,
            "sampleRate": self.sample_rate,
            "inputChannels": self.input_channels,
            "outputChannels": self.output_channels,
            "stateSize": self.state_size,
            "latencySamples": self.latency_samples,
            "expectedInputRmsDb": self.expected_input_rms_db,
            "parameterSchema": self.parameter_schema,
            "sha256": self.sha256,
        }

    @classmethod
    def from_dict(cls, data: dict[str, Any]) -> "ModelManifest":
        manifest = cls(
            model_format_version=int(data["modelFormatVersion"]),
            architecture=str(data["architecture"]),
            sample_rate=int(data["sampleRate"]),
            input_channels=int(data["inputChannels"]),
            output_channels=int(data["outputChannels"]),
            state_size=int(data["stateSize"]),
            latency_samples=int(data.get("latencySamples", 0)),
            expected_input_rms_db=float(data.get("expectedInputRmsDb", -21.0)),
            parameter_schema=list(data.get("parameterSchema", [])),
            sha256=str(data.get("sha256", "")),
        )
        manifest.validate()
        return manifest

    @classmethod
    def load(cls, path: Path) -> "ModelManifest":
        return cls.from_dict(json.loads(path.read_text(encoding="utf-8")))

    def save(self, path: Path) -> None:
        self.validate()
        path.write_text(json.dumps(self.to_dict(), indent=2, sort_keys=True) + "\n", encoding="utf-8")
