"""A small dense regressor from analysis features to production amounts.

**Features, not audio.** The inputs are the numbers the reconstruction pipeline already computes --
`ToneFeatures::spatial`, `ToneFeatures::dynamic`, and the 41-band spectral residual -- rather than
samples. That is the whole reason this can be small enough to be uninteresting: a model that reads
waveforms would need a receptive field, a training budget and a runtime none of this needs, to
recover four numbers that are already implied by measurements sitting in memory. It also keeps the
thing interpretable when it is wrong, because every input has a name.

**Why a model at all, when the deterministic path exists.** `nts_production_recovery` scores that
path at rank agreement 0.59 for room against a 0.5 chance baseline -- `roomReverbEstimate` barely
tracks how much room was applied. Delay and doubling rank near 1 and read badly wrong magnitudes.
Those are two different failures and both are calibration problems, which is exactly what a
regression fits and exactly what a hand-written constant cannot without a measurement nobody has.

**Numpy, not torch.** `pyproject.toml` keeps this package numpy-only on purpose so CI does not grow
a torch dependency, and a 57-32-4 network trained on a few thousand examples does not need one. The
optimiser below is Adam in about twenty lines; if this ever needs more than that, it has outgrown
the argument for using it at all.

Outputs are **normalised production amounts in 0..1**, the same scale the deterministic path
produces, so the two are drop-in replacements for each other and `applyProductionProfile` does not
need to know which one ran. See `ProductionTargets` for what each output means.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np
from numpy.typing import NDArray

#: Names of the inputs, in the order the C++ side writes them. The list is the contract between
#: `nts_production_recovery --features` and this model, and its length is baked into the artifact.
FEATURE_NAMES: tuple[str, ...] = (
    "stereoWidth", "roomReverbEstimate", "doubleTrackingLikelihood", "panning", "modulation",
    "delayPresence", "channelCorrelation",
    "crestFactorDb", "transientPreservation", "compressionAmount", "attackMilliseconds",
    "releaseMilliseconds", "sustain", "gainReductionEstimateDb", "loudnessRangeDb",
    "integratedLoudnessDb",
) + tuple(f"residualDb{band}" for band in range(41))

#: What the model predicts, in order. Each is 0..1 on the same scale the deterministic estimator
#: uses, so `ProductionProfile` carries one or the other without anything downstream branching.
TARGET_NAMES: tuple[str, ...] = ("roomAmount", "delayAmount", "doubling", "compression")


@dataclass(slots=True)
class ProductionRegressor:
    """One hidden layer, tanh, sigmoid output. Standardisation lives inside the model.

    The input mean and scale are **part of the packed weights** rather than a separate
    normalisation file, so a runtime cannot load the weights and forget the standardisation. That
    failure would not crash: it would produce confident nonsense, which is the kind of bug this
    format's hashing and test vectors exist to make impossible.
    """

    input_mean: NDArray[np.float32]
    input_scale: NDArray[np.float32]
    hidden_weight: NDArray[np.float32]
    hidden_bias: NDArray[np.float32]
    output_weight: NDArray[np.float32]
    output_bias: NDArray[np.float32]

    architecture: str = "production_regressor"
    #: Carried only because `ModelManifest` requires one. Nothing here is sample-rate dependent:
    #: the features arrive already measured.
    sample_rate: int = 48_000

    @property
    def input_count(self) -> int:
        return int(self.input_mean.size)

    @property
    def hidden_count(self) -> int:
        return int(self.hidden_bias.size)

    @property
    def output_count(self) -> int:
        return int(self.output_bias.size)

    @classmethod
    def initialise(cls, inputs: int, hidden: int, outputs: int, seed: int) -> ProductionRegressor:
        random = np.random.default_rng(seed)
        # Xavier, so a tanh layer this wide starts in the part of its range that has a gradient.
        first = random.normal(0.0, np.sqrt(2.0 / (inputs + hidden)), (inputs, hidden))
        second = random.normal(0.0, np.sqrt(2.0 / (hidden + outputs)), (hidden, outputs))
        return cls(
            input_mean=np.zeros(inputs, dtype=np.float32),
            input_scale=np.ones(inputs, dtype=np.float32),
            hidden_weight=first.astype(np.float32),
            hidden_bias=np.zeros(hidden, dtype=np.float32),
            output_weight=second.astype(np.float32),
            output_bias=np.zeros(outputs, dtype=np.float32),
        )

    def predict(self, features: NDArray[np.floating]) -> NDArray[np.float32]:
        """Rows in, rows out. Accepts one example or a batch."""
        matrix = np.atleast_2d(np.asarray(features, dtype=np.float32))
        if matrix.shape[1] != self.input_count:
            raise ValueError(f"Expected {self.input_count} features, got {matrix.shape[1]}")
        standardised = (matrix - self.input_mean) / self.input_scale
        hidden = np.tanh(standardised @ self.hidden_weight + self.hidden_bias)
        logits = hidden @ self.output_weight + self.output_bias
        return (1.0 / (1.0 + np.exp(-logits))).astype(np.float32)

    def parameters(self) -> dict[str, NDArray[np.float32]]:
        """The packed payload, in the order the C++ loader reads it."""
        return {
            "input_mean": self.input_mean,
            "input_scale": self.input_scale,
            "hidden_weight": self.hidden_weight,
            "hidden_bias": self.hidden_bias,
            "output_weight": self.output_weight,
            "output_bias": self.output_bias,
        }


#: Which feature column the deterministic estimator copies into each target, in `TARGET_NAMES`
#: order. `estimateProductionProfile` sets `roomAmount` from `roomReverbEstimate`, `delayAmount`
#: from `delayPresence`, `doubling` from `doubleTrackingLikelihood` and `compression` from
#: `compressionAmount`, each clamped to 0..1 and otherwise passed straight through.
#:
#: Named here so the baseline can be computed from the same feature matrix the model is trained
#: on. That is what makes "the model beats the deterministic path" a comparison rather than an
#: assertion: both are scored on the same held-out rows, from the same measurements, against the
#: same labels -- rather than one from a harness run and the other from a training log.
DETERMINISTIC_COLUMNS: dict[str, int] = {
    "roomAmount": 1,
    "delayAmount": 5,
    "doubling": 2,
    "compression": 9,
}


def deterministic_baseline(features: NDArray[np.floating]) -> NDArray[np.float32]:
    """What `estimateProductionProfile` would report for these features, without a model."""
    matrix = np.atleast_2d(np.asarray(features, dtype=np.float32))
    columns = [DETERMINISTIC_COLUMNS[name] for name in TARGET_NAMES]
    return np.clip(matrix[:, columns], 0.0, 1.0).astype(np.float32)


def group_split(groups: NDArray[np.integer], validation_fraction: float = 0.2) -> NDArray[np.bool_]:
    """A boolean mask holding out whole groups, so a caller can score anything on the same split.

    Exposed rather than kept inside `fit` because the deterministic baseline has to be measured on
    exactly the rows the model was not trained on. Recomputing the split at the call site would be
    a second definition of it, and the two agreeing would be a coincidence maintained by hand.
    """
    identifiers = np.asarray(groups)
    stride = max(2, int(round(1.0 / max(validation_fraction, 1.0e-3))))
    distinct = np.unique(identifiers)
    if distinct.size < stride:
        raise ValueError("Too few distinct groups to hold any of them out")
    return np.isin(identifiers, distinct[::stride])


@dataclass(slots=True)
class TrainingReport:
    epochs: int
    training_loss: float
    validation_loss: float
    #: Mean absolute error per target on the held-out split, in the 0..1 output units.
    validation_error: dict[str, float]
    #: How many examples each side got, so a suspiciously good number can be checked against the
    #: size of the split that produced it.
    training_examples: int = 0
    validation_examples: int = 0
    #: True when the split held out whole renders rather than scattered rows. A false here means
    #: the validation number is optimistic and should be reported as such.
    grouped_split: bool = False


def fit(features: NDArray[np.floating], targets: NDArray[np.floating], *, seed: int,
        groups: NDArray[np.integer] | None = None,
        hidden: int = 32, epochs: int = 400, batch: int = 64, learning_rate: float = 3.0e-3,
        weight_decay: float = 1.0e-4, validation_fraction: float = 0.2
        ) -> tuple[ProductionRegressor, TrainingReport]:
    """Adam on mean squared error, with a held-out split reported rather than tuned against.

    **Split by `groups`, which is the render each example was built from.** Every example sharing a
    render shares an amplifier, a voicing, a drive setting and a phrase, and differs only in the
    mix applied on top. Splitting by row instead puts the same render on both sides, and the
    validation number then measures how well the model recognises an amplifier it has already seen
    rather than how well it reads a mix -- which is the question being asked. An earlier version of
    this function claimed a stride split achieved that; it did not, because the corpus cycles
    renders and a coprime stride visits every one of them.

    Falls back to a row stride when `groups` is absent, and says so by producing a report the
    caller can still read -- but a caller with group information and no reason to withhold it
    should pass it.
    """
    inputs = np.asarray(features, dtype=np.float32)
    labels = np.asarray(targets, dtype=np.float32)
    if inputs.ndim != 2 or labels.ndim != 2 or inputs.shape[0] != labels.shape[0]:
        raise ValueError("Features and targets must be matching 2-D arrays")
    if inputs.shape[0] < 16:
        raise ValueError("Too few examples to fit and validate anything meaningful")

    if groups is None:
        stride = max(2, int(round(1.0 / max(validation_fraction, 1.0e-3))))
        held = np.zeros(inputs.shape[0], dtype=bool)
        held[::stride] = True
    else:
        if np.asarray(groups).shape[0] != inputs.shape[0]:
            raise ValueError("One group per example is required")
        held = group_split(groups, validation_fraction)
    if not held.any() or held.all():
        raise ValueError("The validation split is empty or total")
    train_x, train_y = inputs[~held], labels[~held]
    valid_x, valid_y = inputs[held], labels[held]

    model = ProductionRegressor.initialise(inputs.shape[1], hidden, labels.shape[1], seed)
    # Standardisation from the training split only. Fitting it over everything would leak the
    # held-out rows' distribution into the model, which is a small leak that makes the one number
    # this function reports slightly dishonest.
    model.input_mean = train_x.mean(axis=0).astype(np.float32)
    scale = train_x.std(axis=0)
    # A constant feature has zero spread and would divide to infinity. One is the neutral divisor,
    # and the feature then contributes only through its bias, which is correct: it carries nothing.
    model.input_scale = np.where(scale < 1.0e-6, 1.0, scale).astype(np.float32)

    weights = [model.hidden_weight, model.hidden_bias, model.output_weight, model.output_bias]
    moments = [np.zeros_like(weight) for weight in weights]
    velocities = [np.zeros_like(weight) for weight in weights]
    random = np.random.default_rng(seed + 1)
    step = 0
    training_loss = 0.0

    for _ in range(epochs):
        order = random.permutation(train_x.shape[0])
        for start in range(0, order.size, batch):
            rows = order[start:start + batch]
            batch_x = (train_x[rows] - model.input_mean) / model.input_scale
            batch_y = train_y[rows]
            count = float(batch_x.shape[0])

            pre_hidden = batch_x @ model.hidden_weight + model.hidden_bias
            hidden = np.tanh(pre_hidden)
            logits = hidden @ model.output_weight + model.output_bias
            predicted = 1.0 / (1.0 + np.exp(-logits))

            error = predicted - batch_y
            training_loss = float(np.mean(np.square(error)))
            # d/dlogits of MSE through a sigmoid.
            delta_out = (2.0 / count) * error * predicted * (1.0 - predicted)
            gradients = [
                batch_x.T @ ((delta_out @ model.output_weight.T) * (1.0 - np.square(hidden))),
                np.sum((delta_out @ model.output_weight.T) * (1.0 - np.square(hidden)), axis=0),
                hidden.T @ delta_out,
                np.sum(delta_out, axis=0),
            ]
            gradients[0] += weight_decay * model.hidden_weight
            gradients[2] += weight_decay * model.output_weight

            step += 1
            for index, (weight, gradient) in enumerate(zip(weights, gradients, strict=True)):
                moments[index] = 0.9 * moments[index] + 0.1 * gradient
                velocities[index] = 0.999 * velocities[index] + 0.001 * np.square(gradient)
                corrected_moment = moments[index] / (1.0 - 0.9 ** step)
                corrected_velocity = velocities[index] / (1.0 - 0.999 ** step)
                weight -= (learning_rate * corrected_moment
                           / (np.sqrt(corrected_velocity) + 1.0e-8)).astype(np.float32)

    predicted = model.predict(valid_x)
    validation_loss = float(np.mean(np.square(predicted - valid_y)))
    per_target = np.mean(np.abs(predicted - valid_y), axis=0)
    names = TARGET_NAMES[:labels.shape[1]]
    report = TrainingReport(
        epochs=epochs,
        training_loss=training_loss,
        validation_loss=validation_loss,
        validation_error={name: float(value) for name, value in zip(names, per_target, strict=True)},
        training_examples=int(train_x.shape[0]),
        validation_examples=int(valid_x.shape[0]),
        grouped_split=groups is not None,
    )
    return model, report
