from __future__ import annotations

import argparse
import json
from pathlib import Path

from nts_ml.evaluation import analyze_abx_results

parser = argparse.ArgumentParser(description="Analyze completed TubeForge ABX response sheets")
parser.add_argument("pack", type=Path)
parser.add_argument("responses", type=Path)
parser.add_argument("report", type=Path)
options = parser.parse_args()
result = analyze_abx_results(options.pack, options.responses, options.report)
print(json.dumps(result["aggregate"], indent=2, sort_keys=True))
