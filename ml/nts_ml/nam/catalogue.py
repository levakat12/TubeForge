from __future__ import annotations

import hashlib
import re
from collections.abc import Sequence
from pathlib import Path
from typing import Any

from .reader import UnsupportedCapture, load_capture

CONTROL_TOKENS = {"P": "presence", "B": "bass", "M": "mid", "T": "treble", "MV": "master", "G": "gain"}
MICROPHONES = {
    r"\bsm ?57\b": "SM57", r"\bsm ?58\b": "SM58", r"\bsm ?75\b": "SM7B",
    r"\bmd ?421\b": "MD421", r"\br[- ]?121\b": "Royer R-121", r"\br[- ]?101\b": "Royer R-101",
    r"\b(?:akg )?c? ?414\b": "AKG C414", r"\broom only\b": "room",
}
STAGES = {"[amp]": "amp", "[pre]": "preamp", "[pow]": "power-amp"}
BOOSTS = {r"\bmxr\b": "MXR", r"\bmaxon\b": "Maxon", r"\bts\b": "Tube Screamer",
          r"\bno boost\b": "none"}
CHANNELS = {"clean": "clean", "crnch": "crunch", "crunch": "crunch", "crush": "crush",
            "lead": "lead", "rhythm": "rhythm"}


def parse_settings(name: str) -> dict[str, Any]:
    """Recover amplifier settings from the filename conventions the corpus uses.

    The conventions are per-author and not machine-written, so this is best effort by design:
    anything unrecognised is simply absent from the result rather than guessed at. The value is in
    the series that *do* follow a convention -- the 30-point JCM800 gain/master grid, the Mesa
    wattage and channel sweep -- which is what makes the corpus usable as reference data.
    """
    settings: dict[str, Any] = {}
    lowered = name.lower()
    for token, label in STAGES.items():
        if token in lowered: settings["stage"] = label
    for pattern, label in MICROPHONES.items():
        if re.search(pattern, lowered): settings.setdefault("microphones", []).append(label)
    for pattern, label in BOOSTS.items():
        if re.search(pattern, lowered): settings["boost"] = label
    for token, label in CHANNELS.items():
        if re.search(rf"\b{token}\b", lowered): settings["channel"] = label
    controls = {}
    for token, label in CONTROL_TOKENS.items():
        match = re.search(rf"(?:^|[\s\-]){token}(\d{{1,2}})(?:\b)", name)
        if match: controls[label] = int(match.group(1))
    for label, pattern in (("gain", r"\bgain (\d{1,2})\b"), ("volume", r"\bvolume (\d{1,2})\b")):
        match = re.search(pattern, lowered)
        if match: controls.setdefault(label, int(match.group(1)))
    if controls: settings["controls"] = controls
    watts = re.search(r"\b(\d{2,3})w\b", lowered)
    if watts: settings["watts"] = int(watts.group(1))
    if re.search(r"\bbright on\b", lowered): settings["bright"] = True
    if re.search(r"\bbright off\b", lowered): settings["bright"] = False
    if re.search(r"\bfull rig\b", lowered): settings["fullRig"] = True
    if re.search(r"\b(?:- )?di\b", lowered): settings["directOut"] = True
    return settings


def describe(path: Path, root: Path | None = None) -> dict[str, Any]:
    capture = load_capture(path)
    entry: dict[str, Any] = {
        "file": path.name,
        "relativePath": str(path.relative_to(root)) if root else str(path),
        "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
        "sampleRate": capture.sample_rate,
        "formatVersion": capture.version,
        "settings": parse_settings(path.stem),
    }
    entry.update(capture.metadata.to_dict())
    entry["tiers"] = {tier: {"channels": spec.channels, "weightCount": spec.weights.size,
                             "layers": len(spec.layers), "receptiveField": spec.receptive_field,
                             "macsPerSample": spec.macs_per_sample}
                      for tier, spec in capture.tiers.items()}
    return entry


def build_catalogue(roots: Sequence[Path]) -> dict[str, Any]:
    """Index every readable capture under `roots`, recording the unreadable ones rather than
    dropping them silently."""
    captures: list[dict[str, Any]] = []
    rejected: list[dict[str, str]] = []
    for root in roots:
        base = root if root.is_dir() else root.parent
        for path in sorted(base.rglob("*.nam") if root.is_dir() else [root]):
            try:
                captures.append(describe(path, base))
            except (UnsupportedCapture, ValueError, OSError) as error:
                rejected.append({"relativePath": str(path.relative_to(base)), "reason": str(error)})
    makes = sorted({entry["gearMake"] for entry in captures if entry.get("gearMake")})
    return {"schemaVersion": 1, "captures": captures, "rejected": rejected,
            "summary": {"count": len(captures), "rejected": len(rejected), "gearMakes": makes}}
