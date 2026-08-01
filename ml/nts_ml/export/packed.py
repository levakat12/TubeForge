from __future__ import annotations

import hashlib
import json
import struct
from pathlib import Path

import numpy as np
from numpy.typing import NDArray

from ..models import ConditionedLstm, RandomFeatureGru, RandomFeatureTcn, TinyTanhRnn
from ..schemas.model import ModelManifest

PACKED_HEADER = struct.Struct("<4s6I")
PACKED_HEADER_V2 = struct.Struct("<4s11I")
# v3 exists because WaveNet geometry does not fit in a fixed header: kernel size and dilation vary
# per layer, so the header is followed by a layer table of that many u32 pairs. Fields, in order:
# version, architecture, sampleRate, inputChannels, channels, outputChannels, controlCount,
# layerCount, flags, headKernel, reserved.
PACKED_HEADER_V3 = struct.Struct("<4s11I")
WAVENET_ARCHITECTURE = 5
MAXIMUM_WAVENET_LAYERS = 64
MAXIMUM_WAVENET_KERNEL = 64
MAXIMUM_WAVENET_DILATION = 4096


def pack_model(model) -> bytes:
    if isinstance(model, TinyTanhRnn):
        header = PACKED_HEADER.pack(b"NTSM", 1, 1, model.sample_rate, 1, model.state_size, 1)
        parameters = model.parameters().values()
    else:
        architecture = {ConditionedLstm: 2, RandomFeatureGru: 3, RandomFeatureTcn: 4}.get(type(model))
        if architecture is None: raise ValueError(f"Unsupported packed model: {type(model).__name__}")
        flags = int(bool(getattr(model, "residual", True)))
        auxiliary1 = int(getattr(model, "layers", 1)); auxiliary2 = int(getattr(model, "kernel_size", 1))
        header = PACKED_HEADER_V2.pack(b"NTSM", 2, architecture, model.sample_rate, 1,
                                       model.state_size, 1, model.control_count, 0, flags,
                                       auxiliary1, auxiliary2)
        if isinstance(model, ConditionedLstm):
            parameters = model.parameters().values()
        elif isinstance(model, RandomFeatureGru):
            parameters = (model.input_weight, model.recurrent_weight, model.bias,
                          model.output_weight, model.output_bias)
        else:
            parameters = (model.input_projection, model.control_projection, model.kernel,
                          model.output_weight, model.output_bias, model.residual_gain)
    payload = b"".join(np.asarray(parameter, dtype="<f4").tobytes(order="C") for parameter in parameters)
    return header + payload


def pack_wavenet(spec) -> bytes:
    """Pack a NAM WaveNet capture as NTSM v3.

    The weight payload is copied through in the order the capture file already uses, so the
    converter does not reinterpret weights it does not need to understand -- the runtime and
    `nts_ml.nam.wavenet` agree on that order, and the exported test vectors prove it.
    """
    if len(spec.layers) > MAXIMUM_WAVENET_LAYERS:
        raise ValueError(f"WaveNet captures are limited to {MAXIMUM_WAVENET_LAYERS} layers")
    for layer in spec.layers:
        if not 2 <= layer.kernel_size <= MAXIMUM_WAVENET_KERNEL:
            raise ValueError(f"Kernel size {layer.kernel_size} is out of range")
        if not 1 <= layer.dilation <= MAXIMUM_WAVENET_DILATION:
            raise ValueError(f"Dilation {layer.dilation} is out of range")
        if layer.activation != "LeakyReLU" or abs(layer.negative_slope - 0.01) > 1.0e-9:
            raise ValueError("The packed runtime implements LeakyReLU(0.01) layers only")
    if spec.input_size != 1 or spec.condition_size != 1:
        raise ValueError("Packed WaveNet supports single-channel input and condition only")
    header = PACKED_HEADER_V3.pack(b"NTSM", 3, WAVENET_ARCHITECTURE, spec.sample_rate, 1,
                                   spec.channels, 1, 0, len(spec.layers), int(spec.head_bias),
                                   spec.head_kernel_size, 0)
    table = b"".join(struct.pack("<2I", layer.kernel_size, layer.dilation) for layer in spec.layers)
    return header + table + np.asarray(spec.weights, dtype="<f4").tobytes(order="C")


