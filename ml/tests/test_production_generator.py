from __future__ import annotations

import unittest

import numpy as np

from nts_ml.production import (
    ProductionRanges,
    ProductionSettings,
    apply_chain,
    generate_dataset,
    sample_settings,
    shelf_response_db,
)

SAMPLE_RATE = 48000.0


def _dry(samples: int = 12000, seed: int = 7) -> np.ndarray:
    """A deterministic plucked broadband signal.

    Broadband and transient rather than a tone, for the reason the parity driver gives: on a
    single sine a reverb, a delay and a doubling delay all read as a phase shift, so three stages
    could be wrong the same way and still agree with each other.
    """
    random = np.random.default_rng(seed)
    time = np.arange(samples, dtype=np.float64) / SAMPLE_RATE
    value = np.zeros(samples, dtype=np.float64)
    for partial, frequency in enumerate((98.0, 196.0, 391.0, 782.0), start=1):
        value += np.sin(2.0 * np.pi * frequency * time) / (partial * partial)
    value += random.normal(0.0, 0.01, samples)
    envelope = np.exp(-np.mod(time, 0.3) * 7.0) * (1.0 - np.exp(-np.mod(time, 0.3) * 800.0))
    return (value * envelope * 0.3).astype(np.float32)


class ProductionChainTests(unittest.TestCase):
    def test_every_stage_is_bypassed_at_zero(self) -> None:
        """Zero settings must return the input untouched, on both sides.

        This is what makes a swept field interpretable: if any stage did something at zero, the
        harness would be measuring that stage's idle behaviour in every other field's column.
        """
        dry = _dry()
        left, right = apply_chain(dry, SAMPLE_RATE, ProductionSettings())
        np.testing.assert_array_equal(left, dry)
        np.testing.assert_array_equal(right, dry)

    def test_level_is_exactly_the_gain_that_was_asked_for(self) -> None:
        dry = _dry()
        for offset in (-12.0, -3.0, 4.5):
            left, _ = apply_chain(dry, SAMPLE_RATE, ProductionSettings(level_offset_db=offset))
            measured = 20.0 * np.log10(float(np.sqrt(np.mean(np.square(left))))
                                       / float(np.sqrt(np.mean(np.square(dry)))))
            self.assertAlmostEqual(measured, offset, places=3)

    def test_the_shelf_matches_its_own_analytic_response(self) -> None:
        """The applied filter and `shelf_response_db` must be the same filter.

        `nts_production_recovery` scores the spectral residual against that analytic curve, so if
        the curve and the stage disagreed the harness would report an error the estimator never
        made -- and, worse, would report success if both were wrong the same way. Measured here by
        transforming an impulse rather than by reasoning about the algebra.
        """
        impulse = np.zeros(8192, dtype=np.float32)
        impulse[0] = 1.0
        settings = ProductionSettings(tilt_db=6.0, tilt_hz=1200.0)
        response, _ = apply_chain(impulse, SAMPLE_RATE, settings)
        spectrum = np.fft.rfft(response.astype(np.float64))
        frequencies = np.fft.rfftfreq(response.size, 1.0 / SAMPLE_RATE)
        measured_db = 20.0 * np.log10(np.maximum(np.abs(spectrum), 1.0e-12))
        expected_db = shelf_response_db(settings, frequencies, SAMPLE_RATE)
        # Skipping DC and the last bin, where the transform of a finite impulse is least useful.
        band = slice(4, -4)
        self.assertLess(float(np.max(np.abs(measured_db[band] - expected_db[band]))), 0.05)

    def test_compression_narrows_the_gap_between_loud_and_quiet(self) -> None:
        """The defining property, probed with a signal that actually has two levels.

        **Not crest factor**, which was the first thing tried here and is the wrong measure for
        this stage. On a single decaying pluck a 5 ms attack lets the transient through before the
        follower catches it and then pulls the decay down, so crest factor *rises* while the
        signal is unambiguously being compressed. Measuring a loud passage against a quiet one is
        attack-time independent and is what "compression" means.

        Worth knowing that `compressionAmount` in the tone analyser leans on crest factor, which
        is a large part of why `nts_production_recovery` reports this field's rank agreement at
        0.73 rather than near 1. That is a finding about the estimator, not a reason to reshape
        the fixture until it flatters one.
        """
        sustained = np.zeros(48_000, dtype=np.float32)
        time = np.arange(sustained.size, dtype=np.float64) / SAMPLE_RATE
        for partial, frequency in enumerate((98.0, 196.0, 391.0, 782.0), start=1):
            sustained += (np.sin(2.0 * np.pi * frequency * time) / (partial * partial)).astype(np.float32)
        sustained *= np.where(time < 0.5, 0.30, 0.03).astype(np.float32)
        loud, quiet = slice(4_800, 22_000), slice(28_800, 46_000)

        def range_db(signal: np.ndarray) -> float:
            def rms(section: np.ndarray) -> float:
                return float(np.sqrt(np.mean(np.square(section.astype(np.float64)))))
            return 20.0 * np.log10(rms(signal[loud]) / rms(signal[quiet]))

        previous = range_db(sustained)
        for amount in (0.3, 0.6, 0.85):
            compressed, _ = apply_chain(sustained, SAMPLE_RATE, ProductionSettings(compression=amount))
            narrowed = range_db(compressed)
            self.assertLess(narrowed, previous)
            previous = narrowed

    def test_doubling_widens_and_reverb_adds_a_tail(self) -> None:
        """The other two spatial stages, each probed for the thing its label claims.

        A stage that merely changed the signal would pass a "did anything happen" check while
        being the wrong effect entirely -- and the label would still say `doubling`, which is what
        a model would then learn.
        """
        dry = _dry()
        left, right = apply_chain(dry, SAMPLE_RATE, ProductionSettings(doubling=0.8))
        correlation = float(np.corrcoef(left, right)[0, 1])
        self.assertLess(correlation, 0.99)

        # A tail is energy that arrives after the input stopped, so the probe is silence the input
        # genuinely has: the signal is padded with zeros and the pad is asked whether anything
        # reached it. Comparing the end of the *unpadded* signal instead measures the wrong thing
        # -- the chain scales the dry by `1 - mix` while the wet enters at `mix * 0.22`, so a
        # still-ringing note gets quieter overall even though a tail was unmistakably added.
        padded = np.concatenate([dry, np.zeros(8_000, dtype=np.float32)])
        wet_left, _ = apply_chain(padded, SAMPLE_RATE, ProductionSettings(reverb_amount=0.6))
        pad = slice(dry.size + 2_000, None)
        self.assertEqual(float(np.max(np.abs(padded[pad]))), 0.0)
        self.assertGreater(float(np.sqrt(np.mean(np.square(wet_left[pad])))), 1.0e-4)


