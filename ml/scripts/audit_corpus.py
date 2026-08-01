from __future__ import annotations

import argparse
import json
from pathlib import Path

from nts_ml.datasets import SessionPackage, audit_real_corpus

parser = argparse.ArgumentParser(description="Audit real-audio provenance and Phase 4 corpus diversity")
parser.add_argument("--session", type=Path, action="append", required=True)
parser.add_argument("--report", type=Path, required=True)
options = parser.parse_args()
result = audit_real_corpus([SessionPackage(path) for path in options.session], options.report)
print(json.dumps(result, indent=2, sort_keys=True))
raise SystemExit(0 if result["accepted"] else 2)
