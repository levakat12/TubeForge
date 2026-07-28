from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

import numpy as np
from numpy.typing import NDArray


CONTROL_NAMES = ("gain", "tone", "master", "channel", "instrument_mode")


def _sigmoid(values: NDArray[np.floating]) -> NDArray[np.float32]:
    clipped = np.clip(values, -30.0, 30.0)
    return (1.0 / (1.0 + np.exp(-clipped))).astype(np.float32)


class ConditionedModel:
    control_count = len(CONTROL_NAMES)

    def __init__(self) -> None:
        self.controls = np.zeros(self.control_count, dtype=np.float32)

    def set_controls(self, controls: NDArray[np.floating] | list[float]) -> None:
        values = np.asarray(controls, dtype=np.float32).reshape(-1)
        if values.size != self.control_count or not np.all(np.isfinite(values)):
            raise ValueError(f"controls must contain {self.control_count} finite values")
        self.controls[:] = np.clip(values, -1.0, 1.0)


@dataclass(slots=True)
class LstmCache:
    inputs: NDArray[np.float32]
    hidden: NDArray[np.float32]
    cells: NDArray[np.float32]
    gates: NDArray[np.float32]
    outputs: NDArray[np.float32]


class ConditionedLstm(ConditionedModel):
    architecture = "conditioned_lstm"

    def __init__(self, state_size: int = 32, sample_rate: int = 48_000, seed: int = 0,
                 residual: bool = True):
        super().__init__()
        if not 1 <= state_size <= 512:
            raise ValueError("state_size must be between 1 and 512")
        self.state_size, self.sample_rate, self.residual = state_size, sample_rate, residual
        random = np.random.default_rng(seed)
        scale = 1.0 / np.sqrt(state_size + self.control_count + 1)
        self.input_weight = random.normal(0.0, scale, (4 * state_size, 1 + self.control_count)).astype(np.float32)
        self.recurrent_weight = random.normal(0.0, scale * 0.6, (4 * state_size, state_size)).astype(np.float32)
        self.bias = np.zeros(4 * state_size, dtype=np.float32)
        self.bias[state_size:2 * state_size] = 1.0
        self.output_weight = random.normal(0.0, 0.08, state_size).astype(np.float32)
        self.output_bias = np.zeros(1, dtype=np.float32)
        self.residual_gain = np.array([0.25 if residual else 0.0], dtype=np.float32)
        self.hidden = np.zeros(state_size, dtype=np.float32)
        self.cell = np.zeros(state_size, dtype=np.float32)

    def reset(self) -> None:
        self.hidden.fill(0.0); self.cell.fill(0.0)

    def forward(self, audio: NDArray[np.floating], retain_cache: bool = False
                ) -> NDArray[np.float32] | tuple[NDArray[np.float32], LstmCache]:
        samples = np.asarray(audio, dtype=np.float32).reshape(-1)
        inputs = np.empty((samples.size, 1 + self.control_count), dtype=np.float32)
        inputs[:, 0] = samples; inputs[:, 1:] = self.controls
        hidden = np.empty((samples.size + 1, self.state_size), dtype=np.float32)
        cells = np.empty_like(hidden); gates = np.empty((samples.size, 4, self.state_size), dtype=np.float32)
        outputs = np.empty(samples.size, dtype=np.float32)
        hidden[0], cells[0] = self.hidden, self.cell
        for index, sample in enumerate(samples):
            affine = self.input_weight @ inputs[index] + self.recurrent_weight @ hidden[index] + self.bias
            i = _sigmoid(affine[:self.state_size]); f = _sigmoid(affine[self.state_size:2 * self.state_size])
            g = np.tanh(affine[2 * self.state_size:3 * self.state_size]).astype(np.float32)
            o = _sigmoid(affine[3 * self.state_size:])
            cells[index + 1] = f * cells[index] + i * g
            hidden[index + 1] = o * np.tanh(cells[index + 1])
            gates[index] = np.stack((i, f, g, o))
            outputs[index] = self.output_weight @ hidden[index + 1] + self.output_bias[0] + self.residual_gain[0] * sample
        self.hidden, self.cell = hidden[-1].copy(), cells[-1].copy()
        if retain_cache:
            return outputs, LstmCache(inputs, hidden, cells, gates, outputs)
        return outputs

    def process(self, audio: NDArray[np.floating]) -> NDArray[np.float32]:
        result = self.forward(audio)
        assert isinstance(result, np.ndarray)
        return result

    def parameters(self) -> dict[str, NDArray[np.float32]]:
        return {"input_weight": self.input_weight, "recurrent_weight": self.recurrent_weight,
                "bias": self.bias, "output_weight": self.output_weight,
                "output_bias": self.output_bias, "residual_gain": self.residual_gain}

    def save_checkpoint(self, path: Path) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        np.savez(path, architecture=self.architecture, sample_rate=self.sample_rate,
                 state_size=self.state_size, residual=int(self.residual), **self.parameters())

    @classmethod
    def load_checkpoint(cls, path: Path) -> "ConditionedLstm":
        data = np.load(path)
        model = cls(int(data["state_size"]), int(data["sample_rate"]), residual=bool(data["residual"]))
        for name, value in model.parameters().items(): value[...] = data[name]
        model.reset(); return model


