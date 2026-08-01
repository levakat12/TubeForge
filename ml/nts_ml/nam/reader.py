from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import numpy as np
from numpy.typing import NDArray

from .metadata import CaptureMetadata

SUPPORTED_VERSIONS = ("0.7.0",)
SUPPORTED_ACTIVATIONS = ("LeakyReLU", "ReLU", "Tanh")


class UnsupportedCapture(ValueError):
    """A capture this reader deliberately refuses rather than renders approximately.

    The v0.7 format can express conditioning features -- FiLM modulation, gated activations,
    grouped convolutions, a head 1x1 -- that this reader does not implement. None of the 354
    captures in `amp_learning/` and `pedal_learning/` use any of them. Rejecting loudly is the
    point: a reader that quietly ignored an active FiLM block would render plausible, wrong audio,
    and nothing downstream would catch it.
    """


@dataclass(frozen=True, slots=True)
class LayerSpec:
    kernel_size: int
    dilation: int
    activation: str
    negative_slope: float


@dataclass(frozen=True, slots=True)
class WaveNetSpec:
    """One WaveNet submodel: geometry, weights, and the scalars the head needs."""

    channels: int
    input_size: int
    condition_size: int
    head_kernel_size: int
    head_bias: bool
    head_scale: float
    sample_rate: int
    layers: tuple[LayerSpec, ...]
    weights: NDArray[np.float32]

    @property
    def weight_count(self) -> int:
        """The number of floats this geometry implies.

        Derived from the module layout, not from the file: `load_capture` checks the file against
        it, so a capture whose weight vector does not match the geometry is rejected before any
        audio is rendered. Verified against both tiers of all 354 corpus captures.
        """
        channels, per_layer = self.channels, 0
        for layer in self.layers:
            per_layer += (channels * channels * layer.kernel_size  # dilated conv
                          + channels                               # dilated conv bias
                          + channels * self.condition_size         # input mixin (no bias)
                          + channels * channels + channels)        # layer 1x1 and bias
        head = channels * self.head_kernel_size + int(self.head_bias)
        return per_layer + channels * self.input_size + head + 1   # + input rechannel, + head scale

    @property
    def receptive_field(self) -> int:
        return sum((layer.kernel_size - 1) * layer.dilation for layer in self.layers) \
            + self.head_kernel_size

    @property
    def macs_per_sample(self) -> int:
        channels = self.channels
        return sum(channels * channels * layer.kernel_size + channels * self.condition_size
                   + channels * channels for layer in self.layers) \
            + channels * self.head_kernel_size


@dataclass(frozen=True, slots=True)
class NamCapture:
    path: Path
    version: str
    sample_rate: int
    metadata: CaptureMetadata
    tiers: dict[str, WaveNetSpec]

    def spec(self, tier: str = "standard") -> WaveNetSpec:
        if tier not in self.tiers:
            raise UnsupportedCapture(f"Capture has no '{tier}' tier; available: {sorted(self.tiers)}")
        return self.tiers[tier]


def _require(condition: bool, message: str) -> None:
    if not condition: raise UnsupportedCapture(message)


def _inactive(block: Any, name: str) -> None:
    if isinstance(block, dict) and block.get("active"):
        raise UnsupportedCapture(f"{name} is active; conditioned WaveNet features are not implemented")


