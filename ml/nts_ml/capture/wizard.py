from __future__ import annotations

from dataclasses import asdict, dataclass
from enum import Enum
from pathlib import Path
from typing import Any
import json
import os
import uuid

import numpy as np

from ..alignment import align_pair
from ..datasets import SessionPackage, StreamingPairedDataset, WaveReader, validate_session, write_pcm_wave
from ..evaluation import generate_quality_report
from ..export import export_model, validate_artifact
from ..models import create_model, load_model_checkpoint
from ..training import ExperimentConfig, train


class CaptureStage(str, Enum):
    configured = "configured"
    material_ready = "material-ready"
    safety_verified = "safety-verified"
    aligned = "aligned"
    trained = "trained"
    evaluated = "evaluated"
    activated = "activated"


@dataclass(slots=True)
class CaptureConfig:
    input_channel: int = 0
    return_channel: int = 0
    sample_rate: int = 48_000
    target_type: str = "amplifier"
    target_name: str = "User capture"
    expected_input_rms_db: float = -21.0
    capture_duration_seconds: float = 120.0
    controls: dict[str, float] | None = None
    feedback_routing_acknowledged: bool = False

    def validate(self) -> None:
        if self.input_channel < 0 or self.return_channel < 0 or not 8_000 <= self.sample_rate <= 192_000:
            raise ValueError("Invalid channel or sample-rate configuration")
        if not self.target_type or not self.target_name or self.capture_duration_seconds < 10.0:
            raise ValueError("Capture target and at least ten seconds of material are required")
        if not -60.0 <= self.expected_input_rms_db <= 0.0:
            raise ValueError("Expected input RMS must be between -60 and 0 dBFS")


