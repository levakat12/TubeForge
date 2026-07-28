from __future__ import annotations

from pathlib import Path
import hashlib
import json
import struct

import numpy as np
from numpy.typing import NDArray

from ..models import CausalTcn, ConditionedGru, ConditionedLstm, TinyTanhRnn
from ..schemas.model import ModelManifest

PACKED_HEADER = struct.Struct("<4s6I")
PACKED_HEADER_V2 = struct.Struct("<4s11I")


def pack_model(model) -> bytes:
    if isinstance(model, TinyTanhRnn):
        header = PACKED_HEADER.pack(b"NTSM", 1, 1, model.sample_rate, 1, model.state_size, 1)
        parameters = model.parameters().values()
    else:
        architecture = {ConditionedLstm: 2, ConditionedGru: 3, CausalTcn: 4}.get(type(model))
        if architecture is None: raise ValueError(f"Unsupported packed model: {type(model).__name__}")
        flags = int(bool(getattr(model, "residual", True)))
        auxiliary1 = int(getattr(model, "layers", 1)); auxiliary2 = int(getattr(model, "kernel_size", 1))
        header = PACKED_HEADER_V2.pack(b"NTSM", 2, architecture, model.sample_rate, 1,
                                       model.state_size, 1, model.control_count, 0, flags,
                                       auxiliary1, auxiliary2)
        if isinstance(model, ConditionedLstm):
            parameters = model.parameters().values()
        elif isinstance(model, ConditionedGru):
            parameters = (model.input_weight, model.recurrent_weight, model.bias,
                          model.output_weight, model.output_bias)
        else:
            parameters = (model.input_projection, model.control_projection, model.kernel,
                          model.output_weight, model.output_bias, model.residual_gain)
    payload = b"".join(np.asarray(parameter, dtype="<f4").tobytes(order="C") for parameter in parameters)
    return header + payload


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
