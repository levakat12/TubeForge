from __future__ import annotations

import argparse
from pathlib import Path

from nts_ml.export import export_model
from nts_ml.models import load_model_checkpoint

parser = argparse.ArgumentParser(description="Export a checkpoint to the TubeForge packed registry format")
parser.add_argument("checkpoint", type=Path)
parser.add_argument("destination", type=Path)
parser.add_argument("--license", type=Path)
options = parser.parse_args()
license_text = options.license.read_text(encoding="utf-8") if options.license else (
    "User-provided model; rights and redistribution terms must be documented.\n")
manifest = export_model(load_model_checkpoint(options.checkpoint), options.destination,
                        license_text=license_text)
print(manifest.sha256)
