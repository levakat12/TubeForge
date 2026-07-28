from __future__ import annotations

from dataclasses import dataclass

import numpy as np
from numpy.typing import NDArray


@dataclass(slots=True)
class AugmentationConfig:
    gain_db_min: float = 0.0
    gain_db_max: float = 0.0
    polarity_probability: float = 0.0
    input_noise_db: float | None = None
    maximum_timing_jitter: int = 0
    channel: int | None = None


class PairedAugmenter:
    def __init__(self, config: AugmentationConfig, seed: int):
        self.config = config
        self.random = np.random.default_rng(seed)

    def apply(self, source: NDArray[np.floating], target: NDArray[np.floating]
              ) -> tuple[NDArray[np.float32], NDArray[np.float32], int]:
        input_audio = np.asarray(source, dtype=np.float32).copy()
        output_audio = np.asarray(target, dtype=np.float32).copy()
        if input_audio.shape != output_audio.shape:
            raise ValueError("Paired augmentation requires matching shapes")
        if input_audio.ndim == 2 and self.config.channel is not None:
            if not 0 <= self.config.channel < input_audio.shape[1]:
                raise IndexError("Augmentation channel is unavailable")
            input_audio = input_audio[:, self.config.channel]
            output_audio = output_audio[:, self.config.channel]
        gain_db = self.random.uniform(self.config.gain_db_min, self.config.gain_db_max)
        gain = np.float32(10.0 ** (gain_db / 20.0))
        input_audio *= gain
        output_audio *= gain
        if self.random.random() < self.config.polarity_probability:
            input_audio *= -1.0
            output_audio *= -1.0
        if self.config.input_noise_db is not None:
            noise_rms = np.float32(10.0 ** (self.config.input_noise_db / 20.0))
            input_audio += self.random.normal(0.0, noise_rms, input_audio.shape).astype(np.float32)
        jitter = 0
        if self.config.maximum_timing_jitter > 0:
            jitter = int(self.random.integers(-self.config.maximum_timing_jitter,
                                              self.config.maximum_timing_jitter + 1))
            if jitter > 0:
                input_audio = input_audio[jitter:]
                output_audio = output_audio[jitter:]
            elif jitter < 0:
                input_audio = input_audio[:jitter]
                output_audio = output_audio[:jitter]
        return input_audio, output_audio, jitter
