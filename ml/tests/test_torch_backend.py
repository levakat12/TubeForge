from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

import numpy as np

from nts_ml.datasets.streaming import ChunkReference, PairedChunk
from nts_ml.models import ConditionedLstm
from nts_ml.training import ExperimentConfig, torch_backend
from nts_ml.training.trainer import train


def make_chunks(count: int, samples: int = 512, seed: int = 4) -> list[PairedChunk]:
    rng = np.random.default_rng(seed)
    chunks = []
    for index in range(count):
        audio = rng.normal(0.0, 0.15, samples).astype(np.float32)
        mask = np.ones(samples, dtype=np.bool_)
        mask[:64] = False
        reference = ChunkReference(session_id="torch", take_id=f"take-{index}",
                                   input_path=Path("in.wav"), output_path=Path("out.wav"),
                                   start=0, output_start=0, total_samples=samples,
                                   history_samples=64, controls=(0.2, -0.1, 0.4, 0.0, 1.0))
        chunks.append(PairedChunk(audio, np.tanh(3.0 * audio).astype(np.float32), mask, reference))
    return chunks


class TorchBackendAvailabilityTests(unittest.TestCase):
    """These run whether or not torch is installed."""

    def test_numpy_backend_never_touches_torch(self) -> None:
        """The default path must not depend on the optional backend in any way."""
        config = ExperimentConfig()
        self.assertEqual(config.training.backend, "numpy")
        config.training.epochs = 1
        model = ConditionedLstm(4, 48_000, seed=1)
        chunks = make_chunks(2)
        with tempfile.TemporaryDirectory() as temporary:
            result = train(model, chunks, chunks, config, Path(temporary))
            self.assertTrue(result.best_checkpoint.exists())

    def test_auto_backend_falls_back_when_torch_is_missing(self) -> None:
        """`auto` means "use it if it is there", so it must work either way."""
        config = ExperimentConfig()
        config.training.backend = "auto"
        config.training.epochs = 1
        model = ConditionedLstm(4, 48_000, seed=1)
        chunks = make_chunks(2)
        with tempfile.TemporaryDirectory() as temporary:
            result = train(model, chunks, chunks, config, Path(temporary))
            self.assertTrue(result.best_checkpoint.exists())

    def test_explicit_torch_backend_reports_clearly_when_unavailable(self) -> None:
        if torch_backend.is_available():
            self.skipTest("torch is installed, so the unavailable path cannot be reached")
        config = ExperimentConfig()
        config.training.backend = "torch"
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaises(RuntimeError):
                train(ConditionedLstm(4, seed=1), make_chunks(2), make_chunks(1), config,
                      Path(temporary))

    def test_rejects_an_unknown_backend(self) -> None:
        config = ExperimentConfig()
        config.training.backend = "tensorflow"
        with self.assertRaises(ValueError):
            config.validate()


