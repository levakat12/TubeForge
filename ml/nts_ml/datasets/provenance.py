from __future__ import annotations

import hashlib
import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from .package import SessionPackage, validate_session

CORPUS_CATEGORIES = (
    "clean-guitar-arpeggio", "crunch-guitar-open-chords",
    "high-gain-guitar-palm-mutes", "lead-guitar-bends-sustain",
    "clean-fingerstyle-bass", "picked-bass",
    "distorted-bass-sustained-fundamentals", "slap-bass-transients",
)


@dataclass(frozen=True, slots=True)
class TakeProvenance:
    take_id: str
    performance_id: str
    category: str
    creator: str
    license_id: str
    license_uri: str
    release_document: str
    rights_cleared: bool
    synthetic: bool
    input_sha256: str

    @classmethod
    def from_dict(cls, data: dict[str, Any]) -> TakeProvenance:
        required = ("takeId", "performanceId", "category", "creator", "licenseId", "licenseUri",
                    "releaseDocument", "rightsCleared", "synthetic", "inputSha256")
        missing = [field for field in required if field not in data]
        if missing:
            raise ValueError(f"Missing provenance fields: {', '.join(missing)}")
        result = cls(str(data["takeId"]), str(data["performanceId"]), str(data["category"]),
                     str(data["creator"]), str(data["licenseId"]), str(data["licenseUri"]),
                     str(data["releaseDocument"]), bool(data["rightsCleared"]),
                     bool(data["synthetic"]), str(data["inputSha256"]).lower())
        if not all((result.take_id, result.performance_id, result.creator, result.license_id,
                    result.license_uri, result.release_document)):
            raise ValueError("Provenance text fields cannot be empty")
        if result.category not in CORPUS_CATEGORIES:
            raise ValueError(f"Unknown performance category: {result.category}")
        if len(result.input_sha256) != 64 or any(character not in "0123456789abcdef"
                                                 for character in result.input_sha256):
            raise ValueError("inputSha256 must be a lowercase SHA-256 digest")
        return result


def load_provenance(root: Path) -> list[TakeProvenance]:
    data = json.loads((Path(root) / "provenance.json").read_text(encoding="utf-8"))
    if data.get("schemaVersion") != 1:
        raise ValueError("Unsupported provenance schema version")
    return [TakeProvenance.from_dict(item) for item in data.get("takes", [])]


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with Path(path).open("rb") as source:
        while block := source.read(1024 * 1024):
            digest.update(block)
    return digest.hexdigest()


def audit_real_corpus(packages: list[SessionPackage], report_path: Path | None = None) -> dict[str, Any]:
    gaps: list[str] = []
    records: list[dict[str, Any]] = []
    seen_hashes: set[str] = set()
    seen_performances: set[str] = set()
    categories: set[str] = set()
    instruments: set[str] = set()
    for package in packages:
        instruments.add(package.metadata.instrument)
        quality = validate_session(package, write_report=False)
        if not quality.accepted:
            failed = [check.name for check in quality.checks if not check.passed]
            gaps.append(f"{package.metadata.session_id}: paired-session validation failed: {failed}")
        try:
            provenance = load_provenance(package.root)
        except (FileNotFoundError, ValueError, json.JSONDecodeError) as error:
            gaps.append(f"{package.metadata.session_id}: invalid provenance: {error}")
            continue
        by_take = {item.take_id: item for item in provenance}
        for take in package.takes:
            item = by_take.get(take.take_id)
            if item is None:
                gaps.append(f"{package.metadata.session_id}/{take.take_id}: missing provenance")
                continue
            actual_hash = file_sha256(take.input_path)
            if actual_hash != item.input_sha256:
                gaps.append(f"{package.metadata.session_id}/{take.take_id}: input hash mismatch")
            if not item.rights_cleared:
                gaps.append(f"{package.metadata.session_id}/{take.take_id}: rights are not cleared")
            if item.synthetic:
                gaps.append(f"{package.metadata.session_id}/{take.take_id}: synthetic audio is not real corpus evidence")
            if not (package.root / item.release_document).is_file():
                gaps.append(f"{package.metadata.session_id}/{take.take_id}: release document is missing")
            if actual_hash in seen_hashes:
                gaps.append(f"{package.metadata.session_id}/{take.take_id}: duplicate input audio")
            seen_hashes.add(actual_hash)
            seen_performances.add(item.performance_id)
            categories.add(item.category)
            records.append({"sessionId": package.metadata.session_id, "takeId": item.take_id,
                            "instrument": package.metadata.instrument, "performanceId": item.performance_id,
                            "category": item.category, "licenseId": item.license_id,
                            "inputSha256": actual_hash})
        unknown_takes = set(by_take) - {take.take_id for take in package.takes}
        if unknown_takes:
            gaps.append(f"{package.metadata.session_id}: provenance names unknown takes {sorted(unknown_takes)}")
    missing_categories = sorted(set(CORPUS_CATEGORIES) - categories)
    if missing_categories:
        gaps.append(f"missing categories: {missing_categories}")
    if not {"guitar", "bass"}.issubset(instruments):
        gaps.append("corpus must contain both guitar and bass sessions")
    if len(seen_performances) < len(CORPUS_CATEGORIES):
        gaps.append(f"need at least {len(CORPUS_CATEGORIES)} independent performances")
    report = {"schemaVersion": 1, "accepted": not gaps, "sessions": len(packages),
              "performances": len(seen_performances), "categories": sorted(categories),
              "records": records, "gaps": gaps}
    if report_path is not None:
        destination = Path(report_path)
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return report