class ProductionGeneratorTests(unittest.TestCase):
    def test_a_seed_reproduces_the_dataset_exactly(self) -> None:
        """The plan's "done when": a reproducible dataset from a seed.

        Both the labels and the audio, not just the labels -- a generator whose labels replayed
        but whose audio did not would produce two corpora that disagree about what a given example
        sounds like, which is worse than one that is openly random.
        """
        renders = [_dry(seed=1), _dry(seed=2), _dry(seed=3)]
        first = generate_dataset(renders, count=9, seed=4242)
        second = generate_dataset(renders, count=9, seed=4242)
        self.assertEqual([example.labels for example in first],
                         [example.labels for example in second])
        for left, right in zip(first, second, strict=True):
            np.testing.assert_array_equal(left.left, right.left)
            np.testing.assert_array_equal(left.right, right.right)

        different = generate_dataset(renders, count=9, seed=4243)
        self.assertNotEqual([example.labels for example in first],
                            [example.labels for example in different])

    def test_a_generated_example_carries_the_labels_that_made_it(self) -> None:
        """The other half of the plan's "done when".

        Re-applying an example's own labels to the render it came from must reproduce the example.
        That is the property the whole self-supervision argument rests on: if it did not hold, the
        labels would be a description of something other than the audio and a model trained on
        them would be learning noise with great confidence.
        """
        renders = [_dry(seed=11), _dry(seed=12)]
        for example in generate_dataset(renders, count=6, seed=99):
            left, right = apply_chain(renders[example.render_index], SAMPLE_RATE, example.settings)
            np.testing.assert_array_equal(example.left, left)
            np.testing.assert_array_equal(example.right, right)

    def test_settings_stay_inside_ranges_that_occur_in_real_mixes(self) -> None:
        """A model trained on implausible mixes is confident about them.

        The reverb bound is the one worth pinning: `apply_chain` maps the amount to a wet mix of
        `amount * 0.55`, and the plan's ceiling is 35%, so the sampled amount has to stay under
        0.636 for the corpus to contain only mixes somebody would actually make.
        """
        ranges = ProductionRanges()
        random = np.random.default_rng(5)
        doubled = 0
        for _ in range(400):
            settings = sample_settings(random, ranges)
            self.assertGreaterEqual(settings.level_offset_db, ranges.level_offset_db[0])
            self.assertLessEqual(settings.level_offset_db, ranges.level_offset_db[1])
            self.assertLessEqual(abs(settings.tilt_db), 8.0)
            self.assertLessEqual(settings.reverb_amount * 0.55, 0.35)
            self.assertTrue(settings.doubling == 0.0 or settings.doubling >= ranges.doubling[0])
            doubled += settings.doubling > 0.0
        # Doubling is on or off in an arrangement, and roughly half the corpus should have none --
        # a corpus where everything is doubled teaches that doubling is not a distinguishing fact.
        self.assertGreater(doubled, 120)
        self.assertLess(doubled, 280)

    def test_an_empty_render_pool_is_refused(self) -> None:
        with self.assertRaises(ValueError):
            generate_dataset([], count=4, seed=1)


if __name__ == "__main__":
    unittest.main()
