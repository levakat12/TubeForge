from __future__ import annotations

import hashlib
import json
from collections.abc import Iterator
from dataclasses import dataclass
from pathlib import Path

import numpy as np

from ..alignment import align_pair, measure_alignment
from ..schemas.session import AlignmentResult, QualityCheck, SessionMetadata, ValidationReport
from .audio import WaveReader


@dataclass(frozen=True, slots=True)
class PairedTake:
    take_id: str
    input_path: Path
    output_path: Path


@dataclass(slots=True)
class ValidationLimits:
    minimum_duration_seconds: float = 1.0
    clipping_threshold: float = 0.999
    nonzero_rms_db: float = -90.0
    maximum_noise_floor_db: float = -55.0
    maximum_latency_std_samples: float = 2.0
    maximum_drift_ppm: float = 20.0
    minimum_alignment_confidence: float = 0.70
    active_threshold_db: float = -55.0
    missing_output_threshold_db: float = -75.0


class SessionPackage:
    def __init__(self, root: Path):
        self.root = Path(root)
        self.metadata = SessionMetadata.load(self.root / "session.json")
        self.takes = list(self._discover_takes())
        if not self.takes:
            raise ValueError(f"No paired takes found in {self.root}")

    def _discover_takes(self) -> Iterator[PairedTake]:
        input_directory, output_directory = self.root / "input", self.root / "output"
        inputs = {path.stem: path for path in input_directory.glob("*.wav")}
        outputs = {path.stem: path for path in output_directory.glob("*.wav")}
        missing_outputs = sorted(set(inputs) - set(outputs))
        missing_inputs = sorted(set(outputs) - set(inputs))
        if missing_outputs or missing_inputs:
            raise ValueError(f"Unpaired takes; missing outputs={missing_outputs}, missing inputs={missing_inputs}")
        for take_id in sorted(inputs):
            yield PairedTake(take_id, inputs[take_id], outputs[take_id])

    def dataset_version(self) -> str:
        digest = hashlib.sha256()
        digest.update(json.dumps(self.metadata.to_dict(), sort_keys=True).encode())
        for take in self.takes:
            for path in (take.input_path, take.output_path):
                digest.update(path.name.encode())
                with path.open("rb") as source:
                    while block := source.read(1024 * 1024):
                        digest.update(block)
        return digest.hexdigest()


def _db_rms(audio: np.ndarray) -> float:
    return float(20.0 * np.log10(max(float(np.sqrt(np.mean(np.square(audio, dtype=np.float64)))), 1.0e-12)))


