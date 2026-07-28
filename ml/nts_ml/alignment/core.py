from __future__ import annotations

import math
from typing import Literal

import numpy as np
from numpy.typing import NDArray

from ..schemas.session import AlignmentResult


def _mono(audio: NDArray[np.floating]) -> NDArray[np.float64]:
    values = np.asarray(audio, dtype=np.float64)
    if values.ndim == 2:
        values = np.mean(values, axis=1)
    return values.reshape(-1)


def _linear_correlation(reference: NDArray[np.float64], target: NDArray[np.float64],
                        generalized: bool) -> tuple[NDArray[np.float64], NDArray[np.int64]]:
    size = reference.size + target.size - 1
    fft_size = 1 << max(1, size - 1).bit_length()
    cross_spectrum = np.fft.rfft(target, fft_size) * np.conj(np.fft.rfft(reference, fft_size))
    if generalized:
        cross_spectrum /= np.maximum(np.abs(cross_spectrum), 1.0e-12)
    circular = np.fft.irfft(cross_spectrum, fft_size)
    correlation = np.concatenate((circular[-(reference.size - 1):], circular[:target.size]))
    lags = np.arange(-(reference.size - 1), target.size, dtype=np.int64)
    return correlation[:size], lags


def estimate_latency(reference: NDArray[np.floating], target: NDArray[np.floating],
                     max_lag: int | None = None,
                     method: Literal["cross_correlation", "gcc_phat"] = "gcc_phat") -> tuple[int, float, int]:
    source = _mono(reference)
    processed = _mono(target)
    count = min(source.size, processed.size, 262_144)
    if count < 32:
        raise ValueError("At least 32 samples are required for alignment")
    source = source[:count] - np.mean(source[:count])
    processed = processed[:count] - np.mean(processed[:count])
    correlation, lags = _linear_correlation(source, processed, method == "gcc_phat")
    if max_lag is not None:
        valid = np.abs(lags) <= max_lag
        correlation, lags = correlation[valid], lags[valid]
    peak_index = int(np.argmax(np.abs(correlation)))
    lag = int(lags[peak_index])
    polarity = 1 if correlation[peak_index] >= 0.0 else -1
    normalized = abs(float(np.dot(
        source[max(0, -lag): min(source.size, processed.size - lag)],
        processed[max(0, lag): min(processed.size, source.size + lag)],
    )))
    first = source[max(0, -lag): min(source.size, processed.size - lag)]
    second = processed[max(0, lag): min(processed.size, source.size + lag)]
    denominator = math.sqrt(float(np.dot(first, first) * np.dot(second, second))) + 1.0e-12
    return lag, min(1.0, normalized / denominator), polarity


def impulse_marker_latency(reference: NDArray[np.floating], target: NDArray[np.floating],
                           search_samples: int = 48_000) -> tuple[int, float, int]:
    source = _mono(reference)[:search_samples]
    processed = _mono(target)[:search_samples]
    source_peak = int(np.argmax(np.abs(source)))
    target_peak = int(np.argmax(np.abs(processed)))
    source_value, target_value = source[source_peak], processed[target_peak]
    confidence = min(abs(float(source_value)), abs(float(target_value)))
    return target_peak - source_peak, min(1.0, confidence), 1 if source_value * target_value >= 0 else -1


def align_pair(reference: NDArray[np.floating], target: NDArray[np.floating], latency: int,
               polarity: int = 1) -> tuple[NDArray[np.floating], NDArray[np.floating]]:
    source = np.asarray(reference)
    processed = np.asarray(target) * float(polarity)
    if latency >= 0:
        count = min(source.shape[0], processed.shape[0] - latency)
        return source[:max(0, count)], processed[latency:latency + max(0, count)]
    offset = -latency
    count = min(source.shape[0] - offset, processed.shape[0])
    return source[offset:offset + max(0, count)], processed[:max(0, count)]


def correct_clock_drift(audio: NDArray[np.floating], drift_ppm: float) -> NDArray[np.float32]:
    """Undo a constant sample-clock error using linear interpolation.

    Positive drift means the recorded stream accumulated extra samples relative to the reference.
    """
    values = np.asarray(audio, dtype=np.float32)
    if values.shape[0] < 2 or abs(drift_ppm) < 1.0e-12:
        return values.copy()
    corrected_length = max(1, int(round(values.shape[0] / (1.0 + drift_ppm * 1.0e-6))))
    positions = np.linspace(0.0, values.shape[0] - 1, corrected_length)
    base = np.arange(values.shape[0], dtype=np.float64)
    if values.ndim == 1:
        return np.interp(positions, base, values).astype(np.float32)
    return np.stack([np.interp(positions, base, values[:, channel])
                     for channel in range(values.shape[1])], axis=1).astype(np.float32)


def measure_alignment(reference: NDArray[np.floating], target: NDArray[np.floating],
                      sample_rate: int, window_seconds: float = 2.0,
                      max_lag: int | None = None) -> AlignmentResult:
    source, processed = _mono(reference), _mono(target)
    global_lag, confidence, polarity = estimate_latency(source, processed, max_lag, "gcc_phat")
    window = max(512, int(sample_rate * window_seconds))
    hop = window
    local_lags: list[int] = []
    positions: list[int] = []
    for start in range(0, min(source.size, processed.size) - window + 1, hop):
        local_lag, local_confidence, _ = estimate_latency(
            source[start:start + window], processed[start:start + window], max_lag, "cross_correlation")
        if local_confidence >= 0.35:
            local_lags.append(local_lag)
            positions.append(start)
    drift_ppm = 0.0
    if len(local_lags) >= 2:
        drift_ppm = float(np.polyfit(np.asarray(positions, dtype=np.float64),
                                     np.asarray(local_lags, dtype=np.float64), 1)[0] * 1.0e6)
    return AlignmentResult(global_lag, drift_ppm, confidence, polarity, "gcc_phat", local_lags)
