from __future__ import annotations

from pathlib import Path
import argparse
import json

from nts_ml.evaluation import create_abx_pack, load_abx_sources


parser = argparse.ArgumentParser(description="Create a loudness-matched, randomized TubeForge ABX pack")
parser.add_argument("manifest", type=Path)
parser.add_argument("destination", type=Path)
parser.add_argument("--trials", type=int, default=12)
parser.add_argument("--seed", type=int, default=0x54554245)
options = parser.parse_args()
result = create_abx_pack(load_abx_sources(options.manifest), options.destination,
                         options.trials, options.seed)
print(json.dumps({"trials": len(result["trials"]), "destination": str(options.destination)}, indent=2))
