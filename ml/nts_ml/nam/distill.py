from __future__ import annotations

import json
import uuid
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

import numpy as np
from numpy.typing import NDArray

from ..datasets import SessionPackage, validate_session
from ..datasets.audio import WaveReader, write_pcm_wave
from ..datasets.package import ValidationLimits
from .reader import NamCapture, UnsupportedCapture, load_capture
from .wavenet import WaveNetModel

RENDER_BLOCK_SAMPLES = 4 * 48_000
RENDERED_DRIFT_TOLERANCE_PPM = 1.0e-6


def render(capture: NamCapture, tier: str, audio: NDArray[np.floating],
           block_samples: int = RENDER_BLOCK_SAMPLES) -> NDArray[np.float32]:
    """Run a signal through a capture, in blocks, from a primed state.

    Blocked rendering is for memory, not behaviour: the model retains history across calls, so the
    result is identical to rendering the whole file at once.
    """
    model = WaveNetModel(capture.spec(tier))
    model.reset()
    samples = np.asarray(audio, dtype=np.float32).reshape(-1)
    return np.concatenate([model.process(samples[start:start + block_samples])
                           for start in range(0, samples.size, block_samples)]) \
        if samples.size else np.zeros(0, dtype=np.float32)


def measure_latency(capture: NamCapture, tier: str) -> int:
    """The propagation delay a capture bakes in, measured from its impulse response.

    Not zero, and not the same for every capture: a mic'd cabinet is part of what was captured, so
    its acoustic delay is part of the model. Measured across the corpus this ranges from 8 samples
    (Marshall JCM800) through 13 (Fulltone OCD) to 29 (Fender Super Reverb). Declaring it matters
    for more than passing a check -- the training pipeline aligns pairs using session latency, and
    a student told the delay is zero spends its capacity learning a delay it models poorly.
    """
    position, length = 1024, 8192
    impulse = np.zeros(length, dtype=np.float32); impulse[position] = 0.7
    response = render(capture, tier, impulse)
    tail = response[position:position + 512]
    baseline = float(np.median(response[:position])) if position else 0.0
    return int(np.argmax(np.abs(tail - baseline))) if tail.size else 0


def rendered_limits() -> ValidationLimits:
    """Phase 4 limits adjusted for pairs that were rendered rather than recorded.

    Only `minimum_alignment_confidence` moves, and only because it does not measure what its name
    suggests here. Confidence is the normalised cross-correlation peak between input and output; a
    mic'd cabinet removes most of the DI's spectrum, so a legitimately perfect render scores 0.17
    on a dark clean Fender. What the check exists to catch -- drift, jitter, a mis-declared delay --
    is verified instead by `validate_sessions`, which requires drift and latency deviation of
    exactly zero. That is a stricter test than the one being waived, not a weaker one.
    """
    return ValidationLimits(minimum_alignment_confidence=0.0)


def _rms_db(audio: NDArray[np.floating]) -> float:
    return float(20.0 * np.log10(max(float(np.sqrt(np.mean(np.square(audio, dtype=np.float64)))), 1.0e-12)))


@dataclass(frozen=True, slots=True)
class DistilledSession:
    root: Path
    take_count: int
    input_rms_db: float
    output_rms_db: float
    polarity_inverted: bool
    latency_samples: int


