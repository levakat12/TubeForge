from __future__ import annotations

import numpy as np
from numpy.typing import NDArray

from .reader import LayerSpec, WaveNetSpec


class _Layer:
    __slots__ = ("spec", "conv", "conv_bias", "mixin", "one", "one_bias", "history")

    def __init__(self, spec: LayerSpec, channels: int, condition_size: int,
                 take: _Cursor) -> None:
        self.spec = spec
        self.conv = take.tensor(channels, channels, spec.kernel_size)
        self.conv_bias = take.tensor(channels)
        self.mixin = take.tensor(channels, condition_size)
        self.one = take.tensor(channels, channels)
        self.one_bias = take.tensor(channels)
        self.history = np.zeros((channels, (spec.kernel_size - 1) * spec.dilation), dtype=np.float32)

    def reset(self) -> None: self.history.fill(0.0)


class _Cursor:
    """Sequential reader over the flat weight vector, in module-definition order."""

    def __init__(self, weights: NDArray[np.float32]) -> None:
        self.weights, self.offset = weights, 0

    def tensor(self, *shape: int) -> NDArray[np.float32]:
        count = int(np.prod(shape))
        values = self.weights[self.offset:self.offset + count]
        if values.size != count: raise ValueError("Weight vector is shorter than the geometry implies")
        self.offset += count
        return np.array(values, dtype=np.float32).reshape(shape)

    def scalar(self) -> float: return float(self.tensor(1)[0])


def _activate(values: NDArray[np.float32], layer: LayerSpec) -> NDArray[np.float32]:
    if layer.activation == "LeakyReLU":
        return np.where(values >= 0.0, values, values * np.float32(layer.negative_slope)).astype(np.float32)
    if layer.activation == "ReLU": return np.maximum(values, np.float32(0.0))
    return np.tanh(values).astype(np.float32)


class WaveNetModel:
    """Streaming NAM WaveNet inference in NumPy.

    The layout this parses is derived from the module definition order and verified two ways: the
    float count it implies matches both tiers of every corpus capture exactly, and the trailing
    scalar it reads as `head_scale` equals the value the file states in its config. Correctness of
    the *ordering* is established by `tests/test_nam_reader.py`'s parity check against the
    reference implementation, not by either of those.

    Blocks are processed with the whole buffer at once -- one matmul per convolution tap rather
    than a per-sample loop -- because rendering a distillation corpus means running hours of audio
    through this. Left-edge history is retained per layer, so output does not depend on how the
    input is chopped up; `process` on one long block and on many short ones agree exactly.
    """

    def __init__(self, spec: WaveNetSpec) -> None:
        self.spec = spec
        cursor = _Cursor(spec.weights)
        self.rechannel = cursor.tensor(spec.channels, spec.input_size)
        self.layers = [_Layer(layer, spec.channels, spec.condition_size, cursor)
                       for layer in spec.layers]
        self.head = cursor.tensor(spec.channels, spec.head_kernel_size)
        self.head_bias = cursor.scalar() if spec.head_bias else 0.0
        self.head_scale = cursor.scalar()
        if cursor.offset != spec.weights.size:
            raise ValueError(f"Weight vector has {spec.weights.size - cursor.offset} unread floats")
        self.head_history = np.zeros((spec.channels, spec.head_kernel_size - 1), dtype=np.float32)

    @property
    def receptive_field(self) -> int: return self.spec.receptive_field

    def reset(self, prime: bool = True) -> None:
        """Return to the state the model has when it has been fed silence forever.

        That is not all-zero history. Every layer has a convolution bias, so the response to a
        silent input is a non-zero constant that propagates up the stack; zeroing the buffers
        instead leaves the model in a state it can never reach from audio, and its output differs
        from the reference for a full receptive field (132 ms) afterwards. The reference
        implementation gets this for free by zero-padding its input; a streaming model has to
        prime deliberately. `prime=False` exists for tests that need the raw zero state.
        """
        for layer in self.layers: layer.reset()
        self.head_history.fill(0.0)
        if prime: self.process(np.zeros(self.receptive_field, dtype=np.float32))

    def process(self, audio: NDArray[np.floating]) -> NDArray[np.float32]:
        samples = np.asarray(audio, dtype=np.float32).reshape(1, -1)
        if samples.size == 0: return np.zeros(0, dtype=np.float32)
        trunk = (self.rechannel @ samples).astype(np.float32)
        head_input = np.zeros_like(trunk)
        for layer in self.layers:
            padded = np.concatenate((layer.history, trunk), axis=1)
            dilation, taps = layer.spec.dilation, layer.spec.kernel_size
            convolved = np.zeros_like(trunk)
            for tap in range(taps):
                start = tap * dilation
                convolved += layer.conv[:, :, tap] @ padded[:, start:start + trunk.shape[1]]
            layer.history = np.array(padded[:, padded.shape[1] - layer.history.shape[1]:],
                                     dtype=np.float32) if layer.history.size else layer.history
            convolved += layer.conv_bias[:, None] + layer.mixin @ samples
            activated = _activate(convolved, layer.spec)
            head_input += activated
            trunk = (trunk + layer.one @ activated + layer.one_bias[:, None]).astype(np.float32)
        padded_head = np.concatenate((self.head_history, head_input), axis=1)
        output = np.zeros(head_input.shape[1], dtype=np.float32)
        for tap in range(self.spec.head_kernel_size):
            output += self.head[:, tap] @ padded_head[:, tap:tap + head_input.shape[1]]
        if self.head_history.size:
            self.head_history = np.array(padded_head[:, padded_head.shape[1] - self.head_history.shape[1]:],
                                         dtype=np.float32)
        return ((output + np.float32(self.head_bias)) * np.float32(self.head_scale)).astype(np.float32)

    def process_blocks(self, audio: NDArray[np.floating], block_size: int) -> NDArray[np.float32]:
        samples = np.asarray(audio, dtype=np.float32).reshape(-1)
        return np.concatenate([self.process(samples[start:start + block_size])
                               for start in range(0, samples.size, block_size)]) \
            if samples.size else np.zeros(0, dtype=np.float32)
