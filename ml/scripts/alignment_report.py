from __future__ import annotations

import argparse
from pathlib import Path

from nts_ml.alignment import write_alignment_html

parser = argparse.ArgumentParser(description="Render an HTML alignment inspection report")
parser.add_argument("validation_json", type=Path)
parser.add_argument("destination", type=Path)
options = parser.parse_args()
write_alignment_html(options.validation_json, options.destination)