@unittest.skipUnless(torch_backend.is_available(), "requires the optional torch backend")
class TorchBackendTests(unittest.TestCase):
    def test_torch_lstm_matches_the_numpy_reference(self) -> None:
        """The claim the whole backend rests on.

        If these disagree, a model trained with torch is not the model the packed export and
        the C++ runtime will go on to run.
        """
        import torch

        rng = np.random.default_rng(7)
        controls = [0.3, -0.4, 0.6, 1.0, -1.0]
        for state_size, seed in ((8, 1), (32, 2)):
            reference = ConditionedLstm(state_size, 48_000, seed=seed)
            reference.set_controls(controls)
            reference.reset()
            audio = rng.normal(0.0, 0.2, 1024).astype(np.float32)
            expected = reference.process(audio)

            module = torch_backend.TorchConditionedLstm(state_size, reference.control_count,
                                                        bool(reference.residual))
            module.load_from_numpy(reference)
            module.eval()
            with torch.no_grad():
                actual = module(torch.from_numpy(audio).unsqueeze(0),
                                torch.tensor([controls], dtype=torch.float32)).squeeze(0).numpy()

            scale = float(np.max(np.abs(expected))) or 1.0
            self.assertLess(float(np.max(np.abs(expected - actual))) / scale, 1e-5,
                            f"torch and numpy disagree at state_size={state_size}")

    def test_windowing_a_long_sequence_changes_nothing(self) -> None:
        """cuDNN cannot take an arbitrarily long sequence, so long ones are windowed.

        Carrying the state across windows has to be exactly equivalent to one long call, or
        the workaround for the length limit would quietly alter what the model learns.
        """
        import torch

        module = torch_backend.TorchConditionedLstm(8, 5, True)
        module.eval()
        samples = 2048
        audio = torch.randn(1, samples)
        controls = torch.tensor([[0.1, 0.2, -0.3, 0.4, -0.5]])

        with torch.no_grad():
            whole = module(audio, controls)
            original = module.maximum_fused_timesteps
            try:
                module.maximum_fused_timesteps = 256   # force many windows
                windowed = module(audio, controls)
            finally:
                module.maximum_fused_timesteps = original

        self.assertLess(float((whole - windowed).abs().max()), 1e-5,
                        "windowing a long sequence changed the result")

    def test_torch_training_reduces_the_loss_and_writes_a_loadable_checkpoint(self) -> None:
        config = ExperimentConfig()
        config.training.backend = "torch"
        config.training.epochs = 6
        config.training.learning_rate = 0.01
        config.loss.pre_emphasis = 0.3

        model = ConditionedLstm(8, 48_000, seed=5)
        model.set_controls([0.2, -0.1, 0.4, 0.0, 1.0])
        training, validation = make_chunks(4), make_chunks(2, seed=9)
        with tempfile.TemporaryDirectory() as temporary:
            result = train(model, training, validation, config, Path(temporary))
            self.assertLess(result.epoch_losses[-1], result.epoch_losses[0],
                            "torch training did not reduce the loss")
            # The checkpoint has to be an ordinary NumPy one; that is what keeps the packed
            # export and the C++ runtime unaware of which backend trained the weights.
            restored = ConditionedLstm.load_checkpoint(result.best_checkpoint)
            self.assertEqual(restored.state_size, model.state_size)

    def test_seeded_torch_training_is_reproducible(self) -> None:
        """Reproducible to float32 tolerance, which is a weaker promise than the NumPy path.

        The NumPy trainer is bit-exact for a given seed. This one is not, and cannot be made
        so cheaply: cuDNN's LSTM backward pass accumulates in a nondeterministic order, and
        forcing determinism would mean giving up the fused kernel this backend exists to use.
        Repeated runs agree to around 1e-7, which is float32 rounding rather than a different
        trajectory -- but anyone relying on bit-exact reproduction should train on numpy.
        """
        training, validation = make_chunks(4), make_chunks(2, seed=9)

        def run() -> np.ndarray:
            config = ExperimentConfig()
            config.training.backend = "torch"
            config.training.epochs = 4
            config.training.learning_rate = 0.01
            config.training.seed = 4242
            model = ConditionedLstm(8, 48_000, seed=5)
            model.set_controls([0.2, -0.1, 0.4, 0.0, 1.0])
            with tempfile.TemporaryDirectory() as temporary:
                train(model, training, validation, config, Path(temporary))
            return model.input_weight.copy()

        np.testing.assert_allclose(run(), run(), rtol=1e-4, atol=1e-6,
                                   err_msg="seeded torch training diverged beyond float32 rounding")

    def test_differentiable_loss_agrees_with_the_reference_components(self) -> None:
        """The torch objective must be the same objective, or selection drifts from training."""
        import torch

        from nts_ml.losses import combined_loss as numpy_combined_loss

        rng = np.random.default_rng(12)
        prediction = rng.normal(0.0, 0.2, 1024).astype(np.float32)
        target = np.tanh(2.0 * prediction).astype(np.float32)

        for weights in ({"time_l1": 1.0},
                        {"pre_emphasis": 1.0},
                        {"time_l1": 0.5, "dc_penalty": 0.2, "silence_stability": 0.1}):
            expected, _components = numpy_combined_loss(prediction, target, weights)
            actual = float(torch_backend.combined_loss(
                torch.from_numpy(prediction).unsqueeze(0),
                torch.from_numpy(target).unsqueeze(0), weights))
            self.assertAlmostEqual(actual, expected, places=4,
                                   msg=f"torch and numpy loss differ for {sorted(weights)}")


if __name__ == "__main__":
    unittest.main()