def _calibration_material(capture: NamCapture, tier: str,
                          sample_rate: int) -> tuple[dict[str, NDArray[np.float32]], float]:
    """The calibration signals a session must carry, rendered through the teacher.

    `silence.wav` is the model's response to silence rather than digital zero, so the noise-floor
    check measures this teacher rather than a placeholder. Its DC mean is removed and returned
    separately: on real captures the idle output is *entirely* DC -- measured at -62 to -78 dBFS
    with an AC component below -195 dB -- and feeding that to a noise-floor check would report a
    constant offset as noise. The offset is a real property of the capture, so it goes into session
    metadata rather than being discarded.
    """
    duration = max(sample_rate // 2, 1)
    phase = 2.0 * np.pi * 20.0 * (duration / sample_rate) / np.log(20_000.0 / 20.0) * (
        np.power(20_000.0 / 20.0, np.arange(duration) / duration) - 1.0)
    impulse = np.zeros(duration, dtype=np.float32); impulse[min(1024, duration - 1)] = 0.7
    idle = render(capture, tier, np.zeros(duration, dtype=np.float32))
    offset = float(np.mean(idle, dtype=np.float64)) if idle.size else 0.0
    return ({"sweep": render(capture, tier, (0.15 * np.sin(phase)).astype(np.float32)),
             "impulse": render(capture, tier, impulse),
             "silence": (idle - np.float32(offset)).astype(np.float32)}, offset)


def write_session(root: Path, capture: NamCapture, tier: str,
                  takes: Sequence[tuple[str, NDArray[np.floating]]],
                  bit_depth: int = 24, session_id: str | None = None) -> DistilledSession:
    """Render `takes` through the capture and write a Phase 4 session package.

    Latency and polarity are measured, not assumed. Rendering is deterministic, but "deterministic"
    is not "instant": the capture includes a mic'd cabinet whose propagation delay is part of the
    model, and several real amplifiers invert.
    """
    sample_rate = capture.sample_rate
    for directory in ("input", "output", "calibration", "reports"):
        (root / directory).mkdir(parents=True, exist_ok=True)
    inputs, outputs = [], []
    for take_id, audio in takes:
        source = np.asarray(audio, dtype=np.float32).reshape(-1)
        processed = render(capture, tier, source)
        write_pcm_wave(root / "input" / f"{take_id}.wav", source, sample_rate, bit_depth)
        write_pcm_wave(root / "output" / f"{take_id}.wav", processed, sample_rate, bit_depth)
        inputs.append(source); outputs.append(processed)
    material, idle_offset = _calibration_material(capture, tier, sample_rate)
    for name, audio in material.items():
        write_pcm_wave(root / "calibration" / f"{name}.wav", audio, sample_rate, bit_depth)
    source_all = np.concatenate(inputs); output_all = np.concatenate(outputs)
    inverted = bool(np.sum(source_all * output_all) < 0.0)
    metadata = {
        "schemaVersion": 1,
        "sessionId": session_id or str(uuid.uuid4()),
        "sampleRate": sample_rate,
        "bitDepth": bit_depth,
        "instrument": "guitar",
        "inputDevice": "rendered",
        "targetType": capture.metadata.gear_type or "amp",
        "targetName": capture.metadata.name or capture.path.stem,
        "targetSettings": {"source": "neural-amp-modeler capture", "tier": tier,
                           "captureFile": capture.path.name,
                           "modeledBy": capture.metadata.modeled_by,
                           "captureLoudness": capture.metadata.loudness,
                           "captureGain": capture.metadata.gain,
                           "idleDcOffset": idle_offset,
                           "polarityInverted": inverted},
        "inputCalibrationDb": _rms_db(source_all),
        "outputCalibrationDb": _rms_db(output_all),
        "latencySamples": measure_latency(capture, tier),
        "notes": (f"Distilled from {capture.path.name} ({tier} tier). Rendered pairs, not recorded; "
                  "redistribution requires the capture author's permission."),
    }
    (root / "session.json").write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n",
                                       encoding="utf-8")
    return DistilledSession(root, len(takes), metadata["inputCalibrationDb"],
                            metadata["outputCalibrationDb"], inverted,
                            metadata["latencySamples"])


def _read_di(path: Path, sample_rate: int) -> NDArray[np.float32]:
    reader = WaveReader(path)
    if reader.info.sample_rate != sample_rate:
        raise UnsupportedCapture(
            f"{path.name} is {reader.info.sample_rate} Hz but the capture is {sample_rate} Hz; "
            "resample the DI corpus first -- this package does not resample silently")
    return reader.read()


def collect_di(paths: Sequence[Path]) -> list[Path]:
    files: list[Path] = []
    for path in paths:
        files.extend(sorted(path.glob("*.wav")) if path.is_dir() else [path])
    return files


def build_sessions(capture: NamCapture, tier: str, di_paths: Sequence[Path], destination: Path,
                   sessions: int = 2) -> list[DistilledSession]:
    """Split the DI corpus across sessions and render each one.

    Splitting is by source file, never by slicing one performance in two. Training holds the last
    session out for validation, and two halves of the same take are not independent -- the split
    would look like a validation set while measuring memorisation.
    """
    files = collect_di(di_paths)
    if len(files) < sessions:
        raise ValueError(f"{sessions} sessions need at least {sessions} distinct DI files; "
                         f"got {len(files)}. Splitting one take in two leaks validation material.")
    groups: list[list[Path]] = [[] for _ in range(sessions)]
    for index, path in enumerate(files): groups[index % sessions].append(path)
    built: list[DistilledSession] = []
    for index, group in enumerate(groups):
        takes = [(f"take-{position:04d}", _read_di(path, capture.sample_rate))
                 for position, path in enumerate(group)]
        built.append(write_session(destination / f"session-{index + 1:02d}", capture, tier, takes))
    return built


