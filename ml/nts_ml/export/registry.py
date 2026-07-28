from __future__ import annotations

from pathlib import Path
import shutil

from ..schemas.model import ModelManifest
from .packed import validate_artifact


class ModelRegistry:
    def __init__(self, root: Path):
        self.root = Path(root)
        self.root.mkdir(parents=True, exist_ok=True)

    def register(self, model_name: str, version: str, artifact: Path) -> Path:
        if not model_name or not version or any(part in (".", "..") for part in Path(model_name).parts):
            raise ValueError("Invalid model name or version")
        validate_artifact(artifact)
        destination = self.root / model_name / version
        if destination.exists():
            raise FileExistsError(f"Model version already exists: {destination}")
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copytree(artifact, destination)
        validate_artifact(destination)
        return destination

    def resolve(self, model_name: str, version: str) -> tuple[Path, ModelManifest]:
        path = self.root / model_name / version
        return path, validate_artifact(path)

    def versions(self, model_name: str) -> list[str]:
        directory = self.root / model_name
        return sorted(path.name for path in directory.iterdir() if path.is_dir()) if directory.exists() else []
