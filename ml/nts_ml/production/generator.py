"""Reproducible (audio, production label) pairs for training a production estimator.

**The labels are exact because they were chosen.** There is no annotation step and no
copyrighted audio: a dry render goes in, a known chain is applied, and the settings that were
applied are the label. This is the same self-supervision `nts_rig_recovery` and
`nts_production_recovery` rely on, and it is the only reason a production estimator is trainable
at all -- pairs of (dry guitar, produced record) do not exist in any quantity and cannot be
bought.

**The generator defines what the model can learn, which is the risk to keep in view.** If the
reverb in `chain` is nothing like a plate, a model trained here will estimate plate reverbs badly
and `nts_production_recovery` will not notice, because it applies the same chain. Cross-checking
against real material with `nts_song_probe` is not optional before a trained artifact ships.

**Dry renders are an input, not something this module makes.** The plan calls for rendering
through the engine's own amplifier at randomised rigs, and the amplifier is C++ --  reimplementing
it here to generate training data would be a third copy of the thing most likely to drift.
`nts_production_recovery --render` writes those renders; `render_pool_from_directory` reads them
back. What this module owns is the chain and the labels.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

import numpy as np
from numpy.typing import NDArray

from .chain import ProductionSettings, apply_chain


@dataclass(slots=True)
class ProductionRanges:
    """The span each field is sampled over.

    **Ranges that occur in real mixes, not the parameter space.** A model trained on uniformly
    sampled settings is confident about mixes nobody makes -- a guitar 24 dB down under a 20 dB
    tilt drowning in reverb is a valid point in the parameter space and is not a record. The
    defaults are the plan's own numbers.
    """

    level_offset_db: tuple[float, float] = (-18.0, 6.0)
    tilt_db: tuple[float, float] = (-8.0, 8.0)
    tilt_hz: tuple[float, float] = (600.0, 2400.0)
    #: Upper bound chosen so the wet mix stays under 35%: `chain` maps amount to `amount * 0.55`.
    reverb_amount: tuple[float, float] = (0.0, 0.63)
    delay_amount: tuple[float, float] = (0.0, 0.55)
    delay_ms: tuple[float, float] = (90.0, 480.0)
    #: Doubling is on or off in a real arrangement, not dialled -- a part is tracked twice or it
    #: is not. Sampled as a Bernoulli with a magnitude, so half the corpus has none at all.
    doubling_probability: float = 0.5
    doubling: tuple[float, float] = (0.25, 0.9)
    compression: tuple[float, float] = (0.0, 0.8)


@dataclass(slots=True)
class Example:
    """One training pair. `left`/`right` are the produced audio; `labels` is what made it."""

    left: NDArray[np.float32]
    right: NDArray[np.float32]
    settings: ProductionSettings
    #: Which render of the pool this was built from, so a split can keep a render on one side.
    render_index: int

    @property
    def labels(self) -> dict[str, float]:
        return self.settings.as_labels()


def sample_settings(random: np.random.Generator,
                    ranges: ProductionRanges | None = None) -> ProductionSettings:
    """One draw from `ranges`. Deterministic for a given generator state."""
    span = ranges or ProductionRanges()
    doubling = 0.0
    if random.random() < span.doubling_probability:
        doubling = float(random.uniform(*span.doubling))
    return ProductionSettings(
        level_offset_db=float(random.uniform(*span.level_offset_db)),
        tilt_db=float(random.uniform(*span.tilt_db)),
        tilt_hz=float(random.uniform(*span.tilt_hz)),
        reverb_amount=float(random.uniform(*span.reverb_amount)),
        delay_amount=float(random.uniform(*span.delay_amount)),
        delay_ms=float(random.uniform(*span.delay_ms)),
        doubling=doubling,
        compression=float(random.uniform(*span.compression)),
    )


def render_pool_from_directory(directory: Path) -> list[NDArray[np.float32]]:
    """Loads the dry renders `nts_production_recovery --render` wrote, in sorted name order.

    Sorted rather than in directory order so a dataset built from the same folder on two machines
    draws the same render for the same seed. A pool that silently reordered would make "seeded and
    reproducible" true on one machine and false across two, which is the least useful kind of
    reproducible.
    """
    files = sorted(Path(directory).glob("*.f32"))
    if not files:
        raise FileNotFoundError(f"No dry renders (*.f32) in {directory}")
    return [np.fromfile(path, dtype="<f4") for path in files]


def generate_dataset(renders: list[NDArray[np.float32]], count: int, seed: int,
                     sample_rate: float = 48000.0,
                     ranges: ProductionRanges | None = None) -> list[Example]:
    """`count` examples from `renders`, reproducible from `seed` alone.

    Renders are cycled rather than sampled at random, so every render appears the same number of
    times give or take one. Sampling them would let one phrase dominate a small corpus by luck,
    and the whole point of the four fixtures is that recovery measured on one phrase is not a
    measurement.
    """
    if not renders:
        raise ValueError("Generating a dataset needs at least one dry render")
    random = np.random.default_rng(seed)
    examples: list[Example] = []
    for index in range(count):
        render_index = index % len(renders)
        settings = sample_settings(random, ranges)
        left, right = apply_chain(renders[render_index], sample_rate, settings)
        examples.append(Example(left=left, right=right, settings=settings,
                                render_index=render_index))
    return examples
