from __future__ import annotations

from collections.abc import Callable, Sequence

import numpy as np
from numpy.typing import NDArray


def waveform_l1(prediction: NDArray[np.floating], target: NDArray[np.floating]) -> float:
    return float(np.mean(np.abs(np.asarray(prediction) - np.asarray(target))))


def waveform_l2(prediction: NDArray[np.floating], target: NDArray[np.floating]) -> float:
    error = np.asarray(prediction) - np.asarray(target)
    return float(np.mean(np.square(error)))


def pre_emphasized_loss(prediction: NDArray[np.floating], target: NDArray[np.floating],
                        coefficient: float = 0.95) -> float:
    def emphasize(values: NDArray[np.floating]) -> NDArray[np.floating]:
        array = np.asarray(values)
        return np.concatenate((array[:1], array[1:] - coefficient * array[:-1]))
    return waveform_l1(emphasize(prediction), emphasize(target))


def _stft_magnitude(audio: NDArray[np.floating], fft_size: int, hop: int) -> NDArray[np.float64]:
    values = np.asarray(audio, dtype=np.float64).reshape(-1)
    if values.size < fft_size:
        values = np.pad(values, (0, fft_size - values.size))
    frames = 1 + (values.size - fft_size) // hop
    window = np.hanning(fft_size)
    return np.stack([np.abs(np.fft.rfft(values[index * hop:index * hop + fft_size] * window))
                     for index in range(frames)])


def spectral_convergence(prediction: NDArray[np.floating], target: NDArray[np.floating],
                         fft_size: int = 1024, hop: int = 256) -> float:
    predicted = _stft_magnitude(prediction, fft_size, hop)
    expected = _stft_magnitude(target, fft_size, hop)
    return float(np.linalg.norm(expected - predicted) / max(np.linalg.norm(expected), 1.0e-12))


def multi_resolution_stft_loss(prediction: NDArray[np.floating], target: NDArray[np.floating],
                               resolutions: Sequence[tuple[int, int]] = ((256, 64), (512, 128), (1024, 256))) -> float:
    losses: list[float] = []
    for fft_size, hop in resolutions:
        predicted = _stft_magnitude(prediction, fft_size, hop)
        expected = _stft_magnitude(target, fft_size, hop)
        log_difference = np.mean(np.abs(np.log(predicted + 1.0e-7) - np.log(expected + 1.0e-7)))
        convergence = np.linalg.norm(expected - predicted) / max(np.linalg.norm(expected), 1.0e-12)
        losses.append(float(log_difference + convergence))
    return float(np.mean(losses))


def dc_penalty(prediction: NDArray[np.floating]) -> float:
    return float(abs(np.mean(np.asarray(prediction, dtype=np.float64))))


def loudness_difference(prediction: NDArray[np.floating], target: NDArray[np.floating]) -> float:
    def loudness(values: NDArray[np.floating]) -> float:
        rms = max(float(np.sqrt(np.mean(np.square(np.asarray(values, dtype=np.float64))))), 1.0e-12)
        return 20.0 * np.log10(rms)
    return float(abs(loudness(prediction) - loudness(target)))


def silence_stability_loss(prediction: NDArray[np.floating], target: NDArray[np.floating],
                           threshold: float = 1.0e-4) -> float:
    predicted, expected = np.asarray(prediction), np.asarray(target)
    silent = np.abs(expected) <= threshold
    return float(np.mean(np.square(predicted[silent]))) if np.any(silent) else 0.0


def transient_weighted_loss(prediction: NDArray[np.floating], target: NDArray[np.floating]) -> float:
    predicted, expected = np.asarray(prediction), np.asarray(target)
    if expected.size < 2:
        return waveform_l1(predicted, expected)
    derivative = np.abs(np.diff(expected, prepend=expected[0]))
    weights = 1.0 + 4.0 * derivative / max(float(np.mean(derivative)), 1.0e-7)
    return float(np.mean(weights * np.abs(predicted - expected)))


def perceptual_embedding_loss(prediction: NDArray[np.floating], target: NDArray[np.floating],
                              embedder: Callable[[NDArray[np.floating]], NDArray[np.floating]]) -> float:
    return waveform_l2(embedder(prediction), embedder(target))


def combined_loss(prediction: NDArray[np.floating], target: NDArray[np.floating],
                  weights: dict[str, float]) -> tuple[float, dict[str, float]]:
    components = {
        "time_l1": waveform_l1(prediction, target),
        "time_l2": waveform_l2(prediction, target),
        "pre_emphasis": pre_emphasized_loss(prediction, target),
        "multi_resolution_stft": multi_resolution_stft_loss(prediction, target),
        "spectral_convergence": spectral_convergence(prediction, target),
        "dc_penalty": dc_penalty(prediction),
        "loudness_difference": loudness_difference(prediction, target),
        "silence_stability": silence_stability_loss(prediction, target),
        "transient_weighted": transient_weighted_loss(prediction, target),
    }
    total = sum(weights.get(name, 0.0) * value for name, value in components.items())
    return float(total), components
