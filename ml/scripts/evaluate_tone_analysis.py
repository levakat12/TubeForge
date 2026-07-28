from __future__ import annotations

import argparse
import json
from dataclasses import asdict
from pathlib import Path

import numpy as np

from nts_ml.tone import CONTROLLED_RIG_FAMILIES, LinearToneEncoder, evaluate_tone_encoder


def main() -> int:
    parser = argparse.ArgumentParser(description="Run the controlled Phase 7 tone-embedding benchmark")
    parser.add_argument("--seed", type=int, default=701)
    parser.add_argument("--performances", type=int, default=12,
                        help="DI/performance variants rendered through each rig")
    parser.add_argument("--output", type=Path, help="optional JSON result path")
    arguments = parser.parse_args()
    if arguments.performances < 4:
        parser.error("--performances must be at least 4")

    random = np.random.default_rng(arguments.seed)
    family_count = len(CONTROLLED_RIG_FAMILIES)
    rigs_per_family = 2
    rig_count = family_count * rigs_per_family
    family_centres = random.normal(0.0, 2.4, (family_count, 64))
    rig_offsets = random.normal(0.0, 0.32, (rig_count, 64))
    performance_offsets = random.normal(0.0, 0.12, (arguments.performances, 64))
    features: list[np.ndarray] = []
    rig_labels: list[int] = []
    family_labels: list[int] = []
    for rig in range(rig_count):
        family = rig // rigs_per_family
        for performance in range(arguments.performances):
            features.append(family_centres[family] + rig_offsets[rig]
                            + performance_offsets[performance] + random.normal(0.0, 0.035, 64))
            rig_labels.append(rig)
            family_labels.append(family)

    values = np.asarray(features, dtype=np.float32)
    rig = np.asarray(rig_labels, dtype=np.int64)
    family = np.asarray(family_labels, dtype=np.int64)
    model = LinearToneEncoder()
    model.fit_contrastive(values, rig, "phase7-controlled-rigs-v1")
    instrument = (family >= 5).astype(np.int64)
    probabilities = np.where(instrument == 1, 0.92, 0.08)
    result = evaluate_tone_encoder(
        model, values, rig, family,
        values + random.normal(0.0, 0.025, values.shape),
        values * random.uniform(0.25, 1.75, (values.shape[0], 1)),
        values + random.normal(0.0, 0.055, values.shape),
        values + random.normal(0.0, 0.085, values.shape),
        random.normal(14.0, 3.0, (64, 64)), instrument, probabilities)
    payload = {"modelVersion": model.version, "families": list(CONTROLLED_RIG_FAMILIES),
               "rigs": rig_count, "performancesPerRig": arguments.performances,
               "metrics": asdict(result)}
    encoded = json.dumps(payload, indent=2, sort_keys=True)
    if arguments.output:
        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        arguments.output.write_text(encoded + "\n", encoding="utf-8")
    print(encoded)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
