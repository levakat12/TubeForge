from __future__ import annotations

import hashlib
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

from ..export import export_wavenet
from ..schemas.model import ModelManifest
from .reader import UnsupportedCapture, load_capture


@dataclass(frozen=True, slots=True)
class ConversionResult:
    source: Path
    artifact: Path
    tier: str
    manifest: ModelManifest


def artifact_name(path: Path, tier: str) -> str:
    """A stable directory name for a converted capture.

    Keyed by the source file's digest so that re-converting is idempotent and two captures with the
    same display name cannot collide.
    """
    digest = hashlib.sha256(path.read_bytes()).hexdigest()[:12]
    return f"{path.stem}-{tier}-{digest}"


def convert(path: Path, destination: Path, tier: str = "standard",
            expected_input_rms_db: float | None = None) -> ConversionResult:
    """Convert one `.nam` into a packed artifact directory."""
    capture = load_capture(path)
    artifact = destination / artifact_name(path, tier)
    manifest = export_wavenet(capture, artifact, tier, expected_input_rms_db=expected_input_rms_db)
    return ConversionResult(path, artifact, tier, manifest)


def convert_all(roots: Sequence[Path], destination: Path,
                tier: str = "standard") -> tuple[list[ConversionResult], list[tuple[Path, str]]]:
    """Convert every capture under `roots`, returning successes and failures separately.

    Failures are returned rather than raised: converting a folder of 354 captures should report the
    one that could not be read, not abandon the other 353.
    """
    converted: list[ConversionResult] = []
    failed: list[tuple[Path, str]] = []
    for root in roots:
        paths = sorted(root.rglob("*.nam")) if root.is_dir() else [root]
        for path in paths:
            try:
                converted.append(convert(path, destination, tier))
            except (UnsupportedCapture, ValueError, OSError) as error:
                failed.append((path, str(error)))
    return converted, failed
