from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Iterator, Sequence

import numpy as np
from numpy.typing import NDArray

from .audio import WaveReader
from .package import PairedTake, SessionPackage


@dataclass(frozen=True, slots=True)
class ChunkReference:
    session_id: str
    take_id: str
    input_path: Path
    output_path: Path
    start: int
    output_start: int
    total_samples: int
    history_samples: int
    controls: tuple[float, float, float, float, float] = (0.0, 0.0, 0.0, 0.0, 0.0)


@dataclass(slots=True)
class PairedChunk:
    input: NDArray[np.float32]
    target: NDArray[np.float32]
    loss_mask: NDArray[np.bool_]
    reference: ChunkReference


class StreamingPairedDataset(Sequence[PairedChunk]):
    """Indexes files once and reads only context+supervised frames for each item."""

    def __init__(self, sessions: Sequence[SessionPackage], chunk_samples: int,
                 history_samples: int, hop_samples: int | None = None, channel: int = 0):
        if chunk_samples < 1 or history_samples < 0:
            raise ValueError("Invalid chunk/history size")
        self.chunk_samples = chunk_samples
        self.history_samples = history_samples
        self.total_samples = chunk_samples + history_samples
        self.hop_samples = hop_samples or chunk_samples
        self.channel = channel
        self.references: list[ChunkReference] = []
        for session in sessions:
            for take in session.takes:
                self._index_take(session, take)

    def _index_take(self, session: SessionPackage, take: PairedTake) -> None:
        source, output = WaveReader(take.input_path), WaveReader(take.output_path)
        latency = session.metadata.latency_samples
        frames = min(source.info.frames, max(0, output.info.frames - latency))
        settings = session.metadata.target_settings
        raw_controls = settings.get("controlVector")
        if isinstance(raw_controls, list) and len(raw_controls) == 5:
            controls = tuple(float(np.clip(value, -1.0, 1.0)) for value in raw_controls)
        else:
            controls = tuple(float(np.clip(settings.get(name, 0.0), -1.0, 1.0))
                             for name in ("gain", "tone", "master", "channel", "instrumentMode"))
        for start in range(0, frames - self.total_samples + 1, self.hop_samples):
            self.references.append(ChunkReference(session.metadata.session_id, take.take_id,
                                                   take.input_path, take.output_path, start, start + latency,
                                                   self.total_samples, self.history_samples, controls))

    def __len__(self) -> int:
        return len(self.references)

    def __getitem__(self, index: int) -> PairedChunk:
        reference = self.references[index]
        source = WaveReader(reference.input_path).read(reference.start, reference.total_samples)
        target = WaveReader(reference.output_path).read(reference.output_start, reference.total_samples)
        if self.channel >= source.shape[1] or self.channel >= target.shape[1]:
            raise IndexError("Requested channel is unavailable")
        mask = np.zeros(reference.total_samples, dtype=np.bool_)
        mask[reference.history_samples:] = True
        return PairedChunk(source[:, self.channel].copy(), target[:, self.channel].copy(), mask, reference)

    def iter_shuffled(self, seed: int) -> Iterator[PairedChunk]:
        order = np.random.default_rng(seed).permutation(len(self.references))
        for index in order:
            yield self[int(index)]
