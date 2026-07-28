from __future__ import annotations

from dataclasses import dataclass
from typing import Iterable
import hashlib


@dataclass(frozen=True, slots=True)
class SplitItem:
    session_id: str
    performance_id: str


def split_by_performance(items: Iterable[SplitItem], seed: int = 0,
                         train_fraction: float = 0.70,
                         validation_fraction: float = 0.15) -> dict[str, list[SplitItem]]:
    if not 0.0 < train_fraction < 1.0 or not 0.0 <= validation_fraction < 1.0:
        raise ValueError("Invalid split fractions")
    if train_fraction + validation_fraction >= 1.0:
        raise ValueError("A nonempty test fraction is required")
    grouped: dict[str, list[SplitItem]] = {}
    for item in items:
        grouped.setdefault(f"{item.session_id}:{item.performance_id}", []).append(item)
    result: dict[str, list[SplitItem]] = {"train": [], "validation": [], "test": []}
    for key, group in sorted(grouped.items()):
        digest = hashlib.sha256(f"{seed}:{key}".encode()).digest()
        value = int.from_bytes(digest[:8], "little") / float(2**64)
        split = "train" if value < train_fraction else (
            "validation" if value < train_fraction + validation_fraction else "test")
        result[split].extend(group)
    return result


def assert_no_phrase_leakage(splits: dict[str, list[SplitItem]]) -> None:
    ownership: dict[tuple[str, str], str] = {}
    for split, items in splits.items():
        for item in items:
            key = (item.session_id, item.performance_id)
            previous = ownership.setdefault(key, split)
            if previous != split:
                raise ValueError(f"Performance {key} leaks between {previous} and {split}")
