#include "nts/pedals/PedalBoard.h"

#include <algorithm>
#include <cmath>

namespace nts::pedals
{
namespace
{
/// The 0-to-10 controls, as a 0-to-1 fraction.
[[nodiscard]] float controlToUnit(float control) noexcept
{
    return std::clamp(control, 0.0f, 10.0f) * 0.1f;
}

/// Geometric rather than linear, so a tone knob sounds even across its travel.
[[nodiscard]] double sweep(double minimum, double maximum, float unit) noexcept
{
    return minimum * std::pow(maximum / minimum, static_cast<double>(unit));
}
} // namespace

const PedalSlot::Voicing& PedalSlot::voicingFor(PedalKind kind) noexcept
{
    // Ordered to match PedalKind. The numbers are voicing choices, not measurements of any
    // particular box: what makes these read as different pedals is the combination of where
    // the signal is filtered before the clipper, how hard it is driven, and the curve.
    static constexpr std::array<Voicing, kindCount> table { {
        // none: never reached -- a slot set to none returns before it looks at a voicing.
        { 20.0, 0.0f, 0.0f, dsp::Waveshape::hyperbolicTangent, 0.0f, 20.0, 20.0, 0.0f, false },
        // boost: full-range, barely any curve, tone opens all the way up. A clean level lift
        // into the amplifier's own front end, which is what most of a pedalboard is for.
        { 30.0, 0.0f, 24.0f, dsp::Waveshape::hyperbolicTangent, 0.0f, 2000.0, 16000.0, -1.0f, false },
        // overdrive: clips only what is above 720 Hz and sums it back with the untouched
        // input. That parallel path is the mid hump, and it is why this stays articulate
        // where the distortion below does not.
        { 720.0, 6.0f, 34.0f, dsp::Waveshape::diode, 0.0f, 1200.0, 6000.0, -7.0f, true },
        // distortion: everything through a soft clipper at high gain, with the tone control
        // as the only thing standing between it and a fizz.
        { 100.0, 10.0f, 42.0f, dsp::Waveshape::softClip, 0.0f, 800.0, 8000.0, -13.0f, false },
        // fuzz: highest gain, asymmetric, and high-passed hard at the input because a fuzz
        // fed full low end collapses into a splutter rather than a note.
        { 60.0, 20.0f, 52.0f, dsp::Waveshape::asymmetricPolynomial, 0.18f, 700.0, 6000.0, -11.0f, false },
        // compressor: the shaping fields are unused; it runs the dynamics processor instead,
        // and keeps only the tone control, which stays open by default.
        { 20.0, 0.0f, 0.0f, dsp::Waveshape::hyperbolicTangent, 0.0f, 2000.0, 18000.0, 0.0f, false },
        // neural capture: the model is the voicing. Drive becomes the level going into it,
        // which a capture is genuinely sensitive to, and tone a post-model roll-off that is
        // effectively open at the top of its travel.
        //
        // -6 to +24 rather than -12 to +12. The old range put Drive 5 at exactly 0 dB, and with
        // no input compensation -- see configureNeural, which nothing was calling -- that was
        // unity into a model expecting a specific level, so the knob's whole lower half did
        // nothing audible and only the very top produced any character. The other kinds span 24
        // to 32 dB into their clippers; this was the one whose drive control could not drive
        // anything. Compensation now lands the calibrated level near the middle of the travel
        // and the top pushes 24 dB past it.
        //
        // What this cannot fix: a `.nam` pedal capture is a snapshot at one knob position. If it
        // was captured at low drive, no amount of input gain reproduces a high-drive pedal. That
        // is the format, not the range.
        { 20.0, -6.0f, 24.0f, dsp::Waveshape::hyperbolicTangent, 0.0f, 2000.0, 20000.0, 0.0f, false },
    } };
    return table[static_cast<std::size_t>(kind)];
}

void PedalSlot::retarget(dsp::SmoothedParameter& parameter, float& cached, float value) noexcept
{
    if (cached == value) return;
    cached = value;
    parameter.setTarget(value);
}

float PedalSlot::driveTarget() const noexcept
{
    const auto& voicing = voicingFor(activeKind);
    return dsp::dbToLinear(voicing.minDriveDb
                           + (voicing.maxDriveDb - voicing.minDriveDb) * controlToUnit(requested.drive));
}

float PedalSlot::levelTarget() const noexcept
{
    return dsp::dbToLinear(std::clamp(requested.levelDb, -36.0f, 36.0f) + voicingFor(activeKind).trimDb);
}

float PedalSlot::mixTarget() const noexcept
{
    const auto engaged = ! switching && ! requested.bypassed && activeKind != PedalKind::none;
    return engaged ? std::clamp(requested.mix, 0.0f, 100.0f) * 0.01f : 0.0f;
}

void PedalSlot::prepare(const dsp::ProcessSpec& newSpec)
{
    spec = newSpec;
    spec.sampleRate = std::max(1.0, spec.sampleRate);
    spec.maximumBlockSize = std::max<std::size_t>(1, spec.maximumBlockSize);
    spec.channels = std::clamp(spec.channels, std::size_t { 1 }, dsp::maximumChannels);

    inputFilter.prepare(spec);
    toneFilter.prepare(spec);
    compressor.prepare(spec);
    neural.prepare(spec.sampleRate, spec.maximumBlockSize, spec.channels);

    driveGain.prepare(spec.sampleRate, 20.0, dsp::SmoothingMode::logarithmic);
    outputGain.prepare(spec.sampleRate, 20.0, dsp::SmoothingMode::logarithmic);
    // Longer than the gains: this ramp also carries a pedal being switched in or out, and a
    // footswitch that takes a couple of dozen milliseconds reads as instant while a step reads
    // as a click.
    wetMix.prepare(spec.sampleRate, 30.0);

    for (std::size_t channel = 0; channel < dsp::maximumChannels; ++channel)
    {
        dryScratch[channel].assign(spec.maximumBlockSize, 0.0f);
        bandScratch[channel].assign(spec.maximumBlockSize, 0.0f);
        bandPointers[channel] = bandScratch[channel].data();
    }
    reset();
}

void PedalSlot::reset() noexcept
{
    activeKind = requested.kind;
    switching = false;
    resetVoice();
    cachedDrive = driveTarget();
    cachedLevel = levelTarget();
    cachedMix = mixTarget();
    driveGain.reset(cachedDrive);
    outputGain.reset(cachedLevel);
    wetMix.reset(cachedMix);
}

void PedalSlot::resetVoice() noexcept
{
    inputFilter.reset();
    toneFilter.reset();
    compressor.reset();
    neural.reset();
    // Forces updateFilters to recompute: after a reset the filters hold their default
    // pass-through coefficients, whatever the tone control last said.
    filteredTone = -1.0f;
}

void PedalSlot::updateFilters() noexcept
{
    const auto tone = std::clamp(requested.tone, 0.0f, 10.0f);
    const auto stale = filteredTone < 0.0f;
    if (! stale && activeKind == filteredKind && tone == filteredTone) return;

    const auto& voicing = voicingFor(activeKind);
    // Straight in after a reset, glided otherwise: gliding from the default pass-through
    // coefficients would let a block of unfiltered signal through on the way past.
    const auto interpolation = stale ? std::size_t {} : spec.maximumBlockSize;
    inputFilter.setCoefficients(
        dsp::BiquadCoefficients::make(dsp::FilterType::highPass, spec.sampleRate,
                                      dsp::clampFrequency(voicing.inputHighPassHz, spec.sampleRate)),
        interpolation);
    toneFilter.setCoefficients(
        dsp::BiquadCoefficients::make(
            dsp::FilterType::lowPass, spec.sampleRate,
            dsp::clampFrequency(sweep(voicing.toneMinHz, voicing.toneMaxHz, controlToUnit(tone)),
                                spec.sampleRate)),
        interpolation);
    filteredKind = activeKind;
    filteredTone = tone;
}

void PedalSlot::setParameters(const PedalParameters& parameters) noexcept
{
    requested = parameters;
    requested.kind = static_cast<std::size_t>(parameters.kind) < kindCount ? parameters.kind
                                                                          : PedalKind::none;
    configureNeural();
}

void PedalSlot::configureNeural() noexcept
{
    /* Calibrate the model host, which nothing was doing.

       `NeuralAmpProcessor` carries an `InputCalibrationMonitor` and knows the `expectedInputRmsDb`
       its model was staged with, but compensation is opt-in and only the amplifier's instance was
       ever opted in -- the four pedal slots each own one and none of them called this. A capture
       therefore ran at whatever level the chain happened to hand it, with no reference to the level
       it was made at, which is most of why a neural pedal at Drive 5 sounded like nothing was
       there. See the drive range in `voicingFor` for the other half.

       Controls likewise: a distilled conditioned model has five of them, and left at the zeroed
       default it runs at whatever mid-scale means for that capture regardless of the slot's own
       knobs. Drive and Tone are the two a pedal actually has, so they are the two mapped, in the
       same -1..1 convention the amplifier uses. A plain `.nam` capture declares no conditioning
       and ignores the vector, so this costs it nothing.

       Called from setParameters rather than from process because both are message-thread-safe
       stores of atomics and this way process stays free of anything it does not need. */
    neural.setInputCompensationEnabled(true);
    const std::array<float, ml::NeuralAmpProcessor::controlCount> controls {
        std::clamp(requested.drive * 0.2f - 1.0f, -1.0f, 1.0f),
        std::clamp(requested.tone * 0.2f - 1.0f, -1.0f, 1.0f),
        0.0f, 0.0f, 0.0f
    };
    neural.setControls(controls);
}

void PedalSlot::process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept
{
    const auto count = std::min({ channelCount, spec.channels, dsp::maximumChannels });
    if (count == 0 || samples == 0 || samples > spec.maximumBlockSize) return;

    // A kind change rides the blend down to dry first and only then swaps, so the outgoing
    // pedal fades out instead of being cut out from under the signal.
    if (requested.kind != activeKind && ! switching)
        switching = true;
    if (switching && ! wetMix.isSmoothing() && wetMix.value() <= 0.0f)
    {
        activeKind = requested.kind;
        resetVoice();
        switching = false;
        // The incoming pedal starts at its own settings rather than ramping from the
        // outgoing one's, which is free: the blend is at dry, so nothing is audible yet.
        cachedDrive = driveTarget();
        cachedLevel = levelTarget();
        driveGain.reset(cachedDrive);
        outputGain.reset(cachedLevel);
    }
    retarget(wetMix, cachedMix, mixTarget());

    // Either there is nothing to blend in, or the blend has already reached dry and is
    // staying there. Both leave the buffer untouched, and the second is the default rig, so
    // it has to cost nothing at all -- which is why the ramp is advanced only when one is
    // actually running. A ramp still in flight has to advance here or it would never retire.
    if (activeKind == PedalKind::none || (cachedMix <= 0.0f && ! wetMix.isSmoothing()))
    {
        if (wetMix.isSmoothing())
            for (std::size_t sample = 0; sample < samples; ++sample)
                static_cast<void>(wetMix.next());
        return;
    }

    retarget(driveGain, cachedDrive, driveTarget());
    retarget(outputGain, cachedLevel, levelTarget());
    updateFilters();

    for (std::size_t channel = 0; channel < count; ++channel)
        std::copy_n(channels[channel], samples, dryScratch[channel].begin());

    switch (activeKind)
    {
        case PedalKind::compressor: renderCompressor(channels, count, samples); break;
        case PedalKind::neuralCapture: renderNeural(channels, count, samples); break;
        default: renderShaped(channels, count, samples); break;
    }

    // Level rides the wet signal alone, so a slot at zero mix is exactly transparent no
    // matter where its Level knob happens to be sitting.
    for (std::size_t sample = 0; sample < samples; ++sample)
    {
        const auto level = outputGain.next();
        const auto mix = wetMix.next();
        for (std::size_t channel = 0; channel < count; ++channel)
        {
            const auto dry = dryScratch[channel][sample];
            const auto blended = dry + mix * (channels[channel][sample] * level - dry);
            channels[channel][sample] = std::isfinite(blended) ? blended : dry;
        }
    }
}

void PedalSlot::renderShaped(float* const* channels, std::size_t count, std::size_t samples) noexcept
{
    const auto& voicing = voicingFor(activeKind);
    if (voicing.parallelClip)
    {
        for (std::size_t channel = 0; channel < count; ++channel)
            std::copy_n(dryScratch[channel].begin(), samples, bandScratch[channel].begin());
        inputFilter.process(bandPointers.data(), count, samples);
        for (std::size_t sample = 0; sample < samples; ++sample)
        {
            const auto gain = driveGain.next();
            for (std::size_t channel = 0; channel < count; ++channel)
                channels[channel][sample] =
                    dryScratch[channel][sample]
                    + dsp::shapeSample(bandScratch[channel][sample], voicing.shape, gain);
        }
    }
    else
    {
        inputFilter.process(channels, count, samples);
        const auto bias = voicing.bias;
        for (std::size_t sample = 0; sample < samples; ++sample)
        {
            const auto gain = driveGain.next();
            // Cancels the DC that a biased curve sits on. Only a kind that actually uses
            // bias pays for the second evaluation.
            const auto offset = bias == 0.0f ? 0.0f : dsp::shapeSample(bias, voicing.shape, gain);
            for (std::size_t channel = 0; channel < count; ++channel)
                channels[channel][sample] =
                    dsp::shapeSample(channels[channel][sample] + bias, voicing.shape, gain) - offset;
        }
    }
    toneFilter.process(channels, count, samples);
}

void PedalSlot::renderCompressor(float* const* channels, std::size_t count, std::size_t samples) noexcept
{
    const auto amount = controlToUnit(requested.drive);
    dsp::CompressorParameters parameters;
    // One knob has to move threshold and ratio together or the bottom half of its travel
    // does nothing: 4:1 at -3 dB is inaudible on a guitar.
    parameters.thresholdDb = -3.0f - 33.0f * amount;
    parameters.ratio = 1.5f + 6.5f * amount;
    parameters.kneeDb = 8.0f;
    parameters.attackMs = 12.0;
    parameters.releaseMs = 160.0;
    // Auto make-up, so squashing harder does not simply get quieter and leave the Level knob
    // chasing it. Half the theoretical reduction, which is about what sounds level-matched.
    parameters.makeupDb = -parameters.thresholdDb * (1.0f - 1.0f / parameters.ratio) * 0.5f;
    parameters.detector = dsp::DetectorMode::rms;
    parameters.stereoLink = true;
    // A compressor that tracks the low end pumps on every root note.
    parameters.sidechainHighPassHz = 120.0;
    compressor.setParameters(parameters);
    compressor.process(channels, count, samples);
    toneFilter.process(channels, count, samples);
}

void PedalSlot::renderNeural(float* const* channels, std::size_t count, std::size_t samples) noexcept
{
    /* Drive is a pre-gain into the model, so it applies only when there is a model to drive.

       `NeuralAmpProcessor::process` already returns without touching the buffer when nothing is
       staged, so the model was never the problem -- but applying the pre-gain with nothing there
       left an empty slot amplifying. That was invisible only while Drive 5 happened to map to
       exactly 0 dB; widening the range so the knob can actually drive a capture turned the same
       code into a 9 dB boost from a slot the user had loaded nothing into.

       `neural.process` is still called either way, and that is not optional: staging publishes a
       model to a *pending* slot and `process` is what promotes it to active. Returning early here
       instead -- which is what this did first -- meant a freshly loaded capture was never promoted,
       `hasActiveModel` stayed false forever, and the slot sat silent holding a model it had been
       given. The unit test for the empty-slot case could not see that, because it never staged
       one; the wrapper integration test did.

       The smoother is advanced regardless, because a ramp left in flight never retires and
       `retarget` decides whether the slot has settled from exactly that. */
    const auto driveModel = neural.hasActiveModel();
    for (std::size_t sample = 0; sample < samples; ++sample)
    {
        const auto gain = driveGain.next();
        if (! driveModel) continue;
        for (std::size_t channel = 0; channel < count; ++channel)
            channels[channel][sample] *= gain;
    }
    neural.process(channels, count, samples);
    toneFilter.process(channels, count, samples);
}

void PedalBoard::prepare(const dsp::ProcessSpec& spec)
{
    for (auto& pedal : slots) pedal.prepare(spec);
}

void PedalBoard::reset() noexcept
{
    for (auto& pedal : slots) pedal.reset();
}

void PedalBoard::setParameters(std::size_t slot, const PedalParameters& parameters) noexcept
{
    if (slot < slots.size()) slots[slot].setParameters(parameters);
}

void PedalBoard::process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept
{
    for (auto& pedal : slots) pedal.process(channels, channelCount, samples);
}

bool PedalBoard::anyActive() const noexcept
{
    return std::any_of(slots.begin(), slots.end(), [](const PedalSlot& pedal) { return pedal.active(); });
}
} // namespace nts::pedals
