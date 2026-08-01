"""Optional PyTorch training backend.

Imports torch lazily and is never a declared dependency of this package, matching how the
Demucs separator worker treats it. Without torch installed nothing here is reachable and
`nts_ml.training.train` keeps using the NumPy path unchanged.

Why it exists: the NumPy trainer runs backpropagation through time in a Python loop, one
iteration per sample. At 48 kHz a minute of paired audio is 2.9 million iterations per epoch,
which puts a realistic capture somewhere between hours and days. The same recurrence through
`torch.nn.LSTM` is a single fused kernel call per sequence.

The weight layout is deliberately *identical* to `ConditionedLstm`, not merely equivalent.
`nn.LSTM` orders its gate blocks input, forget, cell, output -- the same order the NumPy
implementation uses -- so the matrices transfer without permutation. Its only structural
difference is carrying two bias vectors where the NumPy model has one, which is resolved by
putting the whole bias in `bias_ih_l0` and zeroing `bias_hh_l0`. That exactness is what lets
a model be trained here and then exported, packed and run by the C++ runtime with the
existing parity test unchanged.
"""
from __future__ import annotations

import json
from dataclasses import asdict
from pathlib import Path
from typing import Any

import numpy as np

from ..models import ConditionedLstm

TORCH_IMPORT_ERROR: str | None = None
try:  # pragma: no cover - depends on the machine, both paths are exercised by tests
    import torch
    from torch import nn
except Exception as error:  # noqa: BLE001 - any import failure means "no torch backend"
    torch = None  # type: ignore[assignment]
    nn = None  # type: ignore[assignment]
    TORCH_IMPORT_ERROR = str(error)


def is_available() -> bool:
    """True when a usable torch is importable."""
    return torch is not None


def unavailable_reason() -> str:
    return TORCH_IMPORT_ERROR or ""


def select_device(preference: str = "auto") -> str:
    if torch is None:
        raise RuntimeError("torch is not available")
    if preference == "auto":
        return "cuda" if torch.cuda.is_available() else "cpu"
    return preference


class TorchConditionedLstm(nn.Module if nn is not None else object):  # type: ignore[misc]
    """`ConditionedLstm` expressed as a torch module, weight-for-weight."""

    #: Longest sequence handed to cuDNN in one call. See _run_lstm for where this comes from.
    maximum_fused_timesteps = 32768

    def __init__(self, state_size: int, control_count: int, residual: bool):
        super().__init__()
        self.state_size = state_size
        self.control_count = control_count
        self.lstm = nn.LSTM(input_size=1 + control_count, hidden_size=state_size,
                            num_layers=1, batch_first=True)
        self.output_weight = nn.Parameter(torch.zeros(state_size))
        self.output_bias = nn.Parameter(torch.zeros(1))
        self.residual_gain = nn.Parameter(torch.tensor([0.25 if residual else 0.0]))

    def forward(self, audio: torch.Tensor, controls: torch.Tensor) -> torch.Tensor:
        # audio: (batch, time). controls: (batch, control_count), constant across the chunk.
        batch, time = audio.shape
        expanded = controls.unsqueeze(1).expand(batch, time, self.control_count)
        inputs = torch.cat((audio.unsqueeze(-1), expanded), dim=-1).contiguous()
        hidden, _state = self._run_lstm(inputs)
        readout = hidden @ self.output_weight + self.output_bias
        return readout + self.residual_gain * audio

    def _run_lstm(self, inputs: torch.Tensor):
        """Runs the recurrence, stepping around cuDNN's sequence-length ceiling.

        Measured on an RTX 5080 with torch 2.11/cu128: cuDNN's fused LSTM handles 32768
        timesteps and fails at 65536 with CUDNN_STATUS_NOT_SUPPORTED, on contiguous input.
        The message blames non-contiguity, which is misleading -- it is a length limit.

        Long sequences are therefore fed through in windows with the hidden and cell state
        carried across, rather than being handed to a non-cuDNN path. The distinction matters:
        disabling cuDNN measured no faster than the NumPy trainer it exists to replace, while
        windowing keeps the fused kernel and stays roughly two orders of magnitude ahead. No
        state is detached between windows, so backpropagation still spans the whole sequence.
        """
        time = inputs.shape[1]
        if time <= self.maximum_fused_timesteps:
            return self.lstm(inputs)

        outputs = []
        state = None
        for start in range(0, time, self.maximum_fused_timesteps):
            window = inputs[:, start:start + self.maximum_fused_timesteps].contiguous()
            hidden, state = self.lstm(window, state)
            outputs.append(hidden)
        return torch.cat(outputs, dim=1), state

    def load_from_numpy(self, model: ConditionedLstm) -> None:
        """Copies weights across with no permutation; see the module docstring."""
        with torch.no_grad():
            self.lstm.weight_ih_l0.copy_(torch.from_numpy(model.input_weight))
            self.lstm.weight_hh_l0.copy_(torch.from_numpy(model.recurrent_weight))
            # One NumPy bias becomes torch's input-side bias, with the hidden-side bias zeroed,
            # so the sum torch forms matches the single vector the NumPy model adds.
            self.lstm.bias_ih_l0.copy_(torch.from_numpy(model.bias))
            self.lstm.bias_hh_l0.zero_()
            self.output_weight.copy_(torch.from_numpy(model.output_weight))
            self.output_bias.copy_(torch.from_numpy(model.output_bias))
            self.residual_gain.copy_(torch.from_numpy(model.residual_gain))

    def store_into_numpy(self, model: ConditionedLstm) -> None:
        with torch.no_grad():
            model.input_weight[...] = self.lstm.weight_ih_l0.detach().cpu().numpy()
            model.recurrent_weight[...] = self.lstm.weight_hh_l0.detach().cpu().numpy()
            # Fold both bias vectors back into the single one the NumPy model and the packed
            # export format carry. torch may have moved bias_hh during training.
            model.bias[...] = (self.lstm.bias_ih_l0 + self.lstm.bias_hh_l0).detach().cpu().numpy()
            model.output_weight[...] = self.output_weight.detach().cpu().numpy()
            model.output_bias[...] = self.output_bias.detach().cpu().numpy()
            model.residual_gain[...] = self.residual_gain.detach().cpu().numpy()
        model.reset()


