# Phase 2 DSP architecture

## Runtime boundary

`nts_dsp` is a JUCE-independent C++20 static library. Every real-time processor owns fixed channel state and allocates variable storage only in `prepare()`. Processing methods are `noexcept` and accept caller-owned buffers. The same filter, convolution, dynamics, nonlinear, oversampling, and metering implementations are used by real-time and offline callers.

`nts_ir` is the non-real-time adapter. It uses JUCE only to decode audio files on a `BackgroundWorker`, then passes framework-independent sample arrays through resampling, channel conversion, DC removal, leading-silence trimming, and normalization. Prepared samples can be loaded into the inactive partitioned convolver and atomically crossfaded on the audio thread.

## Signal building blocks

- Gain uses linear values internally with dB conversion only at control boundaries.
- Biquad coefficients use double-precision RBJ-style generation, Nyquist clamping, and optional per-sample coefficient interpolation.
- The Linkwitz-Riley crossover cascades matched second-order Butterworth low/high sections for phase-aligned bass-band recombination.
- Oversampling uses symmetric windowed-sinc FIR coefficients executed as interpolation polyphases and decimation-only output evaluation at 1x, 2x, 4x, or 8x. Both FIR stages have four base-rate samples of group delay, producing eight samples of reported round-trip latency.
- Dynamics detectors and gain computers are stateful per channel, with optional stereo linking, sidechain high-pass filtering, and internal ramps for automatable thresholds, ratios, knees, range, hysteresis, makeup, and ceiling.
- Long cabinet responses use uniform frequency-domain partitions; short responses may use direct convolution.
- `ModeCrossfader` runs arbitrary old/new processing modes in preallocated buffers, while SSE2 kernels accelerate suitable vector operations with scalar references retained for verification.

## Analysis and transport

The radix-2 FFT precomputes bit-reversal and twiddle tables in `prepare()`. STFT analysis supports four windows, configurable overlap, complex spectra, and overlap-add reconstruction. Spectrum and waveform displays publish downsampled values through fixed atomic arrays. Loudness follows BS.1770 K-weighting with 400 ms absolute/relative-gated integrated blocks and an exact three-second short-term window. Scalar meter readings are atomic.

## Validation

`nts_dsp_tests` covers independent impulse references, analytical response, extreme automation, mono/stereo paths, crossover summing, alias rejection, dynamics behavior, convolution references, mode/IR swaps, BS.1770 loudness, FFT/STFT reconstruction, fixed DI hashes, regression metrics, SIMD equivalence, and global allocation instrumentation. `nts_ir_tests` creates and asynchronously decodes a real WAV file. `nts_dsp_benchmarks` records average and 99th-percentile callback time, memory, latency, callback-budget share, SIMD speedup, and metering overhead for the required sample-rate, block-size, and channel matrix.
