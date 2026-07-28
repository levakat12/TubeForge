from __future__ import annotations

from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable
import time

import numpy as np
from numpy.typing import NDArray

from ..datasets.streaming import PairedChunk
from ..losses import combined_loss
from ..models import (CausalTcn, ConditionedGru, ConditionedLstm, FeatureCache, LstmCache,
                      RnnCache, TinyTanhRnn)
from .config import ExperimentConfig


@dataclass(slots=True)
class TrainingResult:
    best_checkpoint: Path
    best_validation_loss: float
    duration_seconds: float
    epoch_losses: list[float]


class AdamW:
    def __init__(self, parameters: dict[str, NDArray[np.float32]], learning_rate: float,
                 weight_decay: float):
        self.parameters = parameters
        self.learning_rate = learning_rate
        self.weight_decay = weight_decay
        self.first = {name: np.zeros_like(value) for name, value in parameters.items()}
        self.second = {name: np.zeros_like(value) for name, value in parameters.items()}
        self.step_count = 0

    def step(self, gradients: dict[str, NDArray[np.float32]]) -> None:
        self.step_count += 1
        for name, parameter in self.parameters.items():
            gradient = gradients[name]
            self.first[name] = 0.9 * self.first[name] + 0.1 * gradient
            self.second[name] = 0.999 * self.second[name] + 0.001 * np.square(gradient)
            first_hat = self.first[name] / (1.0 - 0.9 ** self.step_count)
            second_hat = self.second[name] / (1.0 - 0.999 ** self.step_count)
            parameter -= self.learning_rate * (first_hat / (np.sqrt(second_hat) + 1.0e-8)
                                                + self.weight_decay * parameter)


def _tanh_gradients(model: TinyTanhRnn, cache: RnnCache, target: NDArray[np.floating],
               history_samples: int) -> tuple[float, dict[str, NDArray[np.float32]]]:
    expected = np.asarray(target, dtype=np.float32).reshape(-1)
    active = max(1, expected.size - history_samples)
    errors = cache.outputs - expected
    errors[:history_samples] = 0.0
    loss = float(np.sum(np.square(errors)) / active)
    gradients = {name: np.zeros_like(value) for name, value in model.parameters().items()}
    next_state_gradient = np.zeros(model.state_size, dtype=np.float32)
    for index in range(expected.size - 1, -1, -1):
        output_gradient = np.float32(2.0 * errors[index] / active)
        gradients["output_weight"] += output_gradient * cache.states[index + 1][None, :]
        gradients["output_bias"][0] += output_gradient
        state_gradient = model.output_weight[0] * output_gradient + next_state_gradient
        activation_gradient = state_gradient * (1.0 - np.square(cache.states[index + 1]))
        gradients["input_weight"][:, 0] += activation_gradient * cache.inputs[index]
        gradients["recurrent_weight"] += np.outer(activation_gradient, cache.states[index])
        gradients["bias"] += activation_gradient
        next_state_gradient = model.recurrent_weight.T @ activation_gradient
    for gradient in gradients.values():
        np.clip(gradient, -5.0, 5.0, out=gradient)
    return loss, gradients


def _lstm_gradients(model: ConditionedLstm, cache: LstmCache, target: NDArray[np.floating],
                    history_samples: int) -> tuple[float, dict[str, NDArray[np.float32]]]:
    expected = np.asarray(target, dtype=np.float32).reshape(-1)
    active = max(1, expected.size - history_samples)
    errors = cache.outputs - expected; errors[:history_samples] = 0.0
    loss = float(np.sum(np.square(errors)) / active)
    gradients = {name: np.zeros_like(value) for name, value in model.parameters().items()}
    next_hidden = np.zeros(model.state_size, dtype=np.float32)
    next_cell = np.zeros(model.state_size, dtype=np.float32)
    for index in range(expected.size - 1, -1, -1):
        output_gradient = np.float32(2.0 * errors[index] / active)
        gradients["output_weight"] += output_gradient * cache.hidden[index + 1]
        gradients["output_bias"][0] += output_gradient
        gradients["residual_gain"][0] += output_gradient * cache.inputs[index, 0]
        hidden_gradient = model.output_weight * output_gradient + next_hidden
        i, f, g, o = cache.gates[index]
        cell = cache.cells[index + 1]
        tanh_cell = np.tanh(cell)
        cell_gradient = next_cell + hidden_gradient * o * (1.0 - tanh_cell * tanh_cell)
        gate_i = cell_gradient * g * i * (1.0 - i)
        gate_f = cell_gradient * cache.cells[index] * f * (1.0 - f)
        gate_g = cell_gradient * i * (1.0 - g * g)
        gate_o = hidden_gradient * tanh_cell * o * (1.0 - o)
        affine_gradient = np.concatenate((gate_i, gate_f, gate_g, gate_o)).astype(np.float32)
        gradients["input_weight"] += np.outer(affine_gradient, cache.inputs[index])
        gradients["recurrent_weight"] += np.outer(affine_gradient, cache.hidden[index])
        gradients["bias"] += affine_gradient
        next_hidden = model.recurrent_weight.T @ affine_gradient
        next_cell = cell_gradient * f
    for gradient in gradients.values(): np.clip(gradient, -5.0, 5.0, out=gradient)
    return loss, gradients