def _pre_emphasise(signal: torch.Tensor, coefficient: float) -> torch.Tensor:
    shifted = torch.nn.functional.pad(signal, (1, 0))[..., :-1]
    return signal - coefficient * shifted


def _stft_magnitude(signal: torch.Tensor, size: int) -> torch.Tensor:
    window = torch.hann_window(size, device=signal.device, dtype=signal.dtype)
    spectrum = torch.stft(signal, n_fft=size, hop_length=size // 4, win_length=size,
                          window=window, return_complex=True, center=True)
    return spectrum.abs()


def combined_loss(prediction: torch.Tensor, target: torch.Tensor,
                  weights: dict[str, float]) -> torch.Tensor:
    """The selection objective, differentiable.

    Mirrors `nts_ml.losses.audio.combined_loss` term for term, so the value a checkpoint is
    chosen by is the value that produced it -- which was the point of plan item D2.
    """
    total = prediction.new_zeros(())

    def weight(name: str) -> float:
        return float(weights.get(name, 0.0))

    if weight("time_l1") > 0.0:
        total = total + weight("time_l1") * (prediction - target).abs().mean()
    if weight("time_l2") > 0.0:
        total = total + weight("time_l2") * (prediction - target).pow(2).mean()
    if weight("pre_emphasis") > 0.0:
        total = total + weight("pre_emphasis") * (
            _pre_emphasise(prediction, 0.95) - _pre_emphasise(target, 0.95)).abs().mean()
    if weight("multi_resolution_stft") > 0.0 or weight("spectral_convergence") > 0.0:
        stft_total = prediction.new_zeros(())
        convergence_total = prediction.new_zeros(())
        sizes = [size for size in (256, 512, 1024) if size <= prediction.shape[-1]]
        for size in sizes or [min(256, prediction.shape[-1])]:
            predicted = _stft_magnitude(prediction, size)
            expected = _stft_magnitude(target, size)
            stft_total = stft_total + (predicted - expected).abs().mean()
            convergence_total = convergence_total + (
                (predicted - expected).norm() / expected.norm().clamp_min(1.0e-8))
        count = float(max(1, len(sizes)))
        total = total + weight("multi_resolution_stft") * stft_total / count
        total = total + weight("spectral_convergence") * convergence_total / count
    if weight("dc_penalty") > 0.0:
        total = total + weight("dc_penalty") * prediction.mean(dim=-1).abs().mean()
    if weight("loudness_difference") > 0.0:
        difference = (prediction.pow(2).mean(dim=-1).clamp_min(1.0e-12).log10()
                      - target.pow(2).mean(dim=-1).clamp_min(1.0e-12).log10()) * 10.0
        total = total + weight("loudness_difference") * difference.abs().mean()
    if weight("silence_stability") > 0.0:
        quiet = (target.abs() < 1.0e-3).to(prediction.dtype)
        denominator = quiet.sum().clamp_min(1.0)
        total = total + weight("silence_stability") * (prediction.abs() * quiet).sum() / denominator
    if weight("transient_weighted") > 0.0:
        emphasis = 1.0 + 4.0 * target.diff(dim=-1, prepend=target[..., :1]).abs()
        total = total + weight("transient_weighted") * (
            (prediction - target).abs() * emphasis).mean()
    return total


