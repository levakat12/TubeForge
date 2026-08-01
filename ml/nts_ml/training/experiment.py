from __future__ import annotations

import json
import platform
import sqlite3
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any


def git_commit(repository: Path) -> str:
    try:
        return subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repository,
                                       text=True, stderr=subprocess.DEVNULL).strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


@dataclass(slots=True)
class ExperimentRecord:
    run_id: str
    git_commit: str
    config_json: str
    random_seed: int
    dataset_version: str
    hardware: str
    training_seconds: float = 0.0
    best_checkpoint: str = ""
    validation_metrics_json: str = "{}"
    exported_model_hash: str = ""


class ExperimentTracker:
    def __init__(self, database: Path):
        database.parent.mkdir(parents=True, exist_ok=True)
        self.connection = sqlite3.connect(database)
        self.connection.execute(
            """CREATE TABLE IF NOT EXISTS experiments (
                run_id TEXT PRIMARY KEY, git_commit TEXT NOT NULL, config_json TEXT NOT NULL,
                random_seed INTEGER NOT NULL, dataset_version TEXT NOT NULL, hardware TEXT NOT NULL,
                training_seconds REAL NOT NULL, best_checkpoint TEXT NOT NULL,
                validation_metrics_json TEXT NOT NULL, exported_model_hash TEXT NOT NULL,
                created_unix REAL NOT NULL
            )""")
        self.connection.commit()

    def start(self, run_id: str, repository: Path, config: dict[str, Any], seed: int,
              dataset_version: str) -> ExperimentRecord:
        record = ExperimentRecord(run_id, git_commit(repository), json.dumps(config, sort_keys=True),
                                  seed, dataset_version,
                                  f"{platform.system()} {platform.machine()} | {platform.processor()}")
        self._write(record)
        return record

    def finish(self, record: ExperimentRecord, duration: float, checkpoint: Path,
               metrics: dict[str, float], exported_hash: str = "") -> None:
        record.training_seconds = duration
        record.best_checkpoint = str(checkpoint)
        record.validation_metrics_json = json.dumps(metrics, sort_keys=True)
        record.exported_model_hash = exported_hash
        self._write(record)

    def _write(self, record: ExperimentRecord) -> None:
        self.connection.execute(
            """INSERT OR REPLACE INTO experiments VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)""",
            (record.run_id, record.git_commit, record.config_json, record.random_seed,
             record.dataset_version, record.hardware, record.training_seconds,
             record.best_checkpoint, record.validation_metrics_json,
             record.exported_model_hash, time.time()))
        self.connection.commit()

    def close(self) -> None:
        self.connection.close()

    def __enter__(self) -> ExperimentTracker:
        return self

    def __exit__(self, *_: object) -> None:
        self.close()
