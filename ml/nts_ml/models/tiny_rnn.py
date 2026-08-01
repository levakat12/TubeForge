from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Protocol

import numpy as np
from numpy.typing import NDArray


class AudioModel(Protocol):
    sample_rate: int
    state_size: int

    def reset(self) -> None: ...
    def process(self, audio: NDArray[np.floating]) -> NDArray[np.float32]: ...


@dataclass(slots=True)
class RnnCache:
    inputs: NDArray[np.float32]
    states: NDArray[np.float32]
    outputs: NDArray[np.float32]


class TinyTanhRnn:
    architecture = "tiny_tanh_rnn"

    def __init__(self, state_size: int = 8, sample_rate: int = 48_000, seed: int = 0):
        if state_size < 1:
            raise ValueError("state_size must be positive")
        self.state_size = state_size
        self.sample_rate = sample_rate
        random = np.random.default_rng(seed)
        self.input_weight = random.normal(0.0, 0.12, (state_size, 1)).astype(np.float32)
        recurrent = random.normal(0.0, 0.06, (state_size, state_size)).astype(np.float32)
        recurrent += np.eye(state_size, dtype=np.float32) * 0.35
        self.recurrent_weight = recurrent
        self.bias = np.zeros(state_size, dtype=np.float32)
        self.output_weight = random.normal(0.0, 0.12, (1, state_size)).astype(np.float32)
        self.output_bias = np.zeros(1, dtype=np.float32)
        self.state = np.zeros(state_size, dtype=np.float32)

    def reset(self) -> None:
        self.state.fill(0.0)

    def forward(self, audio: NDArray[np.floating], retain_cache: bool = False
                ) -> NDArray[np.float32] | tuple[NDArray[np.float32], RnnCache]:
        inputs = np.asarray(audio, dtype=np.float32).reshape(-1)
        states = np.empty((inputs.size + 1, self.state_size), dtype=np.float32)
        outputs = np.empty(inputs.size, dtype=np.float32)
        states[0] = self.state
        for index, sample in enumerate(inputs):
            states[index + 1] = np.tanh(self.input_weight[:, 0] * sample
                                       + self.recurrent_weight @ states[index] + self.bias)
            outputs[index] = float((self.output_weight @ states[index + 1])[0] + self.output_bias[0])
        self.state = states[-1].copy()
        if retain_cache:
            return outputs, RnnCache(inputs, states, outputs)
        return outputs

    def process(self, audio: NDArray[np.floating]) -> NDArray[np.float32]:
        result = self.forward(audio)
        assert isinstance(result, np.ndarray)
        return result

    def parameters(self) -> dict[str, NDArray[np.float32]]:
        return {
            "input_weight": self.input_weight,
            "recurrent_weight": self.recurrent_weight,
            "bias": self.bias,
            "output_weight": self.output_weight,
            "output_bias": self.output_bias,
        }

    def save_checkpoint(self, path: Path) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        np.savez(path, sample_rate=self.sample_rate, state_size=self.state_size, **self.parameters())

    @classmethod
    def load_checkpoint(cls, path: Path) -> TinyTanhRnn:
        data = np.load(path)
        model = cls(int(data["state_size"]), int(data["sample_rate"]), 0)
        for name, parameter in model.parameters().items():
            parameter[...] = data[name]
        model.reset()
        return model


def create_model(model_type: str, state_size: int, sample_rate: int, seed: int,
                 layers: int = 1, kernel_size: int = 3):
    if model_type in ("tiny_tanh_rnn", "conditioned_tanh_rnn"):
        return TinyTanhRnn(state_size, sample_rate, seed)
    from .neural import ConditionedLstm, RandomFeatureGru, RandomFeatureTcn
    if model_type == "conditioned_lstm": return ConditionedLstm(state_size, sample_rate, seed)
    if model_type == "conditioned_gru": return RandomFeatureGru(state_size, sample_rate, seed)
    if model_type == "causal_tcn": return RandomFeatureTcn(state_size, sample_rate, seed, layers, kernel_size)
    raise ValueError(f"Unsupported model type: {model_type}")