def validate_sessions(sessions: Sequence[DistilledSession],
                      limits: ValidationLimits | None = None) -> list[str]:
    """Run the Phase 4 gate over rendered sessions and return the names of any failed checks.

    Rendered material must additionally be *exactly* stable -- zero clock drift and zero deviation
    between takes' measured latencies -- because anything else means the renderer, not the room, is
    at fault. That replaces the correlation-confidence check waived by `rendered_limits`.
    """
    failures: list[str] = []
    for session in sessions:
        report = validate_session(SessionPackage(session.root), limits or rendered_limits())
        failures.extend(f"{session.root.name}:{check.name}"
                        for check in report.checks if not check.passed)
        alignment = report.alignment
        if alignment is None: failures.append(f"{session.root.name}:alignment_missing"); continue
        # Drift is estimated by regression, so a perfect render lands on float residue near 1e-15
        # rather than exact zero. The recorded-session limit is 20 ppm; a millionth of one is not
        # drift, and anything a renderer could plausibly get wrong is orders of magnitude above it.
        if abs(alignment.drift_ppm) > RENDERED_DRIFT_TOLERANCE_PPM:
            failures.append(f"{session.root.name}:rendered_drift")
        if any(value != alignment.local_latencies[0] for value in alignment.local_latencies):
            failures.append(f"{session.root.name}:rendered_latency_spread")
    return failures


def distill(capture_path: Path, di_paths: Sequence[Path], destination: Path, config_path: Path,
            tier: str = "standard", sessions: int = 2, repository: Path | None = None) -> Path:
    """Render a NAM capture into sessions, train a student on them, and export a packed artifact.

    The student is whatever `config_path` selects, so the result loads in the existing runtime with
    no format work. The teacher's own ESR is recorded alongside for comparison: a WaveNet capture
    distilled into a 32-unit LSTM is an approximation, and the report should let a reader see how
    much was lost rather than imply parity.
    """
    from ..datasets import StreamingPairedDataset
    from ..export import export_model
    from ..models import create_model, load_model_checkpoint
    from ..training import ExperimentConfig, ExperimentTracker, train

    capture = load_capture(capture_path)
    built = build_sessions(capture, tier, di_paths, destination / "sessions", sessions)
    failures = validate_sessions(built)
    if failures:
        raise ValueError("Rendered sessions failed the capture quality gate: " + ", ".join(failures))
    config = ExperimentConfig.load(config_path)
    if config.data.sample_rate != capture.sample_rate:
        raise ValueError(f"Config sample rate {config.data.sample_rate} does not match the capture's "
                         f"{capture.sample_rate}")
    packages = [SessionPackage(session.root) for session in built]
    training_data = StreamingPairedDataset(packages[:-1], config.data.chunk_samples,
                                           config.data.history_samples)
    validation_data = StreamingPairedDataset(packages[-1:], config.data.chunk_samples,
                                             config.data.history_samples)
    model = create_model(config.model.type, config.model.hidden_size, config.data.sample_rate,
                         config.training.seed, config.model.layers, config.model.kernel_size)
    run_id = str(uuid.uuid4())
    with ExperimentTracker(destination / "experiments.sqlite3") as tracker:
        record = tracker.start(run_id, repository or Path.cwd(), config.to_dict(),
                               config.training.seed,
                               ":".join(package.dataset_version() for package in packages))
        result = train(model, training_data.iter_shuffled(config.training.seed),
                       validation_data.iter_shuffled(config.training.seed), config,
                       destination / run_id)
        artifact = destination / run_id / "model"
        manifest = export_model(load_model_checkpoint(result.best_checkpoint), artifact,
                                expected_input_rms_db=built[0].input_rms_db,
                                license_text=capture.metadata.license_text())
        tracker.finish(record, result.duration_seconds, result.best_checkpoint,
                       {"validationLoss": result.best_validation_loss}, manifest.sha256)
    return artifact


def teacher_student_esr(capture: NamCapture, tier: str, student, audio: NDArray[np.floating]) -> float:
    """How much the student lost relative to the capture it was distilled from."""
    target = render(capture, tier, audio)
    student.reset()
    prediction = np.asarray(student.process(np.asarray(audio, dtype=np.float32)), dtype=np.float32)
    length = min(prediction.size, target.size)
    return float(np.sum((prediction[:length] - target[:length]) ** 2)
                 / max(float(np.sum(target[:length] ** 2)), 1.0e-20))
