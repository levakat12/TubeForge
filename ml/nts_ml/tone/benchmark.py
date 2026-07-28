from __future__ import annotations

from dataclasses import dataclass

import numpy as np
from numpy.typing import NDArray

from .encoder import LinearToneEncoder

CONTROLLED_RIG_FAMILIES = (
    "clean", "edge-of-breakup", "crunch", "high-gain", "fuzz",
    "clean-bass", "compressed-bass", "driven-bass", "split-path-bass",
)


@dataclass(slots=True)
class ToneBenchmarkResult:
    same_rig_retrieval_accuracy: float
    family_retrieval_accuracy: float
    pitch_robustness: float
    loudness_robustness: float
    short_clip_robustness: float
    leakage_robustness: float
    out_of_distribution_rejection: float
    instrument_classification_accuracy: float
    calibration_error: float
    spectral_ablation_delta: float


def _nearest(embeddings: NDArray[np.float32]) -> NDArray[np.int64]:
    similarity = embeddings @ embeddings.T
    np.fill_diagonal(similarity, -np.inf)
    return np.argmax(similarity, axis=1)


def evaluate_tone_encoder(model: LinearToneEncoder, features: NDArray[np.floating],
                          rig_labels: NDArray[np.integer], family_labels: NDArray[np.integer],
                          pitch_variants: NDArray[np.floating], loudness_variants: NDArray[np.floating],
                          short_variants: NDArray[np.floating], leakage_variants: NDArray[np.floating],
                          out_of_distribution: NDArray[np.floating],
                          instrument_labels: NDArray[np.integer] | None = None,
                          instrument_probabilities: NDArray[np.floating] | None = None) -> ToneBenchmarkResult:
    values = np.asarray(features, dtype=np.float32)
    rig = np.asarray(rig_labels).reshape(-1); family = np.asarray(family_labels).reshape(-1)
    embeddings = model.encode(values)
    nearest = _nearest(embeddings)
    same_rig = float(np.mean(rig[nearest] == rig))
    same_family = float(np.mean(family[nearest] == family))

    def robustness(variants: NDArray[np.floating]) -> float:
        variant_embeddings = model.encode(variants)
        return float(np.mean(np.sum(embeddings * variant_embeddings, axis=1)))

    training_distance = model.domain_distance(values)
    ood_distance = model.domain_distance(out_of_distribution)
    threshold = float(np.quantile(training_distance, 0.95) + 0.5)
    ood_rejection = float(np.mean(ood_distance > threshold))
    if instrument_labels is None or instrument_probabilities is None:
        instrument_accuracy, calibration_error = float("nan"), float("nan")
    else:
        labels = np.asarray(instrument_labels).reshape(-1)
        probabilities = np.clip(np.asarray(instrument_probabilities).reshape(-1), 0.0, 1.0)
        if labels.size != values.shape[0] or probabilities.size != values.shape[0]:
            raise ValueError("instrument labels/probabilities have incompatible dimensions")
        instrument_accuracy = float(np.mean((probabilities >= 0.5) == labels))
        calibration_error = 0.0
        for low in np.linspace(0.0, 0.9, 10):
            selected = (probabilities >= low) & (probabilities < low + 0.1)
            if np.any(selected):
                calibration_error += float(np.mean(selected)) * abs(
                    float(np.mean(probabilities[selected])) - float(np.mean(labels[selected])))
    ablated = values.copy(); ablated[:, :16] = 0.0
    ablated_nearest = _nearest(model.encode(ablated))
    ablated_accuracy = float(np.mean(rig[ablated_nearest] == rig))
    return ToneBenchmarkResult(same_rig, same_family, robustness(pitch_variants),
                               robustness(loudness_variants), robustness(short_variants),
                               robustness(leakage_variants), ood_rejection,
                               instrument_accuracy, calibration_error,
                               same_rig - ablated_accuracy)