def train_lstm(model: ConditionedLstm, training_chunks: list[Any], validation_chunks: list[Any],
               config: Any, output_directory: Path, device: str = "auto") -> dict[str, Any]:
    """Trains a ConditionedLstm with torch and writes the usual .npz checkpoint.

    The checkpoint format is untouched: `store_into_numpy` puts the trained weights back into
    the NumPy model, which saves exactly as it always did. That is what keeps the packed
    export and the C++ runtime parity test valid.
    """
    if torch is None:
        raise RuntimeError("torch is not available")

    torch.manual_seed(config.training.seed)
    resolved = select_device(device)
    module = TorchConditionedLstm(model.state_size, model.control_count, bool(model.residual))
    module.load_from_numpy(model)
    module.to(resolved)

    optimiser = torch.optim.AdamW(module.parameters(), lr=config.training.learning_rate,
                                  weight_decay=config.training.weight_decay)
    loss_weights = asdict(config.loss)
    output_directory.mkdir(parents=True, exist_ok=True)
    checkpoint = output_directory / "best-checkpoint.npz"

    def batches(chunks: list[Any]):
        size = max(1, config.training.batch_size)
        for start in range(0, len(chunks), size):
            group = chunks[start:start + size]
            audio = torch.tensor(np.stack([c.input for c in group]), dtype=torch.float32, device=resolved)
            target = torch.tensor(np.stack([c.target for c in group]), dtype=torch.float32, device=resolved)
            controls = torch.tensor(np.stack([np.asarray(c.reference.controls, dtype=np.float32)
                                              for c in group]), dtype=torch.float32, device=resolved)
            history = int(group[0].reference.history_samples)
            yield audio, target, controls, history

    best_loss = float("inf")
    epoch_losses: list[float] = []
    for _epoch in range(config.training.epochs):
        module.train()
        losses: list[float] = []
        for audio, target, controls, history in batches(training_chunks):
            optimiser.zero_grad(set_to_none=True)
            prediction = module(audio, controls)
            # The history region primes the recurrence and is excluded from the objective,
            # exactly as the NumPy trainer masks it.
            loss = combined_loss(prediction[..., history:], target[..., history:], loss_weights)
            loss.backward()
            torch.nn.utils.clip_grad_norm_(module.parameters(), 5.0)
            optimiser.step()
            losses.append(float(loss.detach().cpu()))
        epoch_losses.append(float(np.mean(losses)) if losses else float("nan"))

        module.eval()
        with torch.no_grad():
            validation = [float(combined_loss(module(a, c)[..., h:], t[..., h:], loss_weights).cpu())
                          for a, t, c, h in batches(validation_chunks)]
        score = float(np.mean(validation)) if validation else float("inf")
        if score < best_loss:
            best_loss = score
            module.store_into_numpy(model)
            model.save_checkpoint(checkpoint)

    return {"best_checkpoint": str(checkpoint), "best_validation_loss": best_loss,
            "epoch_losses": epoch_losses, "device": resolved,
            "torch_version": torch.__version__}


def write_report(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