def _feature_gradients(model: ConditionedGru | CausalTcn, cache: FeatureCache,
                       target: NDArray[np.floating], history_samples: int
                       ) -> tuple[float, dict[str, NDArray[np.float32]]]:
    expected = np.asarray(target, dtype=np.float32).reshape(-1)
    active = max(1, expected.size - history_samples)
    errors = cache.outputs - expected; errors[:history_samples] = 0.0
    output_gradient = (2.0 * errors / active).astype(np.float32)
    gradients = {name: np.zeros_like(value) for name, value in model.parameters().items()}
    gradients["output_weight"] = cache.features.T @ output_gradient
    gradients["output_bias"][0] = np.sum(output_gradient)
    if "residual_gain" in gradients:
        gradients["residual_gain"][0] = output_gradient @ cache.inputs[:, 0]
    for gradient in gradients.values(): np.clip(gradient, -5.0, 5.0, out=gradient)
    return float(np.sum(np.square(errors)) / active), gradients


def _gradients(model, cache, target: NDArray[np.floating], history_samples: int):
    if isinstance(model, TinyTanhRnn) and isinstance(cache, RnnCache):
        return _tanh_gradients(model, cache, target, history_samples)
    if isinstance(model, ConditionedLstm) and isinstance(cache, LstmCache):
        return _lstm_gradients(model, cache, target, history_samples)
    if isinstance(model, (ConditionedGru, CausalTcn)) and isinstance(cache, FeatureCache):
        return _feature_gradients(model, cache, target, history_samples)
    raise TypeError("Unsupported model/cache combination")


def train(model, training_chunks: Iterable[PairedChunk],
          validation_chunks: Iterable[PairedChunk], config: ExperimentConfig,
          output_directory: Path) -> TrainingResult:
    output_directory.mkdir(parents=True, exist_ok=True)
    config.save_resolved(output_directory / "resolved-config.json")
    training = list(training_chunks)
    validation = list(validation_chunks)
    if not training or not validation:
        raise ValueError("Training and validation data cannot be empty")
    optimizer = AdamW(model.parameters(), config.training.learning_rate, config.training.weight_decay)
    random = np.random.default_rng(config.training.seed)
    best_loss = float("inf")
    checkpoint = output_directory / "best-checkpoint.npz"
    epoch_losses: list[float] = []
    started = time.perf_counter()
    for _epoch in range(config.training.epochs):
        losses: list[float] = []
        accumulated = {name: np.zeros_like(value) for name, value in model.parameters().items()}
        batch_count = 0
        order = random.permutation(len(training))
        for position, index in enumerate(order):
            chunk = training[int(index)]
            model.reset()
            if hasattr(model, "set_controls"): model.set_controls(chunk.reference.controls)
            output, cache = model.forward(chunk.input, retain_cache=True)
            loss, gradients = _gradients(model, cache, chunk.target, chunk.reference.history_samples)
            for name in accumulated:
                accumulated[name] += gradients[name]
            batch_count += 1
            if batch_count == config.training.batch_size or position + 1 == len(order):
                optimizer.step({name: gradient / batch_count for name, gradient in accumulated.items()})
                for gradient in accumulated.values():
                    gradient.fill(0.0)
                batch_count = 0
            losses.append(loss)
        epoch_losses.append(float(np.mean(losses)))
        validation_losses: list[float] = []
        for chunk in validation:
            model.reset()
            if hasattr(model, "set_controls"): model.set_controls(chunk.reference.controls)
            prediction = model.process(chunk.input)
            mask = chunk.loss_mask
            objective, _components = combined_loss(prediction[mask], chunk.target[mask],
                                                   asdict(config.loss))
            validation_losses.append(objective)
        validation_loss = float(np.mean(validation_losses))
        if validation_loss < best_loss:
            best_loss = validation_loss
            model.save_checkpoint(checkpoint)
    return TrainingResult(checkpoint, best_loss, time.perf_counter() - started, epoch_losses)
