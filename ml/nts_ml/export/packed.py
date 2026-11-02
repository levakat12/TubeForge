from __future__ import annotations

import hashlib
import json
import struct
from pathlib import Path

import numpy as np
from numpy.typing import NDArray

from ..models import (
    ConditionedLstm,
    ProductionRegressor,
    RandomFeatureGru,
    RandomFeatureTcn,
    TinyTanhRnn,
)
from ..schemas.model import ModelManifest

PACKED_HEADER = struct.Struct("<4s6I")
PACKED_HEADER_V2 = struct.Struct("<4s11I")
# v3 exists because WaveNet geometry does not fit in a fixed header: kernel size and dilation vary
# per layer, so the header is followed by a layer table of that many u32 pairs. Fields, in order:
# version, architecture, sampleRate, inputChannels, channels, outputChannels, controlCount,
# layerCount, flags, headKernel, reserved.
PACKED_HEADER_V3 = struct.Struct("<4s11I")
WAVENET_ARCHITECTURE = 5
# v4 is a feed-forward regressor over feature vectors rather than a per-sample audio model, so it
# gets a header of its own instead of borrowing v2's: reusing v2 would mean `inputChannels` meaning
# audio channels in one file and a feature count in the next, and a loader that guessed which from
# the architecture code. Fields: version, architecture, inputCount, hiddenCount, outputCount.
PACKED_HEADER_V4 = struct.Struct("<4s5I")
REGRESSOR_ARCHITECTURE = 6
MAXIMUM_REGRESSOR_INPUTS = 512
MAXIMUM_REGRESSOR_HIDDEN = 256
MAXIMUM_WAVENET_LAYERS = 64
MAXIMUM_WAVENET_KERNEL = 64
MAXIMUM_WAVENET_DILATION = 4096


def pack_regressor(model: ProductionRegressor) -> bytes:
    """The dense regressor as NTSM v4.

    Standardisation is packed with the weights rather than left in `normalization.json`, because a
    runtime that loaded the weights and skipped the standardisation would not fail -- it would
    produce confident nonsense, and the hash covers only what is in this payload.
    """
    if not 1 <= model.input_count <= MAXIMUM_REGRESSOR_INPUTS:
        raise ValueError(f"Regressor inputs must be 1..{MAXIMUM_REGRESSOR_INPUTS}")
    if not 1 <= model.hidden_count <= MAXIMUM_REGRESSOR_HIDDEN:
        raise ValueError(f"Regressor hidden units must be 1..{MAXIMUM_REGRESSOR_HIDDEN}")
    if model.output_count < 1:
        raise ValueError("A regressor needs at least one output")
    header = PACKED_HEADER_V4.pack(b"NTSM", 4, REGRESSOR_ARCHITECTURE, model.input_count,
                                   model.hidden_count, model.output_count)
    payload = b"".join(np.asarray(parameter, dtype="<f4").tobytes(order="C")
                       for parameter in model.parameters().values())
    return header + payload


def pack_model(model) -> bytes:
    if isinstance(model, ProductionRegressor):
        return pack_regressor(model)
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


def export_regressor(model: ProductionRegressor, destination: Path,
                     test_features: NDArray[np.floating] | None = None,
                     license_text: str = "Trained from synthetic production chains.\n",
                     ) -> ModelManifest:
    """Writes the regressor as the same artifact directory everything else here produces.

    Same shape deliberately: `validate_artifact` then covers it, the sha256 in the manifest is a
    real gate on the weights, and the test vectors are what hold the C++ evaluator to this one. The
    only thing that differs is that a vector row is `input_count` features wide rather than one
    sample, and the paired output row is `output_count` wide -- which is why `validate_artifact`
    checks row counts rather than element counts.
    """
    destination.mkdir(parents=True, exist_ok=True)
    vectors = destination / "test-vectors"
    vectors.mkdir(exist_ok=True)
    packed = pack_regressor(model)
    (destination / "model.bin").write_bytes(packed)

    if test_features is None:
        # Spread well outside the training distribution on purpose. A vector drawn from the middle
        # of the corpus would be matched by a loader that got the standardisation wrong, because
        # near the mean the standardisation barely does anything.
        random = np.random.default_rng(911)
        test_features = random.normal(0.0, 3.0, (16, model.input_count))
    features = np.asarray(test_features, dtype="<f4").reshape(-1, model.input_count)
    expected = model.predict(features).astype("<f4")
    features.tofile(vectors / "input.f32")
    expected.tofile(vectors / "output.f32")
    (vectors / "metadata.json").write_text(json.dumps({
        "rows": int(features.shape[0]),
        "inputCount": model.input_count,
        "outputCount": model.output_count,
        "maximumAbsoluteErrorTolerance": 1.0e-5,
        "rmsErrorTolerance": 2.0e-6,
    }, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    manifest = ModelManifest(
        model_format_version=4,
        architecture=model.architecture,
        sample_rate=model.sample_rate,
        input_channels=model.input_count,
        output_channels=model.output_count,
        state_size=model.hidden_count,
        latency_samples=0,
        expected_input_rms_db=-21.0,
        parameter_schema=[],
        sha256=hashlib.sha256(packed).hexdigest(),
    )
    manifest.save(destination / "manifest.json")
    (destination / "normalization.json").write_text(json.dumps({
        # Present because the artifact shape requires it, and neutral because this model carries
        # its own standardisation in the weights. Nothing should apply both.
        "inputRmsDb": -21.0,
        "inputScale": 1.0,
        "outputScale": 1.0,
        "dcOffset": 0.0,
    }, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    (destination / "license.txt").write_text(license_text, encoding="utf-8")
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
    # Paired by *row*, not by element. For every audio model here a row is one sample and both
    # counts are 1, so this is the rule it always was; for the production regressor a row is 57
    # features against 4 predictions and an element-count comparison would reject a correct
    # artifact. What has to match is how many examples each side describes.
    if inputs.size == 0 or outputs.size == 0:
        raise ValueError("Test vectors must be nonempty and paired")
    if inputs.size % manifest.input_channels or outputs.size % manifest.output_channels:
        raise ValueError("Test vectors are not a whole number of rows")
    if inputs.size // manifest.input_channels != outputs.size // manifest.output_channels:
        raise ValueError("Test vectors must be nonempty and paired")
    return manifest
