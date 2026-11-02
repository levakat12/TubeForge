"""The synthetic production chain, and the analytic response of its EQ.

**This is a second implementation of `Tests/ProductionChain.h`, and that is a liability.** The C++
copy scores `estimateProductionProfile` in `nts_production_recovery`; this one generates the
training data for the estimator that is meant to beat it. If the two drift, the model learns one
mix and the harness scores it on another, and neither would report anything wrong --  the model
would simply be worse in the field than in the benchmark, which is the failure mode that is
hardest to attribute after the fact. `ml/tests/production_parity_driver.py` is what stops that,
and it is the reason this file matches the C++ one stage for stage and constant for constant
rather than being written the way one would write it fresh.

Every stage is bypassed exactly when its control is zero, so a swept field is the only thing
moving. Stage order is the order a mix is built in --  dynamics, tone, space, level --  because
applying the level first would make the compressor's threshold mean something different at every
setting and turn one swept field into two.
"""

from __future__ import annotations

from dataclasses import asdict, dataclass

import numpy as np
from numpy.typing import NDArray


@dataclass(slots=True)
class ProductionSettings:
    """What was applied, which is also the label. Every field is zero-is-bypassed."""

    level_offset_db: float = 0.0
    """Gain in dB applied last, after everything else. What `levelOffsetDb` should recover."""

    tilt_db: float = 0.0
    """High-shelf height in dB; positive is brighter. The only field with an exact analytic
    answer -- see `shelf_response_db` -- and so the only one the spectral residual can be scored
    against rather than merely correlated with."""

    tilt_hz: float = 1000.0
    reverb_amount: float = 0.0
    """0..1 into a small Schroeder network. Not a plate and not a room; see the module docstring
    of `generator` for why that matters and what guards it."""

    delay_amount: float = 0.0
    delay_ms: float = 220.0
    doubling: float = 0.0
    compression: float = 0.0

    def as_labels(self) -> dict[str, float]:
        """The example's label vector, as a plain dict for serialisation."""
        return {key: float(value) for key, value in asdict(self).items()}


def shelf_response_db(settings: ProductionSettings, frequency: NDArray[np.floating] | float,
                      sample_rate: float) -> NDArray[np.float64]:
    """The high shelf's magnitude response in dB at `frequency`.

    Exact rather than measured, so scoring the spectral residual compares it against what was
    actually applied instead of against a second estimate of it. The stage is
    ``y = x + g (x - lowpass(x))`` with a one-pole lowpass, so::

        H(z) = 1 + g a (1 - z^-1) / (1 - a z^-1)

    and this evaluates it on the unit circle.
    """
    frequencies = np.asarray(frequency, dtype=np.float64)
    if abs(settings.tilt_db) < 1.0e-6:
        return np.zeros_like(frequencies)
    pole = np.exp(-2.0 * np.pi * settings.tilt_hz / sample_rate)
    gain = 10.0 ** (settings.tilt_db / 20.0) - 1.0
    inverse_z = np.exp(-2.0j * np.pi * frequencies / sample_rate)
    response = 1.0 + gain * pole * (1.0 - inverse_z) / (1.0 - pole * inverse_z)
    return 20.0 * np.log10(np.maximum(np.abs(response), 1.0e-9))


def _delayed(channel: NDArray[np.float32], delay: int) -> NDArray[np.float32]:
    """`channel` shifted later by `delay` samples, zero-filled at the front."""
    if delay <= 0:
        return channel.copy()
    shifted = np.zeros_like(channel)
    if delay < channel.size:
        shifted[delay:] = channel[:-delay]
    return shifted


def _comb(source: NDArray[np.float32], length: int, feedback: float) -> NDArray[np.float32]:
    """A feedback comb, evaluated the way the C++ copy evaluates it.

    Written as an explicit recurrence over blocks of `length` rather than sample by sample: the
    output at index i depends only on index i - length, so a whole stride can be advanced at once
    and the result is bit-comparable with the scalar loop it mirrors while being fast enough that
    generating a dataset is not an overnight job.
    """
    state = source.astype(np.float32).copy()
    if length <= 0 or length >= state.size:
        return state
    for start in range(length, state.size, length):
        stop = min(start + length, state.size)
        state[start:stop] += np.float32(feedback) * state[start - length:stop - length]
    return state


def _all_pass(channel: NDArray[np.float32], length: int) -> NDArray[np.float32]:
    """A Schroeder all-pass. Recursive in its own output, so it advances a stride at a time too."""
    out = -0.5 * channel + _delayed(channel, length)
    out = out.astype(np.float32)
    if length <= 0 or length >= out.size:
        return out
    for start in range(length, out.size, length):
        stop = min(start + length, out.size)
        out[start:stop] += np.float32(0.5) * out[start - length:stop - length]
    return out


