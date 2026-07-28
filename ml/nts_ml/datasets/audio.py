from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import wave

import numpy as np
from numpy.typing import NDArray

FloatAudio = NDArray[np.float32]


@dataclass(frozen=True, slots=True)
class WaveInfo:
    sample_rate: int
    channels: int
    sample_width: int
    frames: int

    @property
    def bit_depth(self) -> int:
        return self.sample_width * 8

    @property
    def duration_seconds(self) -> float:
        return self.frames / self.sample_rate


class WaveReader:
    """Random-access PCM WAV reader that decodes only the requested frames."""

    def __init__(self, path: Path):
        self.path = Path(path)
        with wave.open(str(self.path), "rb") as source:
            if source.getcomptype() != "NONE":
                raise ValueError(f"Compressed WAV is not supported: {self.path}")
            self.info = WaveInfo(source.getframerate(), source.getnchannels(),
                                 source.getsampwidth(), source.getnframes())
        if self.info.sample_width not in (2, 3, 4):
            raise ValueError(f"Unsupported PCM width: {self.info.sample_width * 8} bits")

    def read(self, start: int = 0, frames: int | None = None) -> FloatAudio:
        start = max(0, min(start, self.info.frames))
        count = self.info.frames - start if frames is None else max(0, min(frames, self.info.frames - start))
        with wave.open(str(self.path), "rb") as source:
            source.setpos(start)
            raw = source.readframes(count)
        decoded = decode_pcm(raw, self.info.sample_width)
        return decoded.reshape(-1, self.info.channels)


def decode_pcm(raw: bytes, sample_width: int) -> FloatAudio:
    if sample_width == 2:
        return (np.frombuffer(raw, dtype="<i2").astype(np.float32) / 32768.0).astype(np.float32)
    if sample_width == 4:
        return (np.frombuffer(raw, dtype="<i4").astype(np.float32) / 2147483648.0).astype(np.float32)
    if sample_width == 3:
        bytes24 = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3)
        values = (bytes24[:, 0].astype(np.int32)
                  | (bytes24[:, 1].astype(np.int32) << 8)
                  | (bytes24[:, 2].astype(np.int32) << 16))
        values = np.where(values & 0x800000, values - 0x1000000, values)
        return (values.astype(np.float32) / 8388608.0).astype(np.float32)
    raise ValueError("Only 16-, 24-, and 32-bit PCM are supported")


def write_pcm_wave(path: Path, audio: NDArray[np.floating], sample_rate: int,
                   bit_depth: int = 24) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    array = np.asarray(audio, dtype=np.float32)
    if array.ndim == 1:
        array = array[:, None]
    clipped = np.clip(array, -1.0, 1.0 - np.finfo(np.float32).eps)
    if bit_depth == 16:
        raw = np.round(clipped * 32767.0).astype("<i2").tobytes()
        width = 2
    elif bit_depth == 24:
        values = np.round(clipped * 8388607.0).astype(np.int32).reshape(-1)
        packed = np.empty((values.size, 3), dtype=np.uint8)
        packed[:, 0] = values & 0xff
        packed[:, 1] = (values >> 8) & 0xff
        packed[:, 2] = (values >> 16) & 0xff
        raw = packed.tobytes()
        width = 3
    elif bit_depth == 32:
        raw = np.round(clipped * 2147483647.0).astype("<i4").tobytes()
        width = 4
    else:
        raise ValueError("bit_depth must be 16, 24, or 32")
    with wave.open(str(path), "wb") as destination:
        destination.setnchannels(array.shape[1])
        destination.setsampwidth(width)
        destination.setframerate(sample_rate)
        destination.writeframes(raw)