@dataclass(slots=True)
class FeatureCache:
    inputs: NDArray[np.float32]
    features: NDArray[np.float32]
    outputs: NDArray[np.float32]


class ConditionedGru(ConditionedModel):
    architecture = "conditioned_gru"

    def __init__(self, state_size: int = 24, sample_rate: int = 48_000, seed: int = 0):
        super().__init__(); self.state_size, self.sample_rate = state_size, sample_rate
        random = np.random.default_rng(seed); inputs = 1 + self.control_count
        scale = 1.0 / np.sqrt(state_size + inputs)
        self.input_weight = random.normal(0.0, scale, (3 * state_size, inputs)).astype(np.float32)
        self.recurrent_weight = random.normal(0.0, scale * 0.6, (3 * state_size, state_size)).astype(np.float32)
        self.bias = np.zeros(3 * state_size, dtype=np.float32)
        self.output_weight = random.normal(0.0, 0.08, state_size).astype(np.float32)
        self.output_bias = np.zeros(1, dtype=np.float32); self.state = np.zeros(state_size, dtype=np.float32)

    def reset(self) -> None: self.state.fill(0.0)

    def forward(self, audio: NDArray[np.floating], retain_cache: bool = False):
        samples = np.asarray(audio, dtype=np.float32).reshape(-1)
        inputs = np.column_stack((samples, np.repeat(self.controls[None, :], samples.size, axis=0))).astype(np.float32)
        features = np.empty((samples.size, self.state_size), dtype=np.float32); outputs = np.empty(samples.size, dtype=np.float32)
        for index, sample_input in enumerate(inputs):
            x = self.input_weight @ sample_input + self.bias
            recurrent = self.recurrent_weight @ self.state
            reset = _sigmoid(x[:self.state_size] + recurrent[:self.state_size])
            update = _sigmoid(x[self.state_size:2 * self.state_size] + recurrent[self.state_size:2 * self.state_size])
            candidate = np.tanh(x[2 * self.state_size:] + reset * recurrent[2 * self.state_size:])
            self.state = ((1.0 - update) * candidate + update * self.state).astype(np.float32)
            features[index] = self.state; outputs[index] = self.output_weight @ self.state + self.output_bias[0]
        return (outputs, FeatureCache(inputs, features, outputs)) if retain_cache else outputs

    def process(self, audio): return self.forward(audio)
    def parameters(self): return {"output_weight": self.output_weight, "output_bias": self.output_bias}
    def save_checkpoint(self, path: Path) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        np.savez(path, architecture=self.architecture, sample_rate=self.sample_rate, state_size=self.state_size,
                 input_weight=self.input_weight, recurrent_weight=self.recurrent_weight, bias=self.bias,
                 output_weight=self.output_weight, output_bias=self.output_bias)

    @classmethod
    def load_checkpoint(cls, path: Path) -> "ConditionedGru":
        data = np.load(path); model = cls(int(data["state_size"]), int(data["sample_rate"]))
        for name in ("input_weight", "recurrent_weight", "bias", "output_weight", "output_bias"):
            getattr(model, name)[...] = data[name]
        model.reset(); return model