def _read_layer_array(config: dict[str, Any]) -> tuple[dict[str, Any], tuple[LayerSpec, ...]]:
    arrays = config.get("layers")
    _require(isinstance(arrays, list) and len(arrays) == 1,
             "Only single-layer-array WaveNet captures are supported")
    array = arrays[0]
    kernels, dilations = array.get("kernel_sizes"), array.get("dilations")
    activations = array.get("activation")
    _require(isinstance(kernels, list) and isinstance(dilations, list) and isinstance(activations, list),
             "Layer array is missing kernel_sizes, dilations, or activation")
    _require(len(kernels) == len(dilations) == len(activations),
             "Layer array kernel_sizes, dilations, and activation lengths disagree")
    _require(len(kernels) > 0, "Layer array is empty")
    channels = int(array.get("channels", 0))
    _require(channels > 0, "Layer array declares no channels")
    _require(int(array.get("bottleneck", channels)) == channels,
             "Bottleneck channels differing from layer channels are not implemented")
    for name in ("groups_input", "groups_input_mixin"):
        _require(int(array.get(name, 1)) == 1, f"Grouped convolutions ({name}) are not implemented")
    _require(int((array.get("layer1x1") or {}).get("groups", 1)) == 1,
             "Grouped layer 1x1 convolutions are not implemented")
    _require(bool((array.get("layer1x1") or {}).get("active", True)),
             "Captures without an active layer 1x1 are not implemented")
    _inactive(array.get("head1x1"), "head1x1")
    for name in ("conv_pre_film", "conv_post_film", "input_mixin_pre_film", "input_mixin_post_film",
                 "activation_pre_film", "activation_post_film", "layer1x1_post_film",
                 "head1x1_post_film"):
        _inactive(array.get(name), name)
    _require(array.get("slimmable") in (None, False), "Nested slimmable layers are not implemented")
    for mode in array.get("gating_mode", []):
        _require(str(mode).lower() in ("none", "false"), f"Gating mode '{mode}' is not implemented")
    for secondary in array.get("secondary_activation", []):
        _require(secondary is None, "Secondary activations are not implemented")

    layers: list[LayerSpec] = []
    for kernel, dilation, activation in zip(kernels, dilations, activations, strict=True):
        kind = activation.get("type") if isinstance(activation, dict) else str(activation)
        _require(kind in SUPPORTED_ACTIVATIONS, f"Activation '{kind}' is not implemented")
        slope = float(activation.get("negative_slope", 0.01)) if isinstance(activation, dict) else 0.01
        _require(int(kernel) >= 1 and int(dilation) >= 1, "Kernel sizes and dilations must be positive")
        layers.append(LayerSpec(int(kernel), int(dilation), kind, slope))
    return array, tuple(layers)


def _read_submodel(model: dict[str, Any], sample_rate: int) -> WaveNetSpec:
    _require(model.get("architecture") == "WaveNet",
             f"Submodel architecture '{model.get('architecture')}' is not implemented")
    config = model.get("config") or {}
    _require(config.get("head") is None, "Layer-array-external heads are not implemented")
    array, layers = _read_layer_array(config)
    head = array.get("head") or {}
    _require(int(head.get("out_channels", 1)) == 1, "Multi-channel heads are not implemented")
    spec = WaveNetSpec(
        channels=int(array["channels"]),
        input_size=int(array.get("input_size", 1)),
        condition_size=int(array.get("condition_size", 1)),
        head_kernel_size=int(head.get("kernel_size", 1)),
        head_bias=bool(head.get("bias", True)),
        head_scale=float(config.get("head_scale", 1.0)),
        sample_rate=sample_rate,
        layers=layers,
        weights=np.asarray(model.get("weights", []), dtype=np.float32),
    )
    _require(spec.input_size == 1 and spec.condition_size == 1,
             "Only single-channel input and condition are implemented")
    if spec.weights.size != spec.weight_count:
        raise UnsupportedCapture(f"Capture declares {spec.weights.size} weights but its geometry "
                                 f"implies {spec.weight_count}")
    _require(bool(np.all(np.isfinite(spec.weights))), "Capture contains non-finite weights")
    # The trailing weight duplicates config head_scale in every corpus file; disagreement means the
    # layout assumption is wrong for this capture, which is exactly when to stop.
    _require(abs(float(spec.weights[-1]) - spec.head_scale) <= 1.0e-6 * max(1.0, abs(spec.head_scale)),
             "Trailing weight does not match head_scale; weight layout is not as expected")
    return spec


def load_capture(path: Path | str) -> NamCapture:
    """Parse a `.nam` file into typed tiers, rejecting anything this reader does not model."""
    path = Path(path)
    document = json.loads(path.read_text(encoding="utf-8"))
    version = str(document.get("version", ""))
    _require(version in SUPPORTED_VERSIONS, f"NAM format version '{version}' is not supported")
    sample_rate = int(float(document.get("sample_rate", 0)))
    _require(sample_rate > 0, "Capture declares no sample rate")
    metadata = CaptureMetadata.from_dict(document.get("metadata"))
    architecture = document.get("architecture")
    if architecture == "WaveNet":
        return NamCapture(path, version, sample_rate, metadata,
                          {"standard": _read_submodel(document, sample_rate)})
    _require(architecture == "SlimmableContainer",
             f"Capture architecture '{architecture}' is not implemented")
    submodels = (document.get("config") or {}).get("submodels")
    _require(isinstance(submodels, list) and len(submodels) > 0, "Container declares no submodels")
    ordered = sorted(submodels, key=lambda entry: float(entry.get("max_value", 0.0)))
    tiers = {"lite": _read_submodel(ordered[0]["model"], sample_rate),
             "standard": _read_submodel(ordered[-1]["model"], sample_rate)}
    return NamCapture(path, version, sample_rate, metadata, tiers)
