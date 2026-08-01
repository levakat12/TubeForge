from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

import numpy as np
from numpy.typing import NDArray


def _normalize_rows(values: NDArray[np.floating]) -> NDArray[np.float32]:
    array = np.asarray(values, dtype=np.float32)
    norm = np.linalg.norm(array, axis=1, keepdims=True)
    return (array / np.maximum(norm, 1.0e-8)).astype(np.float32)


def multi_resolution_representation(audio: NDArray[np.floating], sample_rate: int,
                                    dimensions: int = 64) -> NDArray[np.float32]:
    """Pitch/loudness-reduced log-spectral representation for offline tone encoding."""
    samples = np.asarray(audio, dtype=np.float32).reshape(-1)
    if sample_rate < 8_000 or samples.size < sample_rate // 4:
        raise ValueError("tone representation needs at least 250 ms at >= 8 kHz")
    samples = samples - np.mean(samples)
    rms = float(np.sqrt(np.mean(np.square(samples))))
    if rms < 1.0e-5:
        raise ValueError("silence cannot produce a tone representation")
    samples = np.clip(samples * (0.125 / rms), -4.0, 4.0)
    summaries: list[NDArray[np.float32]] = []
    bands_per_resolution = dimensions // 4
    for fft_size in (256, 1024, 4096):
        fft_size = min(fft_size, 2 ** int(np.floor(np.log2(samples.size))))
        hop = max(1, fft_size // 4)
        starts = np.arange(0, max(1, samples.size - fft_size + 1), hop)
        if starts.size > 192:
            starts = starts[np.linspace(0, starts.size - 1, 192).astype(np.int64)]
        window = np.hanning(fft_size).astype(np.float32)
        spectra = []
        for start in starts:
            frame = np.zeros(fft_size, dtype=np.float32)
            count = min(fft_size, samples.size - int(start))
            frame[:count] = samples[int(start):int(start) + count]
            spectra.append(np.log1p(np.abs(np.fft.rfft(frame * window))).astype(np.float32))
        mean_spectrum = np.mean(spectra, axis=0)
        frequencies = np.linspace(0.0, sample_rate / 2.0, mean_spectrum.size)
        edges = np.geomspace(25.0, min(18_000.0, sample_rate / 2.0), bands_per_resolution + 1)
        band_values: list[float] = []
        for index in range(bands_per_resolution):
            mask = (frequencies >= edges[index]) & (frequencies < edges[index + 1])
            if np.any(mask):
                band_values.append(float(np.mean(mean_spectrum[mask])))
            else:
                nearest = int(np.argmin(np.abs(frequencies - np.sqrt(edges[index] * edges[index + 1]))))
                band_values.append(float(mean_spectrum[nearest]))
        bands = np.asarray(band_values, dtype=np.float32)
        bands -= np.mean(bands)
        bands /= np.maximum(np.std(bands), 1.0e-5)
        summaries.append(bands)
    frame_size = max(32, sample_rate // 50)
    frames = samples[:samples.size // frame_size * frame_size].reshape(-1, frame_size)
    frame_rms = np.sqrt(np.mean(np.square(frames), axis=1) + 1.0e-12)
    dynamics = np.array([
        np.max(np.abs(samples)) / np.maximum(np.sqrt(np.mean(np.square(samples))), 1.0e-8),
        np.std(20.0 * np.log10(frame_rms + 1.0e-8)),
        np.mean(np.maximum(0.0, np.diff(frame_rms))),
        np.mean(frame_rms) / np.maximum(np.max(frame_rms), 1.0e-8),
    ], dtype=np.float32)
    dynamics = np.tile(dynamics, bands_per_resolution // dynamics.size)
    representation = np.concatenate((*summaries, dynamics))[:dimensions]
    return representation.astype(np.float32)


class LinearToneEncoder:
    """Learned normalized projection trained from rig-positive/performance-negative groups."""

    def __init__(self, input_dimensions: int = 64, embedding_dimensions: int = 128,
                 version: str = "tone-encoder-untrained-v1", seed: int = 7):
        if input_dimensions < 4 or embedding_dimensions < 8:
            raise ValueError("tone encoder dimensions are too small")
        self.input_dimensions = input_dimensions
        self.embedding_dimensions = embedding_dimensions
        self.version = version
        random = np.random.default_rng(seed)
        self.projection = random.normal(0.0, 1.0 / np.sqrt(input_dimensions),
                                        (embedding_dimensions, input_dimensions)).astype(np.float32)
        self.domain_mean = np.zeros(input_dimensions, dtype=np.float32)
        self.domain_scale = np.ones(input_dimensions, dtype=np.float32)

    def encode(self, features: NDArray[np.floating]) -> NDArray[np.float32]:
        values = np.asarray(features, dtype=np.float32)
        one_dimensional = values.ndim == 1
        values = values.reshape(-1, self.input_dimensions)
        normalized = (values - self.domain_mean) / np.maximum(self.domain_scale, 1.0e-5)
        embeddings = _normalize_rows(normalized @ self.projection.T)
        return embeddings[0] if one_dimensional else embeddings

    def fit_contrastive(self, features: NDArray[np.floating], rig_labels: NDArray[np.integer],
                        version: str = "tone-contrastive-v1") -> None:
        values = np.asarray(features, dtype=np.float64)
        labels = np.asarray(rig_labels).reshape(-1)
        if values.ndim != 2 or values.shape[1] != self.input_dimensions or labels.size != values.shape[0]:
            raise ValueError("features/rig labels have incompatible dimensions")
        unique = np.unique(labels)
        if unique.size < 2 or any(np.sum(labels == label) < 2 for label in unique):
            raise ValueError("contrastive training needs at least two rigs and two examples per rig")
        self.domain_mean = np.mean(values, axis=0).astype(np.float32)
        self.domain_scale = np.maximum(np.std(values, axis=0), 1.0e-4).astype(np.float32)
        normalized = (values - self.domain_mean) / self.domain_scale
        global_mean = np.mean(normalized, axis=0)
        within = np.eye(self.input_dimensions, dtype=np.float64) * 1.0e-3
        between = np.zeros_like(within)
        for label in unique:
            group = normalized[labels == label]
            group_mean = np.mean(group, axis=0)
            centered = group - group_mean
            within += centered.T @ centered / max(1, group.shape[0] - 1)
            difference = group_mean - global_mean
            between += group.shape[0] * np.outer(difference, difference)
        within /= unique.size
        eigenvalues, eigenvectors = np.linalg.eigh(within)
        whitening = eigenvectors @ np.diag(1.0 / np.sqrt(np.maximum(eigenvalues, 1.0e-5))) @ eigenvectors.T
        objective = whitening @ between @ whitening.T
        values_between, vectors_between = np.linalg.eigh(objective)
        ordered = vectors_between[:, np.argsort(values_between)[::-1]].T @ whitening
        rows = [ordered[index % ordered.shape[0]] for index in range(self.embedding_dimensions)]
        projection = np.stack(rows)
        projection /= np.maximum(np.linalg.norm(projection, axis=1, keepdims=True), 1.0e-8)
        self.projection = projection.astype(np.float32)
        self.version = version

    def domain_distance(self, features: NDArray[np.floating]) -> NDArray[np.float32]:
        values = np.asarray(features, dtype=np.float32).reshape(-1, self.input_dimensions)
        z = (values - self.domain_mean) / np.maximum(self.domain_scale, 1.0e-5)
        return np.sqrt(np.mean(np.square(z), axis=1)).astype(np.float32)

    def save(self, path: Path) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        np.savez(path, version=self.version, input_dimensions=self.input_dimensions,
                 embedding_dimensions=self.embedding_dimensions, projection=self.projection,
                 domain_mean=self.domain_mean, domain_scale=self.domain_scale)

    @classmethod
    def load(cls, path: Path) -> LinearToneEncoder:
        data = np.load(path)
        model = cls(int(data["input_dimensions"]), int(data["embedding_dimensions"]), str(data["version"]))
        model.projection[...] = data["projection"]
        model.domain_mean[...] = data["domain_mean"]
        model.domain_scale[...] = data["domain_scale"]
        return model


@dataclass(slots=True)
class DisentangledToneEncoder:
    tone: LinearToneEncoder
    performance: LinearToneEncoder
    production: LinearToneEncoder

    @classmethod
    def create(cls, input_dimensions: int = 64, embedding_dimensions: int = 128) -> DisentangledToneEncoder:
        return cls(LinearToneEncoder(input_dimensions, embedding_dimensions, seed=7),
                   LinearToneEncoder(input_dimensions, embedding_dimensions, seed=17),
                   LinearToneEncoder(input_dimensions, embedding_dimensions, seed=29))

    def fit(self, features: NDArray[np.floating], rig_labels: NDArray[np.integer],
            performance_labels: NDArray[np.integer], production_labels: NDArray[np.integer]) -> None:
        self.tone.fit_contrastive(features, rig_labels, "tone-disentangled-v1")
        self.performance.fit_contrastive(features, performance_labels, "performance-disentangled-v1")
        self.production.fit_contrastive(features, production_labels, "production-disentangled-v1")


class InterpretableToneHeads:
    """Ridge heads for normalized descriptors and binary instrument/technique targets."""

    def __init__(self, target_names: tuple[str, ...]):
        if not target_names:
            raise ValueError("at least one interpretable target is required")
        self.target_names = target_names
        self.weights: NDArray[np.float32] | None = None
        self.residual_scale = np.ones(len(target_names), dtype=np.float32)

    def fit(self, embeddings: NDArray[np.floating], targets: dict[str, NDArray[np.floating]],
            regularization: float = 1.0e-3) -> None:
        values = np.asarray(embeddings, dtype=np.float64)
        if values.ndim != 2 or any(name not in targets for name in self.target_names):
            raise ValueError("missing target or invalid embedding matrix")
        expected = np.column_stack([np.asarray(targets[name], dtype=np.float64).reshape(-1)
                                    for name in self.target_names])
        if expected.shape[0] != values.shape[0] or not np.all(np.isfinite(expected)):
            raise ValueError("interpretable targets have incompatible dimensions")
        design = np.column_stack((values, np.ones(values.shape[0])))
        identity = np.eye(design.shape[1]); identity[-1, -1] = 0.0
        self.weights = np.linalg.solve(design.T @ design + regularization * identity,
                                       design.T @ expected).T.astype(np.float32)
        prediction = design @ self.weights.T
        self.residual_scale = np.maximum(np.sqrt(np.mean(np.square(prediction - expected), axis=0)),
                                         0.02).astype(np.float32)

    def predict(self, embeddings: NDArray[np.floating]
                ) -> tuple[dict[str, NDArray[np.float32]], dict[str, NDArray[np.float32]]]:
        if self.weights is None:
            raise RuntimeError("interpretable heads have not been fitted")
        values = np.asarray(embeddings, dtype=np.float32).reshape(-1, self.weights.shape[1] - 1)
        design = np.column_stack((values, np.ones(values.shape[0], dtype=np.float32)))
        prediction = np.clip(design @ self.weights.T, 0.0, 1.0).astype(np.float32)
        uncertainty = np.broadcast_to(np.clip(self.residual_scale, 0.02, 1.0), prediction.shape).copy()
        return ({name: prediction[:, index] for index, name in enumerate(self.target_names)},
                {name: uncertainty[:, index] for index, name in enumerate(self.target_names)})