def _missing_output_ratio(source: np.ndarray, processed: np.ndarray, sample_rate: int,
                          limits: ValidationLimits) -> float:
    window = max(128, sample_rate // 20)
    missing, active = 0, 0
    for start in range(0, min(len(source), len(processed)) - window + 1, window):
        input_db = _db_rms(source[start:start + window])
        if input_db < limits.active_threshold_db:
            continue
        active += 1
        if _db_rms(processed[start:start + window]) < limits.missing_output_threshold_db:
            missing += 1
    return missing / max(active, 1)


def validate_session(package: SessionPackage, limits: ValidationLimits | None = None,
                     write_report: bool = True) -> ValidationReport:
    limits = limits or ValidationLimits()
    checks: list[QualityCheck] = []
    take_reports: list[dict[str, object]] = []
    alignments: list[AlignmentResult] = []

    for take in package.takes:
        source_reader, output_reader = WaveReader(take.input_path), WaveReader(take.output_path)
        source_info, output_info = source_reader.info, output_reader.info
        same_rate = source_info.sample_rate == output_info.sample_rate == package.metadata.sample_rate
        same_channels = source_info.channels == output_info.channels
        expected_depth = source_info.bit_depth == output_info.bit_depth == package.metadata.bit_depth
        checks.extend((
            QualityCheck(f"{take.take_id}.sample_rate", same_rate, output_info.sample_rate,
                         package.metadata.sample_rate, "Input/output/session rates must match"),
            QualityCheck(f"{take.take_id}.channel_count", same_channels, output_info.channels,
                         source_info.channels, "Input and output channel counts must match"),
            QualityCheck(f"{take.take_id}.bit_depth", expected_depth, output_info.bit_depth,
                         package.metadata.bit_depth, "PCM depth must match session metadata"),
        ))
        source, processed = source_reader.read(), output_reader.read()
        minimum_duration = min(source_info.duration_seconds, output_info.duration_seconds)
        source_peak, output_peak = float(np.max(np.abs(source))), float(np.max(np.abs(processed)))
        source_rms, output_rms = _db_rms(source), _db_rms(processed)
        checks.extend((
            QualityCheck(f"{take.take_id}.duration", minimum_duration >= limits.minimum_duration_seconds,
                         minimum_duration, limits.minimum_duration_seconds),
            QualityCheck(f"{take.take_id}.input_clipping", source_peak < limits.clipping_threshold,
                         source_peak, limits.clipping_threshold),
            QualityCheck(f"{take.take_id}.output_clipping", output_peak < limits.clipping_threshold,
                         output_peak, limits.clipping_threshold),
            QualityCheck(f"{take.take_id}.input_nonzero", source_rms > limits.nonzero_rms_db,
                         source_rms, limits.nonzero_rms_db),
            QualityCheck(f"{take.take_id}.output_nonzero", output_rms > limits.nonzero_rms_db,
                         output_rms, limits.nonzero_rms_db),
        ))
        alignment = measure_alignment(source, processed, source_info.sample_rate,
                                      max_lag=max(source_info.sample_rate // 2, package.metadata.latency_samples * 2 + 64))
        alignments.append(alignment)
        aligned_source, aligned_output = align_pair(source, processed,
                                                    alignment.global_latency_samples, alignment.polarity)
        missing_ratio = _missing_output_ratio(aligned_source, aligned_output, source_info.sample_rate, limits)
        latency_std = float(np.std(alignment.local_latencies)) if alignment.local_latencies else 0.0
        polarity_ok = alignment.polarity == 1 or package.metadata.documented_polarity_inversion
        checks.extend((
            QualityCheck(f"{take.take_id}.alignment_confidence",
                         alignment.confidence >= limits.minimum_alignment_confidence,
                         alignment.confidence, limits.minimum_alignment_confidence),
            QualityCheck(f"{take.take_id}.metadata_latency",
                         abs(alignment.global_latency_samples - package.metadata.latency_samples)
                             <= limits.maximum_latency_std_samples,
                         alignment.global_latency_samples, package.metadata.latency_samples,
                         "Measured latency must match session metadata"),
            QualityCheck(f"{take.take_id}.latency_stability", latency_std <= limits.maximum_latency_std_samples,
                         latency_std, limits.maximum_latency_std_samples),
            QualityCheck(f"{take.take_id}.clock_drift", abs(alignment.drift_ppm) <= limits.maximum_drift_ppm,
                         alignment.drift_ppm, limits.maximum_drift_ppm),
            QualityCheck(f"{take.take_id}.missing_output", missing_ratio <= 0.01,
                         missing_ratio, 0.01),
            QualityCheck(f"{take.take_id}.polarity", polarity_ok, alignment.polarity,
                         "positive or documented"),
        ))
        take_reports.append({"takeId": take.take_id, "inputPeak": source_peak, "outputPeak": output_peak,
                             "inputRmsDb": source_rms, "outputRmsDb": output_rms,
                             "alignment": alignment.to_dict(), "missingOutputRatio": missing_ratio})

    for calibration_name in ("sweep.wav", "impulse.wav"):
        present = (package.root / "calibration" / calibration_name).is_file()
        checks.append(QualityCheck(f"calibration.{calibration_name}", present, present, True,
                                   "Required calibration signal is missing" if not present else ""))
    silence_path = package.root / "calibration" / "silence.wav"
    if silence_path.exists():
        noise_db = _db_rms(WaveReader(silence_path).read())
        checks.append(QualityCheck("noise_floor", noise_db <= limits.maximum_noise_floor_db,
                                   noise_db, limits.maximum_noise_floor_db))
    else:
        checks.append(QualityCheck("noise_floor", False, "missing", limits.maximum_noise_floor_db,
                                   "calibration/silence.wav is required"))

    latency_values = [alignment.global_latency_samples for alignment in alignments]
    session_latency_std = float(np.std(latency_values)) if latency_values else float("inf")
    checks.append(QualityCheck("session_latency_stability",
                               session_latency_std <= limits.maximum_latency_std_samples,
                               session_latency_std, limits.maximum_latency_std_samples))
    aggregate = AlignmentResult(
        int(round(float(np.median(latency_values)))) if latency_values else 0,
        float(np.median([alignment.drift_ppm for alignment in alignments])) if alignments else 0.0,
        min((alignment.confidence for alignment in alignments), default=0.0),
        alignments[0].polarity if alignments else 1,
        "aggregate",
        latency_values,
    )
    report = ValidationReport(1, package.metadata.session_id, all(check.passed for check in checks),
                              checks, aggregate, take_reports)
    if write_report:
        report.save(package.root / "reports" / "validation.json")
    return report
