# Phase 3 coverage

Coverage is scored against 100 observable requirements extracted from the Phase 3 plan. Complete
items score 1 point, partial items score 0.5, and missing items score 0.

**Current verified score: 99.5 / 100 = 99.5%.**

| # | Requirement | Status | Evidence or remaining gap |
|---:|---|:---:|---|
| 1 | Complete conventional amp graph | Complete | `AmpVoice` implements every signal block in the specified order |
| 2 | Guitar mode | Complete | Dedicated presets and graph behavior |
| 3 | Bass mode | Complete | Phase-aligned low/high graph |
| 4 | Two original topologies | Complete | Tight Modern and Vintage Bloom |
| 5 | Configurable two-to-four stage preamp | Complete | Fixed four-stage capacity and clamped active count |
| 6 | Tone-stack subsystem | Complete | Three distinct modes |
| 7 | Power-stage approximation | Complete | Feedback, nonlinear headroom and supply sag |
| 8 | Presence and resonance | Complete | Power-level-interacting post/feedback approximation |
| 9 | Cabinet subsystem | Complete | Dual IR processing and public load API |
| 10 | Bass clean blend | Complete | Compressed phase-aligned low branch |
| 11 | Preset system | Complete | Built-ins, JSON and dual-voice changes |
| 12 | Automated audio validation | Complete | `nts_amp_tests` |
| 13 | Listening-test protocol | Complete | Protocol plus synchronized click-free browser player and CSV export |
| 14 | Input peak meter | Complete | `InputCalibrator` peak reading |
| 15 | Input RMS estimator | Complete | 400 ms smoothed energy |
| 16 | Calibration target range | Complete | Per-profile low/high RMS and peak targets |
| 17 | Automatic trim suggestion | Complete | Peak-aware bounded suggestion |
| 18 | Manual trim | Complete | Smoothed automatable Input control |
| 19 | Active/passive pickup metadata | Complete | `PickupProfile` in profiles and presets |
| 20 | Model-specific calibration | Complete | Four built-in preset profiles |
| 21 | Pre-EQ low cut | Complete | Automatable biquad |
| 22 | Pre-EQ high cut | Complete | Automatable biquad |
| 23 | Tightness | Complete | Nonlinear low-cut mapping |
| 24 | Pick emphasis | Complete | Pre-distortion peaking stage |
| 25 | Optional pre low shelf | Complete | Profile-controlled shelf |
| 26 | Optional pre mid emphasis | Complete | Profile-controlled peak |
| 27 | Tightness acts before distortion | Complete | Graph order and 55 Hz rejection test |
| 28 | Stage config fields | Complete | Plan fields plus dynamic controls |
| 29 | Stage filter/gain/nonlinear/dynamic/filter/trim order | Complete | `ResponsivePreampStage` |
| 30 | Per-stage oversampling | Complete | Prepared 1x/2x/4x/8x selection |
| 31 | Serial two-to-four stage combinations | Complete | Preset-configured stage count |
| 32 | Stage reset and bounds | Complete | Determinism and bias tests |
| 33 | Envelope-driven bias shift | Complete | Bounded slow bias memory |
| 34 | Frequency-dependent saturation | Complete | High-frequency drive branch |
| 35 | Attack-sensitive gain reduction | Complete | Envelope-delta attenuation |
| 36 | Slow recovery | Complete | Configurable recovery memory |
| 37 | Asymmetric transfer | Complete | Polarity-dependent transfer |
| 38 | Configurable memory amount | Complete | Stage memory parameter |
| 39 | Passive-style three-band tone stack | Complete | Coupled coefficient model |
| 40 | Active three-band EQ | Complete | Independent active mode |
| 41 | Bass semi-parametric mids | Complete | Frequency and Q controls |
| 42 | Passive controls are coupled | Complete | Bass changes mid coefficients in test |
| 43 | Phase-inverter limited headroom | Complete | Bounded nonlinear output |
| 44 | Phase-inverter asymmetric clipping | Complete | Polarity scaling |
| 45 | Phase-inverter frequency shaping | Complete | High-pass and presence-region peak |
| 46 | Differential imbalance | Complete | Channel-dependent gain |
| 47 | Negative-feedback interaction | Complete | Delayed local feedback state |
| 48 | Master volume | Complete | Smoothed power input gain |
| 49 | Power saturation | Complete | Variable nonlinear drive |
| 50 | Damping | Complete | Supply/headroom interaction |
| 51 | Sag amount | Complete | Energy-dependent virtual supply |
| 52 | Bias character | Complete | Supply-dependent offset |
| 53 | Presence control | Complete | Automatable upper response |
| 54 | Resonance control | Complete | Automatable low damping response |
| 55 | Feedback amount | Complete | Automatable power feedback |
| 56 | Bounded sag state | Complete | Supply constrained to 0.32–1.0 |
| 57 | Sag attack/recovery constants | Complete | Independent profile parameters and recovery test |
| 58 | Cabinet IR selection | Complete | Two load slots plus built-in selectable blend |
| 59 | Dual IR blend | Complete | Parallel convolvers |
| 60 | Cabinet phase invert | Complete | Slot-B polarity option |
| 61 | Delay alignment | Complete | Preallocated 0–256 sample delay |
| 62 | Cabinet low/high cuts | Complete | Post-convolution filters |
| 63 | Mic position metadata | Complete | Name, microphone and position per slot |
| 64 | Cabinet bypass | Complete | Automatable simple-page toggle |
| 65 | Bass DI mode | Complete | Cabinet/DI blend parameter |
| 66 | Guitar stronger pre-low-cut | Complete | Original guitar profiles |
| 67 | Guitar uses more nonlinear stages | Complete | Tight guitar uses four |
| 68 | Guitar cabinet enabled by default | Complete | Built-in presets |
| 69 | Guitar post-distortion high cut | Complete | Cabinet high cut and post EQ |
| 70 | Bass Linkwitz-Riley split | Complete | Shared Phase 2 crossover |
| 71 | Low-path clean gain/compression | Complete | Configurable compressor and blend |
| 72 | Optional low saturation | Complete | Bass path option |
| 73 | Driven high path | Complete | Full amp/cabinet path |
| 74 | Phase-aligned recombination | Complete | Linkwitz-Riley sum |
| 75 | Bass crossover control | Complete | Automatable advanced control |
| 76 | Bass clean blend control | Complete | Smoothed advanced control |
| 77 | Low-frequency mono control | Complete | Continuous mono fold |
| 78 | Cabinet/DI blend | Complete | Cabinet parameter |
| 79 | Versioned preset identity/instrument/calibration | Complete | Schema v1 fields |
| 80 | Nested full preset representation | Complete | Every pre-EQ, stage, tone, phase, power, cabinet, bass, post-EQ, output and matching field is serialized and restored |
| 81 | Preset serialization test | Complete | Round-trip test |
| 82 | Original factory presets | Complete | Four topology/instrument combinations |
| 83 | Click-free preset switching | Complete | Inactive voice and crossfade test |
| 84 | Simple UI page | Complete | Nine specified controls |
| 85 | Advanced UI page | Complete | Gain, bias, filters, OS, sag, feedback, crossover, blend and alignment |
| 86 | Stage state/bias/sag/tone/crossover/preset unit tests | Complete | Focused amp and inherited DSP tests |
| 87 | Static sine and two-tone tests | Complete | Deterministic stimulus renders |
| 88 | Palm mute/chord/bass/slap transient tests | Complete | Six-source regression set |
| 89 | Gain automation test | Complete | Interpolated stage transition |
| 90 | Preset-switching audio test | Complete | Boundary discontinuity assertion |
| 91 | Loudness-matched ABX procedure | Complete | Executable BS.1770 matching, independent blinding, hashes and statistics |
| 92 | Eight listening source categories | Partial | BS.1770 ABX pack/analyzer, hashes, blinding and statistics are complete; real performances and listener responses are not present locally |
| 93 | Musically usable without ML | Complete | VST3 graph and built-in cabinet/presets run without ML assets |
| 94 | Clearly distinct guitar/bass behavior | Complete | Deterministic hash comparison |
| 95 | Bass fundamental survives heavy drive | Complete | 55 Hz magnitude acceptance test |
| 96 | Preset changes are click-free | Complete | Parallel graph crossfade |
| 97 | User controls are automatable and stateful | Complete | APVTS and wrapper persistence test |
| 98 | CPU within target at 4x | Complete | Stereo graph stays below 50% of callback budget at 48/128 |
| 99 | Deterministic offline render | Complete | Same implementation and hash equality across six sources |
| 100 | Parameters exposed for later optimization | Complete | Typed public graph/stage/profile structs |

## Remaining work for 100%

Add user-recorded, rights-cleared DI takes and completed human ABX result records for all eight listening
categories. `phase-03-abx-tools.md` provides the executable path and validation rules.