class CaptureWizard:
    def __init__(self, root: Path):
        self.root = Path(root); self.state_path = self.root / "capture-workflow.json"

    def _save(self, stage: CaptureStage, details: dict[str, Any] | None = None) -> None:
        self.root.mkdir(parents=True, exist_ok=True)
        self.state_path.write_text(json.dumps({"schemaVersion": 1, "stage": stage.value,
            "details": details or {}}, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    def initialize(self, config: CaptureConfig) -> dict[str, Any]:
        config.validate(); self.root.mkdir(parents=True, exist_ok=True)
        (self.root / "configuration.json").write_text(json.dumps(asdict(config), indent=2, sort_keys=True) + "\n", encoding="utf-8")
        for directory in ("prepared-input", "session/input", "session/output", "session/calibration",
                          "session/reports", "models", "evaluation"):
            (self.root / directory).mkdir(parents=True, exist_ok=True)
        session = {"schemaVersion": 1, "sessionId": str(uuid.uuid4()), "sampleRate": config.sample_rate,
                   "bitDepth": 24, "instrument": "other", "inputDevice": f"channel-{config.input_channel}",
                   "targetType": config.target_type, "targetName": config.target_name,
                   "targetSettings": config.controls or {}, "inputCalibrationDb": config.expected_input_rms_db,
                   "outputCalibrationDb": 0.0, "latencySamples": 0,
                   "notes": "Created by the TubeForge Phase 5 capture wizard"}
        (self.root / "session" / "session.json").write_text(json.dumps(session, indent=2) + "\n", encoding="utf-8")
        self._save(CaptureStage.configured, {"feedbackWarningAcknowledged": config.feedback_routing_acknowledged})
        return self.generate_prepared_material(config.sample_rate)

    def generate_prepared_material(self, sample_rate: int) -> dict[str, Any]:
        duration, directory = 2.0, self.root / "prepared-input"
        samples = int(sample_rate * duration); time = np.arange(samples, dtype=np.float64) / sample_rate
        sweep_phase = 2.0 * np.pi * 20.0 * duration / np.log(20_000.0 / 20.0) * (
            np.exp(time / duration * np.log(20_000.0 / 20.0)) - 1.0)
        random = np.random.default_rng(505)
        stepped = random.normal(0.0, 1.0, samples) * np.repeat(
            np.array([0.02, 0.05, 0.1, 0.18]), samples // 4 + 1)[:samples]
        impulse = np.zeros(samples); impulse[min(1024, samples - 1)] = 0.7
        material = {"log-sweep": 0.15 * np.sin(sweep_phase), "impulse": impulse,
                    "multitone": 0.04 * sum(np.sin(2 * np.pi * f * time) for f in (41, 83, 167, 337, 677, 1361, 2729, 5471)),
                    "level-stepped-noise": stepped}
        for name, audio in material.items(): write_pcm_wave(directory / f"{name}.wav", audio, sample_rate, 24)
        checklist = {"requiredMusicalTakes": ["guitar-chords", "guitar-palm-mutes",
            "bass-sustained-notes", "bass-transients"], "routingWarning":
            "Never route the target return back into its send. Begin with monitors muted and output low."}
        (directory / "musical-takes.json").write_text(json.dumps(checklist, indent=2) + "\n", encoding="utf-8")
        self._save(CaptureStage.material_ready, {"files": sorted(path.name for path in directory.glob("*.wav"))})
        return checklist

    def inspect_capture(self) -> dict[str, Any]:
        config = CaptureConfig(**json.loads((self.root / "configuration.json").read_text(encoding="utf-8")))
        if not config.feedback_routing_acknowledged:
            raise ValueError("Feedback-routing warning must be acknowledged before capture validation")
        package = SessionPackage(self.root / "session")
        report = validate_session(package)
        failed = [check.name for check in report.checks if not check.passed]
        safety = {"accepted": report.accepted, "failedChecks": failed,
                  "latencySamples": report.alignment.global_latency_samples if report.alignment else None,
                  "driftPpm": report.alignment.drift_ppm if report.alignment else None,
                  "correlationConfidence": report.alignment.confidence if report.alignment else 0.0}
        (self.root / "session" / "reports" / "capture-safety.json").write_text(
            json.dumps(safety, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        if not report.accepted: raise ValueError(f"Capture rejected: {failed}")
        self._save(CaptureStage.aligned, safety); return safety

    def train_capture(self, training_sessions: list[Path], validation_sessions: list[Path],
                      config_path: Path) -> tuple[Path, dict[str, object]]:
        config = ExperimentConfig.load(config_path)
        training_packages = [SessionPackage(path) for path in training_sessions]
        validation_packages = [SessionPackage(path) for path in validation_sessions]
        if not training_packages or not validation_packages: raise ValueError("Unseen validation sessions are required")
        for package in training_packages + validation_packages:
            if not validate_session(package, write_report=False).accepted:
                raise ValueError(f"Session failed capture validation: {package.root}")
        training_data = StreamingPairedDataset(training_packages, config.data.chunk_samples, config.data.history_samples)
        validation_data = StreamingPairedDataset(validation_packages, config.data.chunk_samples, config.data.history_samples)
        model = create_model(config.model.type, config.model.hidden_size, config.data.sample_rate,
                             config.training.seed, config.model.layers, config.model.kernel_size)
        run = self.root / "models" / str(uuid.uuid4())
        result = train(model, training_data.iter_shuffled(config.training.seed),
                       validation_data.iter_shuffled(config.training.seed), config, run)
        best = load_model_checkpoint(result.best_checkpoint)
        artifact = run / "artifact"
        export_model(best, artifact, expected_input_rms_db=training_packages[0].metadata.input_calibration_db)
        clips = []
        for package in validation_packages:
            for take in package.takes:
                source, target = WaveReader(take.input_path).read()[:, 0], WaveReader(take.output_path).read()[:, 0]
                measured = validate_session(package, write_report=False).alignment
                aligned_source, aligned_target = align_pair(source, target, measured.global_latency_samples, measured.polarity)
                clips.append((take.take_id, aligned_source, aligned_target))
        quality = generate_quality_report(best, clips, run / "quality-report.json",
                                          training_packages[0].metadata.input_calibration_db)
        self._save(CaptureStage.evaluated, {"artifact": str(artifact), "quality": quality})
        return artifact, quality

    def activate(self, artifact: Path) -> Path:
        manifest = validate_artifact(Path(artifact))
        pointer = self.root / "active-model.json"; temporary = pointer.with_suffix(".tmp")
        temporary.write_text(json.dumps({"artifact": str(Path(artifact).resolve()),
            "sha256": manifest.sha256, "architecture": manifest.architecture}, indent=2) + "\n", encoding="utf-8")
        os.replace(temporary, pointer); self._save(CaptureStage.activated, {"artifact": str(artifact)})
        return pointer
