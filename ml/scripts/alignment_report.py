from __future__ import annotations

from pathlib import Path
import argparse

from nts_ml.alignment import write_alignment_html


parser = argparse.ArgumentParser(description="Render an HTML alignment inspection report")
parser.add_argument("validation_json", type=Path)
parser.add_argument("destination", type=Path)
options = parser.parse_args()
write_alignment_html(options.validation_json, options.destination)
