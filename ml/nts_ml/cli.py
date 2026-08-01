from __future__ import annotations

import argparse
import json
import uuid
from pathlib import Path

from .datasets import SessionPackage, StreamingPairedDataset, validate_session
from .evaluation import evaluate, generate_evaluation_inputs, load_evaluation_pairs
from .export import export_model
from .models import create_model, load_model_checkpoint
from .nam import distill
from .nam.catalogue import build_catalogue
from .nam.convert import convert_all
from .training import ExperimentConfig, ExperimentTracker, train


def validate_dataset(arguments: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Validate a TubeForge paired-audio capture session")
    parser.add_argument("session", type=Path)
    options = parser.parse_args(arguments)
    report = validate_session(SessionPackage(options.session))
    print(json.dumps(report.to_dict(), indent=2, sort_keys=True))
    return 0 if report.accepted else 2


def tiny_train(arguments: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Run deterministic tiny recurrent-model training")
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--session", type=Path, action="append", required=True)
    parser.add_argument("--repository", type=Path, default=Path.cwd())
    options = parser.parse_args(arguments)
    config = ExperimentConfig.load(options.config)
    sessions = [SessionPackage(path) for path in options.session]
    for session in sessions:
        if not validate_session(session).accepted:
            raise ValueError(f"Session failed validation: {session.root}")
    if len(sessions) < 2:
        raise ValueError("At least two recording sessions are required to prevent validation leakage")
    training_sessions, validation_sessions = sessions[:-1], sessions[-1:]
    training_data = StreamingPairedDataset(training_sessions, config.data.chunk_samples,
                                           config.data.history_samples)
    validation_data = StreamingPairedDataset(validation_sessions, config.data.chunk_samples,
                                             config.data.history_samples)
    model = create_model(config.model.type, config.model.hidden_size, config.data.sample_rate,
                         config.training.seed, config.model.layers, config.model.kernel_size)
    run_id = str(uuid.uuid4())
    dataset_version = ":".join(session.dataset_version() for session in sessions)
    with ExperimentTracker(options.output / "experiments.sqlite3") as tracker:
        record = tracker.start(run_id, options.repository, config.to_dict(), config.training.seed,
                               dataset_version)
        result = train(model, training_data.iter_shuffled(config.training.seed),
                       validation_data.iter_shuffled(config.training.seed), config, options.output / run_id)
        artifact = options.output / run_id / "model"
        manifest = export_model(load_model_checkpoint(result.best_checkpoint), artifact,
                                expected_input_rms_db=sessions[0].metadata.input_calibration_db)
        tracker.finish(record, result.duration_seconds, result.best_checkpoint,
                       {"validationLoss": result.best_validation_loss}, manifest.sha256)
    print(options.output / run_id)
    return 0


def nam_distill(arguments: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Distil a Neural Amp Modeler capture into a packed TubeForge artifact")
    parser.add_argument("--capture", type=Path, required=True, help="Path to a .nam file")
    parser.add_argument("--di", type=Path, action="append", required=True,
                        help="DI wav file or directory; repeatable. At least two distinct files.")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--config", type=Path, default=Path("configs/nam-distill-lstm.toml"))
    parser.add_argument("--tier", choices=("standard", "lite"), default="standard")
    parser.add_argument("--sessions", type=int, default=2)
    parser.add_argument("--repository", type=Path, default=Path.cwd())
    options = parser.parse_args(arguments)
    artifact = distill(options.capture, options.di, options.output, options.config,
                       options.tier, options.sessions, options.repository)
    print(artifact)
    return 0


def nam_import(arguments: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Convert Neural Amp Modeler captures into packed TubeForge artifacts")
    parser.add_argument("captures", type=Path, nargs="+", help=".nam files or directories")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--tier", choices=("standard", "lite"), default="standard")
    options = parser.parse_args(arguments)
    converted, failed = convert_all(options.captures, options.output, options.tier)
    for path, reason in failed:
        print(f"skipped {path.name}: {reason}")
    for result in converted:
        print(result.artifact)
    print(f"{len(converted)} converted, {len(failed)} skipped")
    return 0 if converted or not failed else 2


def nam_catalogue(arguments: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Index a folder of Neural Amp Modeler captures as a tone reference corpus")
    parser.add_argument("roots", type=Path, nargs="+")
    parser.add_argument("--output", type=Path, required=True)
    options = parser.parse_args(arguments)
    catalogue = build_catalogue(options.roots)
    options.output.parent.mkdir(parents=True, exist_ok=True)
    options.output.write_text(json.dumps(catalogue, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"{len(catalogue['captures'])} captures indexed from {len(options.roots)} root(s)")
    return 0


def evaluate_model(arguments: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Evaluate a checkpoint on fixed clips")
    parser.add_argument("--checkpoint", type=Path, required=True)
    parser.add_argument("--dataset", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    options = parser.parse_args(arguments)
    model = load_model_checkpoint(options.checkpoint)
    report = evaluate(model, load_evaluation_pairs(options.dataset), options.report)
    print(json.dumps(report["aggregate"], indent=2, sort_keys=True))
    return 0


def generate_eval(arguments: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Generate deterministic fixed evaluation inputs")
    parser.add_argument("destination", type=Path)
    parser.add_argument("--sample-rate", type=int, default=48_000)
    options = parser.parse_args(arguments)
    generate_evaluation_inputs(options.destination, options.sample_rate)
    return 0
