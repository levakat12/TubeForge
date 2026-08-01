from __future__ import annotations

import argparse
import json
import uuid
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description="Create an empty paired-audio capture session")
    parser.add_argument("destination", type=Path)
    parser.add_argument("--sample-rate", type=int, default=48_000)
    parser.add_argument("--bit-depth", type=int, choices=(16, 24, 32), default=24)
    parser.add_argument("--instrument", choices=("guitar", "bass", "other"), default="guitar")
    parser.add_argument("--input-device", required=True)
    parser.add_argument("--target-type", default="plugin")
    parser.add_argument("--target-name", default="OriginalTarget")
    options = parser.parse_args()
    for directory in ("input", "output", "calibration", "reports"):
        (options.destination / directory).mkdir(parents=True, exist_ok=True)
    metadata = {
        "schemaVersion": 1,
        "sessionId": str(uuid.uuid4()),
        "sampleRate": options.sample_rate,
        "bitDepth": options.bit_depth,
        "instrument": options.instrument,
        "inputDevice": options.input_device,
        "targetType": options.target_type,
        "targetName": options.target_name,
        "targetSettings": {},
        "inputCalibrationDb": 0.0,
        "outputCalibrationDb": 0.0,
        "latencySamples": 0,
        "notes": "",
    }
    (options.destination / "session.json").write_text(
        json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return 0


raise SystemExit(main())
