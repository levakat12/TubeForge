from __future__ import annotations

from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any
import json
import tomllib


@dataclass(slots=True)
class ModelConfig:
    type: str = "tiny_tanh_rnn"
    hidden_size: int = 8
    layers: int = 1
    kernel_size: int = 3
    conditioning: str = "concatenate"
    residual: bool = True


@dataclass(slots=True)
class DataConfig:
    sample_rate: int = 48_000
    chunk_samples: int = 4096
    history_samples: int = 2048


@dataclass(slots=True)
class TrainingOptions:
    batch_size: int = 4
    optimizer: str = "adamw"
    learning_rate: float = 0.0003
    weight_decay: float = 0.0001
    epochs: int = 10
    seed: int = 1234


@dataclass(slots=True)
class LossConfig:
    time_l1: float = 1.0
    time_l2: float = 0.0
    pre_emphasis: float = 0.0
    multi_resolution_stft: float = 0.5
    spectral_convergence: float = 0.0
    dc_penalty: float = 0.05
    loudness_difference: float = 0.0
    silence_stability: float = 0.05
    transient_weighted: float = 0.0


@dataclass(slots=True)
class ExperimentConfig:
    model: ModelConfig = field(default_factory=ModelConfig)
    data: DataConfig = field(default_factory=DataConfig)
    training: TrainingOptions = field(default_factory=TrainingOptions)
    loss: LossConfig = field(default_factory=LossConfig)

    @classmethod
    def load(cls, path: Path) -> "ExperimentConfig":
        data = tomllib.loads(path.read_text(encoding="utf-8"))
        config = cls(ModelConfig(**data.get("model", {})), DataConfig(**data.get("data", {})),
                     TrainingOptions(**data.get("training", {})), LossConfig(**data.get("loss", {})))
        config.validate()
        return config

    def validate(self) -> None:
        if self.model.type not in ("tiny_tanh_rnn", "conditioned_tanh_rnn", "conditioned_lstm",
                                   "conditioned_gru", "causal_tcn"):
            raise ValueError("Unsupported model type")
        if self.model.hidden_size < 1 or self.model.layers < 1:
            raise ValueError("Model hidden size and layer count must be positive")
        if self.model.type != "causal_tcn" and self.model.layers != 1:
            raise ValueError("The first recurrent runtime supports one layer")
        if self.model.kernel_size < 2 or self.model.conditioning != "concatenate":
            raise ValueError("Invalid TCN kernel or conditioning mode")
        if self.data.chunk_samples < 1 or self.data.history_samples < 0:
            raise ValueError("Invalid chunk or history size")
        if self.training.optimizer != "adamw" or self.training.epochs < 1:
            raise ValueError("optimizer must be adamw and epochs must be positive")
        if self.training.learning_rate <= 0.0 or self.training.batch_size < 1:
            raise ValueError("Invalid learning rate or batch size")

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)

    def save_resolved(self, path: Path) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(self.to_dict(), indent=2, sort_keys=True) + "\n", encoding="utf-8")