def apply_chain(dry: NDArray[np.floating], sample_rate: float, settings: ProductionSettings
                ) -> tuple[NDArray[np.float32], NDArray[np.float32]]:
    """Applies the chain to a mono render, returning a stereo pair.

    Stereo because half of what is being measured is spatial: a doubled, widened part is not a
    mono signal with a number attached to it, and handing the analyser a mono buffer would make
    stereo width and double-tracking unmeasurable by construction.
    """
    left = np.asarray(dry, dtype=np.float32).copy()
    if left.ndim != 1:
        raise ValueError("The production chain takes a mono render")
    right = left.copy()

    # ---- Dynamics. A peak follower into a fixed -18 dBFS threshold, so the only thing the amount
    # changes is the ratio and the makeup that compensates for it.
    if settings.compression > 1.0e-4:
        ratio = 1.0 + settings.compression * 7.0
        threshold = 10.0 ** (-18.0 / 20.0)
        attack = float(np.exp(-1.0 / (sample_rate * 0.005)))
        release = float(np.exp(-1.0 / (sample_rate * 0.120)))
        makeup = 10.0 ** (settings.compression * 4.0 / 20.0)
        # Sample by sample, because the follower's coefficient depends on whether the level rose
        # or fell and so cannot be expressed as one filter.
        envelope = 0.0
        for index in range(left.size):
            level = abs(float(left[index]))
            coefficient = attack if level > envelope else release
            envelope = coefficient * envelope + (1.0 - coefficient) * level
            gain = 1.0
            if envelope > threshold:
                gain = (threshold + (envelope - threshold) / ratio) / envelope
            left[index] = np.float32(left[index] * gain * makeup)
        right = left.copy()

    # ---- Tone. The shelf `shelf_response_db` describes analytically.
    if abs(settings.tilt_db) > 1.0e-6:
        pole = float(np.exp(-2.0 * np.pi * settings.tilt_hz / sample_rate))
        gain = 10.0 ** (settings.tilt_db / 20.0) - 1.0
        lowpass = 0.0
        for index in range(left.size):
            lowpass = lowpass * pole + float(left[index]) * (1.0 - pole)
            left[index] = np.float32(float(left[index]) + (float(left[index]) - lowpass) * gain)
        right = left.copy()

    # ---- Doubling. A short delayed copy sent to the opposite side. Deliberately not symmetric: a
    # real double-track is two performances, and the closest honest approximation with one
    # performance in hand is one image sitting a few milliseconds behind the other.
    if settings.doubling > 1.0e-4:
        delay = int(sample_rate * (0.008 + 0.014 * settings.doubling))
        amount = settings.doubling * 0.85
        dry_left = left.copy()
        late = _delayed(dry_left, delay)
        left = (dry_left * (1.0 - amount * 0.35) + late * amount * 0.25).astype(np.float32)
        right = (dry_left * (1.0 - amount * 0.35) * (1.0 - amount * 0.6)
                 + late * amount * 0.95).astype(np.float32)

    # ---- A single delayed repeat. One tap rather than a feedback line, so `delay_ms` is
    # unambiguous and a swept amount changes the amount and nothing else.
    if settings.delay_amount > 1.0e-4:
        delay = int(sample_rate * settings.delay_ms / 1000.0)
        amount = settings.delay_amount * 0.5
        dry_left, dry_right = left.copy(), right.copy()
        left = (dry_left + _delayed(dry_right, delay) * amount).astype(np.float32)
        right = (dry_right + _delayed(dry_left, delay) * amount * 0.9).astype(np.float32)

    # ---- Space. Four combs into two all-passes, which is the smallest thing that produces a
    # decaying diffuse tail rather than a discrete echo. The prime-ish lengths are what stop the
    # combs lining up into a pitched ring.
    if settings.reverb_amount > 1.0e-4:
        comb_ms = (29.7, 37.1, 41.1, 43.7)
        all_pass_ms = (5.0, 1.7)
        feedback = 0.70 + settings.reverb_amount * 0.16
        mix = settings.reverb_amount * 0.55
        dry_left, dry_right = left.copy(), right.copy()
        wet_left = np.zeros_like(dry_left)
        wet_right = np.zeros_like(dry_right)
        for index, milliseconds in enumerate(comb_ms):
            # The two sides get slightly different lengths, which is what makes the tail wide
            # rather than a mono tail sitting in the middle.
            length_left = int(sample_rate * milliseconds / 1000.0)
            length_right = length_left + 37 + index * 11
            wet_left += _comb(dry_left, length_left, feedback) * 0.25
            wet_right += _comb(dry_right, length_right, feedback) * 0.25
        for milliseconds in all_pass_ms:
            length = int(sample_rate * milliseconds / 1000.0)
            wet_left = _all_pass(wet_left, length)
            wet_right = _all_pass(wet_right, length)
        left = (dry_left * (1.0 - mix) + wet_left * mix * 0.22).astype(np.float32)
        right = (dry_right * (1.0 - mix) + wet_right * mix * 0.22).astype(np.float32)

    # ---- Level, last, so nothing downstream of it has a threshold that moves with it.
    if abs(settings.level_offset_db) > 1.0e-6:
        gain = np.float32(10.0 ** (settings.level_offset_db / 20.0))
        left = (left * gain).astype(np.float32)
        right = (right * gain).astype(np.float32)

    return left, right
