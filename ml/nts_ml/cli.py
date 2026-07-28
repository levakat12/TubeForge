from __future__ import annotations

from pathlib import Path
import argparse
import json
import uuid

from .datasets import SessionPackage, StreamingPairedDataset, validate_session
from .evaluation import evaluate, generate_evaluation_inputs, load_evaluation_pairs
from .export import export_model
from .models import create_model, load_model_checkpoint
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