def export_wavenet(capture, destination: Path, tier: str = "standard",
                   test_input: NDArray[np.floating] | None = None,
                   expected_input_rms_db: float | None = None) -> ModelManifest:
    """Convert a NAM capture into the artifact directory the plug-in already knows how to load.

    The test vectors are rendered by `nts_ml.nam.wavenet`, which is checked against the reference
    implementation. That makes the plug-in's existing test-vector validation a real correctness gate
    on the C++ WaveNet rather than a formality: an inference bug on the runtime side cannot load.

    The vector deliberately starts from a primed state, because that is what the runtime must
    reproduce after a reset -- an implementation that zeroes its buffers instead disagrees for a
    full receptive field and fails here, which is the intent.
    """
    from ..nam.wavenet import WaveNetModel

    spec = capture.spec(tier)
    destination.mkdir(parents=True, exist_ok=True)
    vectors = destination / "test-vectors"
    vectors.mkdir(exist_ok=True)
    packed = pack_wavenet(spec)
    (destination / "model.bin").write_bytes(packed)
    if test_input is None:
        random = np.random.default_rng(417)
        test_input = random.normal(0.0, 0.08, 4096).astype(np.float32)
    vector = np.asarray(test_input, dtype="<f4").reshape(-1)
    model = WaveNetModel(spec)
    model.reset()
    expected = model.process(vector).astype("<f4")
    vector.tofile(vectors / "input.f32")
    expected.tofile(vectors / "output.f32")
    (vectors / "metadata.json").write_text(json.dumps({
        "samples": int(vector.size),
        "maximumAbsoluteErrorTolerance": 1.0e-4,
        "rmsErrorTolerance": 1.0e-5,
        "accumulatedDriftTolerance": 1.0e-3,
        "stateResetMaximumError": 1.0e-6,
        "primedState": True,
        "receptiveFieldSamples": spec.receptive_field,
    }, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    loudness = capture.metadata.loudness
    rms_db = expected_input_rms_db if expected_input_rms_db is not None else (
        float(loudness) if loudness is not None else -21.0)
    manifest = ModelManifest(
        model_format_version=3,
        architecture="wavenet",
        sample_rate=spec.sample_rate,
        input_channels=1,
        output_channels=1,
        state_size=spec.receptive_field,
        latency_samples=0,
        expected_input_rms_db=max(-60.0, min(0.0, rms_db)),
        parameter_schema=[],
        sha256=hashlib.sha256(packed).hexdigest(),
    )
    manifest.save(destination / "manifest.json")
    (destination / "normalization.json").write_text(json.dumps({
        "inputRmsDb": manifest.expected_input_rms_db,
        "inputScale": 1.0,
        "outputScale": 1.0,
        "dcOffset": 0.0,
    }, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    (destination / "license.txt").write_text(capture.metadata.license_text(), encoding="utf-8")
    return manifest


def export_model(model, destination: Path,
                 test_input: NDArray[np.floating] | None = None,
                 expected_input_rms_db: float = -21.0,
                 license_text: str = "User-provided model; rights and redistribution terms must be documented.\n",
                 parameter_schema: list[dict[str, object]] | None = None) -> ModelManifest:
    destination.mkdir(parents=True, exist_ok=True)
    vectors = destination / "test-vectors"
    vectors.mkdir(exist_ok=True)
    packed = pack_model(model)
    (destination / "model.bin").write_bytes(packed)
    digest = hashlib.sha256(packed).hexdigest()
    default_schema = [{"id": name, "minimum": -1.0, "maximum": 1.0, "default": 0.0}
                      for name in ("gain", "tone", "master", "channel", "instrument_mode")]
    manifest = ModelManifest(
        model_format_version=1 if isinstance(model, TinyTanhRnn) else 2,
        architecture=model.architecture,
        sample_rate=model.sample_rate,
        input_channels=1,
        output_channels=1,
        state_size=model.state_size,
        latency_samples=0,
        expected_input_rms_db=expected_input_rms_db,
        parameter_schema=parameter_schema if parameter_schema is not None else (
            default_schema if hasattr(model, "control_count") else []),
        sha256=digest,
    )
    manifest.save(destination / "manifest.json")
    (destination / "normalization.json").write_text(json.dumps({
        "inputRmsDb": expected_input_rms_db,
        "inputScale": 1.0,
        "outputScale": 1.0,
        "dcOffset": 0.0,
    }, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    (destination / "license.txt").write_text(license_text, encoding="utf-8")
    if test_input is None:
        random = np.random.default_rng(417)
        test_input = random.normal(0.0, 0.08, 2048).astype(np.float32)
    vector = np.asarray(test_input, dtype="<f4").reshape(-1)
    model.reset()
    expected = model.process(vector).astype("<f4")
    vector.tofile(vectors / "input.f32")
    expected.tofile(vectors / "output.f32")
    (vectors / "metadata.json").write_text(json.dumps({
        "samples": int(vector.size),
        "maximumAbsoluteErrorTolerance": 1.0e-5,
        "rmsErrorTolerance": 2.0e-6,
        "accumulatedDriftTolerance": 1.0e-4,
        "stateResetMaximumError": 1.0e-7,
    }, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return manifest


def validate_artifact(path: Path) -> ModelManifest:
    required = ("model.bin", "manifest.json", "normalization.json", "test-vectors/input.f32",
                "test-vectors/output.f32", "test-vectors/metadata.json", "license.txt")
    missing = [name for name in required if not (path / name).is_file()]
    if missing:
        raise ValueError(f"Model artifact is incomplete: {', '.join(missing)}")
    manifest = ModelManifest.load(path / "manifest.json")
    actual = hashlib.sha256((path / "model.bin").read_bytes()).hexdigest()
    if actual != manifest.sha256:
        raise ValueError("model.bin SHA-256 does not match the manifest")
    normalization = json.loads((path / "normalization.json").read_text(encoding="utf-8"))
    if not all(name in normalization for name in ("inputRmsDb", "inputScale", "outputScale", "dcOffset")):
        raise ValueError("normalization.json is incomplete")
    inputs = np.fromfile(path / "test-vectors" / "input.f32", dtype="<f4")
    outputs = np.fromfile(path / "test-vectors" / "output.f32", dtype="<f4")
    if inputs.size == 0 or inputs.size != outputs.size:
        raise ValueError("Test vectors must be nonempty and paired")
    return manifest
