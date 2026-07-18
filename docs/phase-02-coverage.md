# Phase 2 coverage

Coverage is scored against 91 observable requirements extracted from the Phase 2 plan. Complete items score 1 point, partial items score 0.5, and missing items score 0.

**Current verified score: 91 / 91 = 100.0%.**

| # | Requirement | Status | Evidence or gap |
|---:|---|:---:|---|
| 1 | Reusable deterministic DSP library | Complete | JUCE-independent `nts_dsp` C++20 target |
| 2 | Uniform prepare/reset/process contract | Complete | Every real-time block processor has explicit preparation, reset, and allocation-free processing; offline transforms retain task-specific APIs |
| 3 | Channel-aware state | Complete | Fixed mono/stereo state in filters, dynamics, convolution, meters, and bass split |
| 4 | No processing-time allocation | Complete | Instrumented multi-processor allocation test |
| 5 | Smoothed parameters | Complete | Gain/filter smoothing plus internal gate, compressor, and limiter automation ramps are verified |
| 6 | Coefficients outside inner loops | Complete | Filter and dynamics coefficients update in setters |
| 7 | Denormal suppression | Complete | Explicit state flushing plus wrapper `ScopedNoDenormals` |
| 8 | Explicit mono and stereo tests | Complete | Both paths exercised throughout `nts_dsp_tests` and benchmark matrix |
| 9 | Shared offline/realtime implementation | Complete | One DSP library is used by block processing and offline regression/IR paths |
| 10 | dB-to-linear conversion | Complete | `dbToLinear` with numeric test |
| 11 | Linear-to-dB conversion | Complete | `linearToDb` with numeric test |
| 12 | Ramped gain | Complete | `SmoothedGain` and zipper-free ramp test |
| 13 | First-order low-pass | Complete | `OnePoleFilter::lowPass` |
| 14 | First-order high-pass | Complete | `OnePoleFilter::highPass` |
| 15 | Second-order Butterworth low-pass | Complete | RBJ biquad at Butterworth Q |
| 16 | Second-order Butterworth high-pass | Complete | RBJ biquad at Butterworth Q |
| 17 | Peaking EQ | Complete | Coefficient generator and center-gain test |
| 18 | Low shelf | Complete | Coefficient generator and low-frequency test |
| 19 | High shelf | Complete | Coefficient generator and high-frequency test |
| 20 | Notch | Complete | Coefficient generator and rejection test |
| 21 | All-pass | Complete | Coefficient generator and unity-magnitude test |
| 22 | State-variable filter | Complete | Low, band, high, and notch TPT outputs |
| 23 | Linkwitz-Riley crossover | Complete | Cascaded matched Butterworth sections |
| 24 | Nyquist guarding | Complete | Central frequency clamp used by all coefficient generators |
| 25 | Coefficient interpolation | Complete | Optional sample-count interpolation in `Biquad` and crossover |
| 26 | Expected magnitude-response verification | Complete | Analytical biquad response tests for every filter family |
| 27 | Independent impulse reference for every filter | Complete | Direct-form biquad, one-pole recurrence, TPT SVF, and cascaded crossover references verify every filter impulse |
| 28 | Extreme frequency/Q stability | Complete | Automated 1 Hz/23.9 kHz and Q 0.05/50 stress |
| 29 | Filter parameter modulation | Complete | Repeated interpolated extreme coefficient changes |
| 30 | 1x/2x/4x/8x oversampling | Complete | `OversamplingFactor` and tests for all four modes |
| 31 | Polyphase half-band implementation | Complete | Symmetric windowed-sinc anti-alias FIR executes through interpolation phases and decimation-only output evaluation |
| 32 | Upsample/nonlinear/filter/downsample sequence | Complete | `Oversampler::process` template |
| 33 | Exact oversampling latency | Complete | Designed and reported 0 or 8 base-rate samples |
| 34 | Passband-ripple test | Complete | FIR response measured below transition band |
| 35 | Stopband-attenuation test | Complete | FIR response measured in image stopband |
| 36 | Nonlinear alias-rejection test | Complete | 10 kHz driven-tanh alias comparison at 1x and 8x |
| 37 | Phase-behavior test | Complete | Symmetric coefficient/linear-phase verification |
| 38 | Oversampling CPU cost | Complete | Average and p99 benchmark rows for 4x mono/stereo |
| 39 | Oversampling latency test | Complete | All factors asserted against designed latency |
| 40 | Tanh nonlinearity | Complete | `shapeSample` |
| 41 | Arctangent nonlinearity | Complete | `shapeSample` |
| 42 | Hard clipping | Complete | `shapeSample` |
| 43 | Soft clipping | Complete | Cubic soft transfer |
| 44 | Asymmetric polynomial clipping | Complete | Bounded asymmetric transfer |
| 45 | Diode-style transfer | Complete | Unequal exponential positive/negative branches |
| 46 | Biasable waveshaper | Complete | Bias cancellation and asymmetry test |
| 47 | Envelope-dependent waveshaper | Complete | Per-channel attack/release envelope drive |
| 48 | Noise gate | Complete | Threshold, range, attack, hold, release, hysteresis, sidechain HP, five-state machine |
| 49 | Feed-forward compressor | Complete | Knee, ratio, makeup, RMS/peak, stereo link, sidechain HP, log gain computer |
| 50 | Peak/lookahead limiter | Complete | Ceiling, release, optional lookahead, and reported latency; true peak remains intentionally deferred by plan |
| 51 | Direct FIR convolution | Complete | Circular-history direct convolver and reference test |
| 52 | Partitioned long convolution | Complete | Uniform FFT overlap-add partitions matched to direct reference |
| 53 | Mono IR | Complete | Mono loading and processing |
| 54 | Stereo IR | Complete | Independent left/right partition sets |
| 55 | Mono-to-stereo IR | Complete | Mono duplication during preparation and convolver loading |
| 56 | IR sample-rate conversion | Complete | Deterministic linear offline resampler |
| 57 | IR trimming | Complete | Configurable leading-silence threshold |
| 58 | IR normalization | Complete | Configurable peak target |
| 59 | Async audio-file decode | Complete | JUCE adapter on `BackgroundWorker` plus real WAV integration test |
| 60 | IR DC removal | Complete | Per-channel mean removal before trim/normalization |
| 61 | Off-thread partition preparation | Complete | Inactive convolver partition build API is non-audio and worker-compatible |
| 62 | Atomic convolver swap | Complete | Atomic active index and request handoff |
| 63 | Old/new IR crossfade | Complete | Preallocated dual processing and click-boundary test |
| 64 | Bass low/high split path | Complete | `BassSplitProcessor` with optional low compression/high shaping |
| 65 | Flat phase-aligned bass recombination | Complete | Multi-frequency summed RMS error below 1.5% |
| 66 | Sample peak and RMS meters | Complete | Per-channel atomic readings with exact tests |
| 67 | Integrated/short-term loudness | Complete | BS.1770 K-weighting, 400 ms blocks, absolute/relative integrated gating, and exact 3 s short-term energy |
| 68 | Crest factor | Complete | Peak/RMS ratio transport |
| 69 | Input/output clipping indicators | Complete | Sticky atomic clip flags |
| 70 | Spectrum meter | Complete | Windowed FFT spectrum with atomic bins |
| 71 | Waveform history | Complete | Fixed atomic ring with configurable decimation |
| 72 | Gain-reduction meter | Complete | Atomic dynamics reduction field |
| 73 | Lock-free meter transport | Complete | Fixed atomics; no UI-owned buffers in processing |
| 74 | Reusable FFT wrapper | Complete | Prepared radix-2 forward/inverse transform |
| 75 | Configurable STFT window/overlap/size | Complete | Rectangular, Hann, Hamming, Blackman and arbitrary hop |
| 76 | Complex and magnitude spectral output | Complete | STFT complex output and spectrum magnitudes |
| 77 | Offline STFT reconstruction | Complete | Normalized overlap-add reconstruction test |
| 78 | Gain/frequency/Q/crossover smoothing primitives | Complete | Linear/log smoother plus bounded block-rate setters and crossover interpolation |
| 79 | Mode-switch crossfades | Complete | Generic allocation-free `ModeCrossfader` processes arbitrary old/new modes in parallel and ramps between them |
| 80 | Full deterministic stimulus set | Complete | Generated stimuli plus checked-in redistributable fixed PCM guitar/bass DI captures, palm mute, and transient fixtures |
| 81 | Expected hashes/tolerance references | Complete | Quantized FNV hash plus tolerance metrics |
| 82 | Regression error metrics | Complete | Max, RMS, spectral, latency, and DC metrics |
| 83 | Required benchmark configurations | Complete | 44.1/64, 48/128, 96/64 in mono and stereo |
| 84 | Average and p99 callback time | Complete | Recorded microsecond CSV results |
| 85 | Memory and latency benchmark fields | Complete | CSV records estimated working memory and exact algorithm latency |
| 86 | SIMD benefit measurement | Complete | Explicit SSE2 and scalar gain kernels are A/B measured; the recorded matrix shows 1.449x to 1.918x speedup |
| 87 | Benchmark every processor | Complete | 156 recorded rows cover 26 processor/acceptance categories across the required rate/block/channel matrix |
| 88 | Metering callback-cost acceptance | Complete | Dedicated baseline/overhead rows and automated acceptance keep worst measured overhead at 0.087% of callback budget |
| 89 | All processors allocation-free and stable | Complete | Global allocation instrumentation plus extreme-value stress |
| 90 | Aliasing decreases with oversampling | Complete | Measured 1x versus 8x alias component assertion |
| 91 | Regression vectors within tolerances | Complete | DSP, convolution, STFT, and deterministic hash suites pass |

## Completion evidence

- `nts_dsp_tests` verifies the completed behavior, independent references, fixed hashes, allocation safety, and numeric acceptance limits.
- `nts_dsp_benchmark_acceptance` reruns the complete matrix and fails if metering consumes 1% or more of callback budget or if SIMD does not beat scalar processing by at least 5%.
- `docs/phase-02-benchmarks.csv` stores 156 measurements across 26 processor and acceptance categories.
