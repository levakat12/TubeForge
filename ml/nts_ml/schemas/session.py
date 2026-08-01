from __future__ import annotations

import json
import math
import uuid
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any


@dataclass(slots=True)
class SessionMetadata:
    schema_version: int
    session_id: str
    sample_rate: int
    bit_depth: int
    instrument: str
    input_device: str
    target_type: str
    target_name: str
    target_settings: dict[str, Any] = field(default_factory=dict)
    input_calibration_db: float = 0.0
    output_calibration_db: float = 0.0
    latency_samples: int = 0
    notes: str = ""

    @classmethod
    def from_dict(cls, data: dict[str, Any]) -> SessionMetadata:
        required = (
            "schemaVersion", "sessionId", "sampleRate", "bitDepth", "instrument",
            "inputDevice", "targetType", "targetName",
        )
        missing = [key for key in required if key not in data]
        if missing:
            raise ValueError(f"Missing session metadata fields: {', '.join(missing)}")
        metadata = cls(
            schema_version=int(data["schemaVersion"]),
            session_id=str(data["sessionId"]),
            sample_rate=int(data["sampleRate"]),
            bit_depth=int(data["bitDepth"]),
            instrument=str(data["instrument"]),
            input_device=str(data["inputDevice"]),
            target_type=str(data["targetType"]),
            target_name=str(data["targetName"]),
            target_settings=dict(data.get("targetSettings", {})),
            input_calibration_db=float(data.get("inputCalibrationDb", 0.0)),
            output_calibration_db=float(data.get("outputCalibrationDb", 0.0)),
            latency_samples=int(data.get("latencySamples", 0)),
            notes=str(data.get("notes", "")),
        )
        metadata.validate()
        return metadata

    @classmethod
    def load(cls, path: Path) -> SessionMetadata:
        return cls.from_dict(json.loads(path.read_text(encoding="utf-8")))

    def validate(self) -> None:
        if self.schema_version != 1:
            raise ValueError(f"Unsupported session schema version: {self.schema_version}")
        try:
            uuid.UUID(self.session_id)
        except ValueError as error:
            raise ValueError("sessionId must be a UUID") from error
        if not 8_000 <= self.sample_rate <= 384_000:
            raise ValueError("sampleRate is outside the supported range")
        if self.bit_depth not in (16, 24, 32):
            raise ValueError("bitDepth must be 16, 24, or 32")
        if self.instrument not in ("guitar", "bass", "other"):
            raise ValueError("instrument must be guitar, bass, or other")
        if not self.input_device or not self.target_type or not self.target_name:
            raise ValueError("device and target fields cannot be empty")
        if self.latency_samples < 0:
            raise ValueError("latencySamples cannot be negative")
        for value in (self.input_calibration_db, self.output_calibration_db):
            if not math.isfinite(value):
                raise ValueError("calibration values must be finite")

    @property
    def documented_polarity_inversion(self) -> bool:
        return bool(self.target_settings.get("polarityInverted", False))

    def to_dict(self) -> dict[str, Any]:
        return {
            "schemaVersion": self.schema_version,
            "sessionId": self.session_id,
            "sampleRate": self.sample_rate,
            "bitDepth": self.bit_depth,
            "instrument": self.instrument,
            "inputDevice": self.input_device,
            "targetType": self.target_type,
            "targetName": self.target_name,
            "targetSettings": self.target_settings,
            "inputCalibrationDb": self.input_calibration_db,
            "outputCalibrationDb": self.output_calibration_db,
            "latencySamples": self.latency_samples,
            "notes": self.notes,
        }


@dataclass(slots=True)
class AlignmentResult:
    global_latency_samples: int
    drift_ppm: float
    confidence: float
    polarity: int = 1
    method: str = "cross_correlation"
    local_latencies: list[int] = field(default_factory=list)

    def to_dict(self) -> dict[str, Any]:
        return {
            "globalLatencySamples": self.global_latency_samples,
            "driftPpm": self.drift_ppm,
            "confidence": self.confidence,
            "polarity": self.polarity,
            "method": self.method,
            "localLatencies": self.local_latencies,
        }


@dataclass(slots=True)
class QualityCheck:
    name: str
    passed: bool
    value: float | int | str | bool | None
    limit: float | int | str | bool | None
    message: str = ""


@dataclass(slots=True)
class ValidationReport:
    schema_version: int
    session_id: str
    accepted: bool
    checks: list[QualityCheck]
    alignment: AlignmentResult | None
    takes: list[dict[str, Any]] = field(default_factory=list)

    def to_dict(self) -> dict[str, Any]:
        return {
            "schemaVersion": self.schema_version,
            "sessionId": self.session_id,
            "accepted": self.accepted,
            "checks": [asdict(check) for check in self.checks],
            "alignment": self.alignment.to_dict() if self.alignment else None,
            "takes": self.takes,
        }

    def save(self, path: Path) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(self.to_dict(), indent=2, sort_keys=True) + "\n", encoding="utf-8")