class CausalTcn(ConditionedModel):
    architecture = "causal_tcn"

    def __init__(self, state_size: int = 16, sample_rate: int = 48_000, seed: int = 0,
                 layers: int = 4, kernel_size: int = 3):
        super().__init__(); self.state_size, self.sample_rate = state_size, sample_rate
        self.layers, self.kernel_size = layers, kernel_size
        if layers < 1 or kernel_size < 2: raise ValueError("TCN requires layers >= 1 and kernel_size >= 2")
        random = np.random.default_rng(seed)
        self.input_projection = random.normal(0.0, 0.2, state_size).astype(np.float32)
        self.control_projection = random.normal(0.0, 0.1, (state_size, self.control_count)).astype(np.float32)
        self.kernel = random.normal(0.0, 0.08, (layers, state_size, kernel_size)).astype(np.float32)
        self.output_weight = random.normal(0.0, 0.08, state_size).astype(np.float32)
        self.output_bias = np.zeros(1, dtype=np.float32); self.residual_gain = np.array([0.2], dtype=np.float32)
        self.receptive_field = 1 + (kernel_size - 1) * sum(2 ** layer for layer in range(layers))
        self.histories = [np.zeros((state_size, 1 + (kernel_size - 1) * 2 ** layer), dtype=np.float32)
                          for layer in range(layers)]
        self.positions = [0] * layers

    def reset(self) -> None:
        for history in self.histories: history.fill(0.0)
        self.positions = [0] * self.layers

    def forward(self, audio: NDArray[np.floating], retain_cache: bool = False):
        samples = np.asarray(audio, dtype=np.float32).reshape(-1); features = np.empty((samples.size, self.state_size), dtype=np.float32)
        for sample_index, sample in enumerate(samples):
            value = self.input_projection * sample + self.control_projection @ self.controls
            for layer in range(self.layers):
                history, position, dilation = self.histories[layer], self.positions[layer], 2 ** layer
                history[:, position] = value
                convolved = np.zeros(self.state_size, dtype=np.float32)
                for tap in range(self.kernel_size):
                    convolved += self.kernel[layer, :, tap] * history[:, (position - tap * dilation) % history.shape[1]]
                self.positions[layer] = (position + 1) % history.shape[1]
                value = (value + np.tanh(convolved)).astype(np.float32)
            features[sample_index] = value
        outputs = features @ self.output_weight + self.output_bias[0] + self.residual_gain[0] * samples
        cache = FeatureCache(np.column_stack((samples, np.repeat(self.controls[None, :], samples.size, axis=0))).astype(np.float32),
                             features, outputs.astype(np.float32))
        return (cache.outputs, cache) if retain_cache else cache.outputs

    def process(self, audio): return self.forward(audio)
    def parameters(self): return {"output_weight": self.output_weight, "output_bias": self.output_bias,
                                   "residual_gain": self.residual_gain}
    def save_checkpoint(self, path: Path) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        np.savez(path, architecture=self.architecture, sample_rate=self.sample_rate, state_size=self.state_size,
                 layers=self.layers, kernel_size=self.kernel_size, input_projection=self.input_projection,
                 control_projection=self.control_projection, kernel=self.kernel,
                 output_weight=self.output_weight, output_bias=self.output_bias, residual_gain=self.residual_gain)

    @classmethod
    def load_checkpoint(cls, path: Path) -> "CausalTcn":
        data = np.load(path); model = cls(int(data["state_size"]), int(data["sample_rate"]), layers=int(data["layers"]), kernel_size=int(data["kernel_size"]))
        for name in ("input_projection", "control_projection", "kernel", "output_weight", "output_bias", "residual_gain"):
            getattr(model, name)[...] = data[name]
        model.reset(); return model


def load_model_checkpoint(path: Path):
    data = np.load(path)
    architecture = str(data["architecture"]) if "architecture" in data else "tiny_tanh_rnn"
    data.close()
    if architecture == ConditionedLstm.architecture: return ConditionedLstm.load_checkpoint(path)
    if architecture == ConditionedGru.architecture: return ConditionedGru.load_checkpoint(path)
    if architecture == CausalTcn.architecture: return CausalTcn.load_checkpoint(path)
    from .tiny_rnn import TinyTanhRnn
    return TinyTanhRnn.load_checkpoint(path)
