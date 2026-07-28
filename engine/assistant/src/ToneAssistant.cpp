#include <nts/assistant/ToneAssistant.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>

namespace nts::assistant
{
namespace
{
float clamp01(float value) noexcept { return std::clamp(value, 0.0f, 1.0f); }
float decibels(float value) noexcept { return 20.0f * std::log10(std::max(value, 1.0e-9f)); }

constexpr std::array schema {
    ParameterRange { "input", -60.0f, 24.0f, 6.0f }, ParameterRange { "output", -60.0f, 24.0f, 6.0f },
    ParameterRange { "gain", 0.0f, 10.0f, 2.0f }, ParameterRange { "bass", 0.0f, 10.0f, 2.0f },
    ParameterRange { "mid", 0.0f, 10.0f, 2.0f }, ParameterRange { "treble", 0.0f, 10.0f, 2.0f },
    ParameterRange { "presence", 0.0f, 10.0f, 2.0f }, ParameterRange { "resonance", 0.0f, 10.0f, 2.0f },
    ParameterRange { "master", -60.0f, 12.0f, 6.0f }, ParameterRange { "cabinet", 0.0f, 1.0f, 1.0f },
    ParameterRange { "stage1", -12.0f, 42.0f, 6.0f }, ParameterRange { "stage2", -12.0f, 42.0f, 6.0f },
    ParameterRange { "stage3", -12.0f, 42.0f, 6.0f }, ParameterRange { "stage4", -12.0f, 42.0f, 6.0f },
    ParameterRange { "bias", -0.8f, 0.8f, 0.2f }, ParameterRange { "lowCut", 20.0f, 500.0f, 80.0f },
    ParameterRange { "highCut", 3000.0f, 22000.0f, 2500.0f }, ParameterRange { "oversampling", 0.0f, 3.0f, 3.0f },
    ParameterRange { "sag", 0.0f, 100.0f, 20.0f }, ParameterRange { "feedback", 0.0f, 100.0f, 20.0f },
    ParameterRange { "crossover", 60.0f, 500.0f, 100.0f }, ParameterRange { "cleanBlend", 0.0f, 100.0f, 20.0f },
    ParameterRange { "cabinetAlignment", 0.0f, 256.0f, 64.0f }, ParameterRange { "tightness", 0.0f, 10.0f, 2.0f },
    ParameterRange { "pickEmphasis", -12.0f, 12.0f, 3.0f }
};

const ParameterRange* rangeFor(std::string_view id) noexcept
{
    const auto found = std::find_if(schema.begin(), schema.end(), [id](const auto& value) { return value.id == id; });
    return found == schema.end() ? nullptr : &*found;
}

float valueOf(std::span<const ParameterValue> values, std::string_view id, float fallback = 0.0f) noexcept
{
    const auto found = std::find_if(values.begin(), values.end(), [id](const auto& value) { return value.id == id; });
    return found == values.end() ? fallback : found->value;
}

void setValue(std::vector<ParameterValue>& values, std::string_view id, float value)
{
    const auto found = std::find_if(values.begin(), values.end(), [id](const auto& item) { return item.id == id; });
    if (found != values.end()) found->value = value;
}

ParameterChange change(std::span<const ParameterValue> values, std::string id, float target)
{
    const auto from = valueOf(values, id);
    const auto* range = rangeFor(id);
    const auto bounded = range == nullptr ? target : std::clamp(target, range->minimum, range->maximum);
    return { std::move(id), from, bounded };
}

Recommendation makeRecommendation(std::string id, Problem problem, std::string diagnosis,
                                  std::string beginner, std::string advanced, float confidence,
                                  std::vector<std::string> evidence,
                                  std::vector<ParameterChange> changes,
                                  std::string effect, bool measurable = true)
{
    Recommendation result;
    result.actionId = std::move(id); result.problem = problem; result.diagnosis = std::move(diagnosis);
    result.beginnerExplanation = std::move(beginner); result.advancedExplanation = std::move(advanced);
    result.confidence = clamp01(confidence); result.evidence = std::move(evidence);
    result.changes = std::move(changes); result.expectedEffect = std::move(effect);
    result.measurableProblem = measurable; return result;
}

bool contains(const std::vector<std::string>& values, const std::string& value)
{ return std::find(values.begin(), values.end(), value) != values.end(); }

void addGoal(std::vector<Recommendation>& recommendations, const AssistantInput& input)
{
    const auto& p = input.parameters;
    const auto add = [&recommendations](Recommendation value) { recommendations.push_back(std::move(value)); };
    switch (input.goal)
    {
        case Goal::tightRhythm:
            add(makeRecommendation("goal-tight-rhythm", Problem::goalAdjustment, "Tight rhythm voicing",
                "Make the low end tighter and the pick attack clearer.",
                "Raise the pre-distortion high-pass and tightness macro while slightly reducing aggregate gain.",
                0.76f, { "user-selected style goal" },
                { change(p, "lowCut", valueOf(p, "lowCut", 80.0f) + 35.0f),
                  change(p, "tightness", valueOf(p, "tightness", 5.0f) + 1.2f),
                  change(p, "gain", valueOf(p, "gain", 5.0f) - 0.5f) },
                "Faster palm-muted recovery with less low-frequency smear.", false)); break;
        case Goal::cleanBassSupport:
            add(makeRecommendation("goal-clean-bass", Problem::goalAdjustment, "Clean bass support",
                "Keep the bass foundation clean underneath the driven tone.",
                "Increase the phase-aligned low-band blend and move its crossover slightly upward.",
                0.78f, { "bass support goal" },
                { change(p, "cleanBlend", valueOf(p, "cleanBlend", 50.0f) + 15.0f),
                  change(p, "crossover", valueOf(p, "crossover", 180.0f) + 30.0f) },
                "More stable fundamentals without removing upper-band aggression.", false)); break;
        case Goal::aggressivePickedBass:
            add(makeRecommendation("goal-picked-bass", Problem::goalAdjustment, "Aggressive picked bass",
                "Bring the pick forward without sacrificing the low end.",
                "Increase pick emphasis and high-band drive while retaining the clean low split.",
                0.75f, { "picked-bass goal" },
                { change(p, "pickEmphasis", valueOf(p, "pickEmphasis") + 2.0f),
                  change(p, "gain", valueOf(p, "gain", 5.0f) + 0.7f),
                  change(p, "cleanBlend", valueOf(p, "cleanBlend", 50.0f) + 8.0f) },
                "More attack and grind with preserved bass weight.", false)); break;
        case Goal::smoothLead:
            add(makeRecommendation("goal-smooth-lead", Problem::goalAdjustment, "Smooth lead voicing",
                "Round off the sharp edge and make single notes feel fuller.",
                "Reduce presence and treble while adding a bounded mid emphasis.", 0.74f,
                { "smooth-lead goal" },
                { change(p, "presence", valueOf(p, "presence", 5.0f) - 1.0f),
                  change(p, "treble", valueOf(p, "treble", 5.0f) - 0.7f),
                  change(p, "mid", valueOf(p, "mid", 5.0f) + 1.0f) },
                "Less bite with more sustained midrange focus.", false)); break;
        case Goal::warmClean:
            add(makeRecommendation("goal-warm-clean", Problem::goalAdjustment, "Warm clean voicing",
                "Use less breakup and a softer top end.",
                "Reduce preamp gain and treble, then retain a modest bass shelf.", 0.77f,
                { "warm-clean goal" },
                { change(p, "gain", valueOf(p, "gain", 5.0f) - 1.5f),
                  change(p, "treble", valueOf(p, "treble", 5.0f) - 0.8f),
                  change(p, "bass", valueOf(p, "bass", 5.0f) + 0.6f) },
                "Cleaner transients and a warmer tonal balance.", false)); break;
        case Goal::lessHarsh:
            add(makeRecommendation("goal-less-harsh", Problem::goalAdjustment, "Reduce harshness",
                "Soften the sharp upper-mid and high-frequency edge.",
                "Lower presence and cabinet high-cut with a small treble reduction.", 0.80f,
                { "less-harsh goal" },
                { change(p, "presence", valueOf(p, "presence", 5.0f) - 1.2f),
                  change(p, "highCut", valueOf(p, "highCut", 12000.0f) - 1400.0f),
                  change(p, "treble", valueOf(p, "treble", 5.0f) - 0.6f) },
                "A smoother top end without a blanket reduction in level.", false)); break;
        case Goal::preserveLowEnd:
            add(makeRecommendation("goal-preserve-low", Problem::goalAdjustment, "Preserve low end",
                "Keep more fundamental weight through the distorted path.",
                "Lower the pre high-pass and increase resonance or bass clean blend within safe bounds.",
                0.77f, { "low-end preservation goal" },
                { change(p, "lowCut", valueOf(p, "lowCut", 80.0f) - 25.0f),
                  change(p, "cleanBlend", valueOf(p, "cleanBlend", 50.0f) + 12.0f),
                  change(p, "resonance", valueOf(p, "resonance", 5.0f) + 0.7f) },
                "More fundamental energy while retaining controlled distortion.", false)); break;
        case Goal::matchReference:
            if (input.tone && input.reference)
            {
                const auto brightnessDelta = input.reference->report.brightness.value - input.tone->report.brightness.value;
                const auto gainDelta = input.reference->report.gain.value - input.tone->report.gain.value;
                add(makeRecommendation("goal-match-reference", Problem::goalAdjustment, "Move toward reference tone",
                    "Adjust brightness and gain toward the selected reference.",
                    "Descriptor deltas initialize a bounded tone-stack/gain correction; this does not copy pitch or timing.",
                    0.70f + input.reference->report.confidence.aggregate * 0.2f,
                    { "brightness delta " + std::to_string(brightnessDelta), "gain delta " + std::to_string(gainDelta) },
                    { change(p, "treble", valueOf(p, "treble", 5.0f) + brightnessDelta * 2.0f),
                      change(p, "gain", valueOf(p, "gain", 5.0f) + gainDelta * 2.0f) },
                    "Closer tone descriptors while preserving the player's performance.", false));
            }
            break;
        case Goal::diagnose: break;
    }
}
} // namespace

std::span<const ParameterRange> parameterSchema() noexcept { return schema; }

void SummaryAccumulator::add(const AudioSummaryFrame& frame) noexcept
{
    const auto weight = state.frames == 0 ? 1.0f : 0.15f;
    const auto mix = [weight](float oldValue, float newValue) { return oldValue + weight * (newValue - oldValue); };
    state.inputPeak = std::max(frame.inputPeak, state.inputPeak * 0.92f);
    state.outputPeak = std::max(frame.outputPeak, state.outputPeak * 0.92f);
    state.inputRms = mix(state.inputRms, frame.inputRms); state.outputRms = mix(state.outputRms, frame.outputRms);
    state.inputCrestDb = decibels(state.inputPeak / std::max(state.inputRms, 1.0e-8f));
    state.outputCrestDb = decibels(state.outputPeak / std::max(state.outputRms, 1.0e-8f));
    if (frame.inputRms > 1.0e-6f) state.noiseFloorRms = std::min(state.noiseFloorRms, frame.inputRms);
    state.zeroCrossingRate = mix(state.zeroCrossingRate, frame.zeroCrossingRate);
    state.channelCorrelation = mix(state.channelCorrelation, frame.channelCorrelation);
    const auto sampleCount = std::max<std::uint32_t>(1, frame.samples);
    state.inputClipRate = mix(state.inputClipRate, static_cast<float>(frame.inputClipped) / sampleCount);
    state.outputClipRate = mix(state.outputClipRate, static_cast<float>(frame.outputClipped) / sampleCount);
    if (previousInputRms > 0.01f && frame.inputRms < previousInputRms * 0.08f)
        state.intermittentRatio = mix(state.intermittentRatio, 1.0f);
    else state.intermittentRatio = mix(state.intermittentRatio, 0.0f);
    previousInputRms = frame.inputRms; state.latencySamples = frame.latencySamples;
    state.sampleRate = frame.sampleRate; ++state.frames;
}

SignalObservation SummaryAccumulator::observation() const noexcept { return state; }
void SummaryAccumulator::reset() noexcept { state = {}; state.noiseFloorRms = 1.0f; previousInputRms = 0.0f; }

std::vector<Recommendation> RecommendationEngine::evaluate(const AssistantInput& input,
                                                          const PreferenceProfile& preferences,
                                                          std::size_t maximumResults) const
{
    std::vector<Recommendation> result;
    const auto& s = input.signal; const auto& p = input.parameters;
    const auto add = [&result](Recommendation recommendation) { result.push_back(std::move(recommendation)); };
    if (s.frames > 0 && (s.inputPeak > 0.985f || s.inputClipRate > 0.0005f))
        add(makeRecommendation("reduce-input-clipping", Problem::inputClipping, "Input clipping",
            "Your input is clipping before the amp. Turn it down slightly.",
            "Peak exceeds -0.13 dBFS or clipped-sample rate exceeds 0.05%; reduce input trim before nonlinear stages.",
            clamp01(0.75f + s.inputClipRate * 20.0f), { "input peak " + std::to_string(s.inputPeak),
            "clip rate " + std::to_string(s.inputClipRate) },
            { change(p, "input", valueOf(p, "input") - 3.0f) }, "Restores input headroom and reduces hard clipping."));
    if (s.frames > 3 && decibels(s.inputRms) < -46.0f)
        add(makeRecommendation("raise-quiet-input", Problem::inputTooQuiet, "Input level is too quiet",
            "The guitar or bass is reaching the amp too quietly.",
            "Smoothed input RMS is below -46 dBFS; add bounded input trim rather than compensating with noisy late-stage gain.",
            clamp01((-42.0f - decibels(s.inputRms)) / 18.0f), { "input RMS " + std::to_string(decibels(s.inputRms)) + " dBFS" },
            { change(p, "input", valueOf(p, "input") + 4.0f) }, "Improves calibration and signal-to-noise ratio."));
    if (s.outputPeak > 0.985f || s.outputClipRate > 0.0005f)
        add(makeRecommendation("reduce-output-clipping", Problem::outputClipping, "Output clipping",
            "The final output is clipping. Lower the output without changing the tone.",
            "Post-engine peak or clipped-sample rate exceeds the safe ceiling; reduce only final output gain.",
            clamp01(0.78f + s.outputClipRate * 20.0f), { "output peak " + std::to_string(s.outputPeak) },
            { change(p, "output", valueOf(p, "output") - 3.0f) }, "Creates output headroom without changing gain structure."));
    if (s.intermittentRatio > 0.25f)
        add(makeRecommendation("check-intermittent-input", Problem::intermittentSignal, "Intermittent input signal",
            "The signal is dropping suddenly. Check the cable, jack, or wireless link.",
            "Successive summary frames show repeated RMS drops greater than 22 dB; no parameter mutation is proposed.",
            clamp01(s.intermittentRatio + 0.45f), { "dropout ratio " + std::to_string(s.intermittentRatio) }, {},
            "Fixing the physical signal path prevents unpredictable gating and gain changes."));
    if (s.frames > 20 && s.noiseFloorRms > 0.008f && s.zeroCrossingRate > 0.18f
        && s.inputCrestDb < 7.0f)
        add(makeRecommendation("reduce-input-noise", Problem::excessiveNoise, "Excessive input noise",
            "The input has a steady noisy floor. Check shielding and gain staging before adding more gain.",
            "Minimum observed RMS, high zero-crossing density, and low crest suggest broadband noise; reduce preamp gain only as a preview.",
            clamp01(0.52f + s.noiseFloorRms * 8.0f),
            { "noise-floor RMS " + std::to_string(s.noiseFloorRms),
              "zero-crossing rate " + std::to_string(s.zeroCrossingRate) },
            { change(p, "gain", valueOf(p, "gain", 5.0f) - 0.6f) },
            "Reduces noise amplification while the physical source is checked."));
    if (input.rig.pickup == nts::amp::PickupProfile::active && s.inputPeak > 0.9f && s.inputCrestDb < 4.0f)
        add(makeRecommendation("active-pickup-headroom", Problem::activePickupOverload, "Active pickup overload risk",
            "The active pickup signal is close to overload with little peak headroom.",
            "Active-pickup profile plus peak above -0.9 dBFS and crest below 4 dB indicates sustained front-end overload.",
            0.79f, { "active pickup profile", "input peak " + std::to_string(s.inputPeak) },
            { change(p, "input", valueOf(p, "input") - 3.0f) }, "Restores headroom before the first nonlinear stage."));
    if (s.latencySamples > 128)
        add(makeRecommendation("reduce-latency", Problem::excessiveLatency, "Excessive processing latency",
            "The current quality setting may feel delayed while playing.",
            "Reported processing latency exceeds 128 samples; preview 1x oversampling for tracking.",
            clamp01(static_cast<float>(s.latencySamples) / 512.0f), { std::to_string(s.latencySamples) + " reported samples" },
            { change(p, "oversampling", 0.0f) }, "Reduces algorithmic latency while trading some anti-aliasing."));

    if (input.tone && input.tone->success)
    {
        const auto& tone = *input.tone; const auto& report = tone.report; const auto& f = tone.features;
        if (f.spectral.lowMidBuildup > 0.62f && f.spectral.spectralCentroidHz < 1800.0f)
            add(makeRecommendation("reduce-mud", Problem::muddyLowMids, "Muddy low-mid buildup",
                "The low mids are masking note definition.",
                "Elevated 180–400 Hz proxy plus low spectral centroid; reduce energy before distortion and retain post-EQ bass.",
                clamp01(0.45f + f.spectral.lowMidBuildup * 0.45f),
                { "low-mid buildup " + std::to_string(f.spectral.lowMidBuildup),
                  "centroid " + std::to_string(f.spectral.spectralCentroidHz) + " Hz" },
                { change(p, "lowCut", valueOf(p, "lowCut", 80.0f) + 30.0f),
                  change(p, "mid", valueOf(p, "mid", 5.0f) - 0.8f),
                  change(p, "gain", valueOf(p, "gain", 5.0f) - 0.4f) },
                "Clearer note separation with less pre-distortion low-mid masking."));
        if (f.spectral.lowFrequencyExtension < 0.22f)
            add(makeRecommendation("restore-fundamental", Problem::thinFundamentals, "Thin fundamentals",
                "The tone is missing some body and fundamental weight.",
                "Low-frequency extension is below the instrument-context threshold; lower the pre high-pass and add bounded bass.",
                clamp01(0.72f - f.spectral.lowFrequencyExtension),
                { "low extension " + std::to_string(f.spectral.lowFrequencyExtension) },
                { change(p, "lowCut", valueOf(p, "lowCut", 80.0f) - 20.0f),
                  change(p, "bass", valueOf(p, "bass", 5.0f) + 0.8f) }, "Restores body without a large output-level change."));
        if (f.spectral.lowFrequencyExtension > 0.78f && f.spectral.lowMidBuildup > 0.62f)
            add(makeRecommendation("control-boom", Problem::boomyLows, "Boomy low end",
                "The low end is overpowering the note definition.",
                "Low extension and low-mid buildup are both elevated; reduce resonance and bass after checking room monitoring.",
                clamp01((f.spectral.lowFrequencyExtension + f.spectral.lowMidBuildup) * 0.5f),
                { "low extension " + std::to_string(f.spectral.lowFrequencyExtension),
                  "low-mid buildup " + std::to_string(f.spectral.lowMidBuildup) },
                { change(p, "bass", valueOf(p, "bass", 5.0f) - 0.8f),
                  change(p, "resonance", valueOf(p, "resonance", 5.0f) - 1.0f) },
                "Tighter lows with less speaker-like bloom."));
        if (f.spectral.upperMidAttack > 0.72f)
            add(makeRecommendation("soften-upper-mids", Problem::harshUpperMids, "Harsh upper mids",
                "The attack region is sharper than it needs to be.",
                "Upper-mid attack proxy is elevated; reduce presence and treble instead of broadly lowering volume.",
                clamp01(f.spectral.upperMidAttack), { "upper-mid attack " + std::to_string(f.spectral.upperMidAttack) },
                { change(p, "presence", valueOf(p, "presence", 5.0f) - 1.0f),
                  change(p, "treble", valueOf(p, "treble", 5.0f) - 0.7f) }, "Smoother pick edge with mids and fundamentals retained."));
        if (f.spectral.highFrequencyRolloff < 0.22f && report.gain.value > 0.55f)
            add(makeRecommendation("reduce-fizz", Problem::fizzyHighs, "Fizzy high-frequency distortion",
                "There is excess high-frequency fizz above the useful attack range.",
                "High-frequency rolloff is weak while gain is elevated; lower cabinet high-cut and a small amount of gain.",
                clamp01(0.65f + report.gain.value * 0.2f), { "high rolloff " + std::to_string(f.spectral.highFrequencyRolloff),
                  "gain descriptor " + std::to_string(report.gain.value) },
                { change(p, "highCut", valueOf(p, "highCut", 12000.0f) - 1500.0f),
                  change(p, "gain", valueOf(p, "gain", 5.0f) - 0.4f) }, "Less fizz while preserving the audible pick transient."));
        if (report.compression.value > 0.76f)
            add(makeRecommendation("open-dynamics", Problem::overcompressed, "Overcompressed dynamics",
                "The tone is staying too flat and has lost playing dynamics.",
                "Compression proxy exceeds 0.76 and crest/transient evidence is reduced; lower preamp gain and sag.",
                clamp01(report.compression.value), { "compression " + std::to_string(report.compression.value),
                  "crest " + std::to_string(f.dynamic.crestFactorDb) + " dB" },
                { change(p, "gain", valueOf(p, "gain", 5.0f) - 1.0f),
                  change(p, "sag", valueOf(p, "sag", 35.0f) - 10.0f) }, "More touch sensitivity and transient contrast."));
        if (f.dynamic.transientPreservation < 0.25f)
            add(makeRecommendation("restore-transient", Problem::weakTransient, "Weak transient",
                "The front edge of notes is being softened too much.",
                "Transient-preservation proxy is below 0.25; reduce gain and add a bounded pick-emphasis correction.",
                clamp01(0.72f - f.dynamic.transientPreservation),
                { "transient preservation " + std::to_string(f.dynamic.transientPreservation) },
                { change(p, "gain", valueOf(p, "gain", 5.0f) - 0.6f),
                  change(p, "pickEmphasis", valueOf(p, "pickEmphasis") + 1.5f) }, "Restores note definition without changing timing."));
        if (f.spectral.upperMidAttack < 0.22f && f.dynamic.transientPreservation < 0.42f)
            add(makeRecommendation("unbury-pick", Problem::buriedPickAttack, "Buried pick attack",
                "The pick is getting lost behind the body of the tone.",
                "Both upper-mid attack and transient preservation are low; add bounded pre-distortion pick emphasis.",
                0.71f, { "upper-mid attack " + std::to_string(f.spectral.upperMidAttack),
                "transient " + std::to_string(f.dynamic.transientPreservation) },
                { change(p, "pickEmphasis", valueOf(p, "pickEmphasis") + 1.5f) },
                "Clearer note starts without increasing overall loudness."));
        if (report.brightness.value > 0.76f && s.zeroCrossingRate > 0.20f)
            add(makeRecommendation("reduce-string-noise", Problem::excessiveStringNoise, "Excessive string noise",
                "High-frequency handling noise is unusually prominent.",
                "High brightness plus elevated live-input zero crossings indicates excess scrape/noise energy; lower cabinet cutoff modestly.",
                0.68f, { "brightness " + std::to_string(report.brightness.value),
                "zero crossings " + std::to_string(s.zeroCrossingRate) },
                { change(p, "highCut", valueOf(p, "highCut", 12000.0f) - 1000.0f) },
                "Less scrape and fizz while retaining useful attack."));
        if (f.dynamic.loudnessRangeDb > 15.0f)
            add(makeRecommendation("stabilize-level", Problem::inconsistentLevel, "Inconsistent playing level",
                "The level varies widely between notes.",
                "Measured loudness range exceeds 15 dB; use a small gain/sag stabilization rather than a large compressor jump.",
                clamp01(f.dynamic.loudnessRangeDb / 24.0f),
                { "loudness range " + std::to_string(f.dynamic.loudnessRangeDb) + " dB" },
                { change(p, "sag", valueOf(p, "sag", 35.0f) + 6.0f) },
                "Slightly more even response while preserving playing dynamics."));
        if (f.dynamic.sustain > 0.88f && report.gain.value > 0.58f)
            add(makeRecommendation("reduce-sustain", Problem::excessiveSustain, "Excessive sustain",
                "Notes are hanging on longer than the current goal needs.",
                "High sustain and gain descriptors indicate accumulated nonlinear compression; reduce gain slightly.",
                0.72f, { "sustain " + std::to_string(f.dynamic.sustain), "gain " + std::to_string(report.gain.value) },
                { change(p, "gain", valueOf(p, "gain", 5.0f) - 0.7f) }, "Shorter decay and clearer rhythmic gaps."));
        if (report.context.instrument == nts::tone::Instrument::bass && report.gain.value > 0.65f
            && report.cleanLowBlendEstimate.value < 0.35f)
            add(makeRecommendation("preserve-bass-fundamental", Problem::bassFundamentalLoss,
                "Bass distortion is removing fundamentals", "Blend more clean low end under the distortion.",
                "Bass context, high gain, and low clean-blend estimate indicate fundamental masking; increase the phase-aligned split.",
                0.84f, { "bass context", "gain " + std::to_string(report.gain.value),
                "clean blend " + std::to_string(report.cleanLowBlendEstimate.value) },
                { change(p, "cleanBlend", valueOf(p, "cleanBlend", 40.0f) + 15.0f) },
                "Stronger low fundamentals while retaining driven upper harmonics."));
        if (report.context.instrument == nts::tone::Instrument::bass
            && report.compression.value > 0.72f && f.spectral.lowFrequencyExtension > 0.68f)
            add(makeRecommendation("reduce-bass-pumping", Problem::bassLowEndPumping, "Bass low-end pumping",
                "The low band is making the level breathe too strongly.",
                "Bass context combines high compression and low-frequency extension; reduce sag and add clean low blend.",
                0.76f, { "compression " + std::to_string(report.compression.value),
                "low extension " + std::to_string(f.spectral.lowFrequencyExtension) },
                { change(p, "sag", valueOf(p, "sag", 35.0f) - 8.0f),
                  change(p, "cleanBlend", valueOf(p, "cleanBlend", 50.0f) + 10.0f) },
                "More stable bass fundamentals and less envelope pumping."));
        if (f.spectral.highFrequencyRolloff > 0.88f && valueOf(p, "cabinet", 1.0f) > 0.5f)
            add(makeRecommendation("check-cabinet-chain", Problem::cabinetMismatch, "Cabinet chain may be too dark",
                "The cabinet filtering is removing unusually much top end.",
                "Strong measured high-frequency rolloff with the internal cabinet active may indicate a dark mismatch or duplicated host cabinet.",
                0.66f, { "high-frequency rolloff " + std::to_string(f.spectral.highFrequencyRolloff),
                "internal cabinet enabled" },
                { change(p, "highCut", valueOf(p, "highCut", 9000.0f) + 1200.0f) },
                "Tests a more open cabinet response without bypassing protection."));
        if (f.spatial.channelCorrelation < -0.15f)
            add(makeRecommendation("repair-phase", Problem::stereoPhaseProblem, "Stereo phase cancellation risk",
                "The left and right channels may cancel when summed to mono.",
                "Measured channel correlation is negative; reset cabinet alignment before changing tone controls.",
                clamp01(0.55f - f.spatial.channelCorrelation),
                { "channel correlation " + std::to_string(f.spatial.channelCorrelation) },
                { change(p, "cabinetAlignment", 0.0f) }, "Improves mono compatibility and low-frequency stability."));
        if (report.context.instrument == nts::tone::Instrument::bass
            && f.spatial.channelCorrelation < 0.15f
            && valueOf(p, "cleanBlend", 50.0f) > 15.0f && valueOf(p, "cleanBlend", 50.0f) < 85.0f)
            add(makeRecommendation("repair-clean-blend-phase", Problem::cleanBlendPhaseCancellation,
                "Clean blend phase cancellation", "The clean and driven bass paths may be cancelling each other.",
                "Bass context, active parallel blend, and low correlation indicate phase risk; reset alignment before changing blend level.",
                clamp01(0.72f - f.spatial.channelCorrelation * 0.3f),
                { "correlation " + std::to_string(f.spatial.channelCorrelation),
                  "clean blend " + std::to_string(valueOf(p, "cleanBlend", 50.0f)) },
                { change(p, "cabinetAlignment", 0.0f) }, "Restores low-frequency summation in the parallel bass path."));
    }
    if (valueOf(p, "gain", 5.0f) > 8.2f)
        add(makeRecommendation("reduce-excess-gain", Problem::excessivePreampGain, "Too much preamp gain",
            "The gain is high enough to blur note detail and amplify noise.",
            "Gain macro exceeds 8.2/10; preview a bounded reduction before changing final output level.",
            clamp01((valueOf(p, "gain") - 7.0f) / 3.0f), { "gain " + std::to_string(valueOf(p, "gain")) },
            { change(p, "gain", valueOf(p, "gain") - 1.0f) }, "Improves definition and lowers accumulated stage noise."));
    if (input.rig.gateThresholdDb > -32.0f && s.inputCrestDb > 10.0f)
        add(makeRecommendation("gate-chopping", Problem::gateChopping, "Gate may be chopping notes",
            "The gate is aggressive for the measured note decay.",
            "Gate threshold is above -32 dBFS while input crest exceeds 10 dB; raise calibrated input rather than hiding decay.",
            0.72f, { "gate threshold " + std::to_string(input.rig.gateThresholdDb) + " dB",
            "input crest " + std::to_string(s.inputCrestDb) + " dB" },
            { change(p, "input", valueOf(p, "input") + 2.0f) }, "Allows more note decay through the existing gate."));

    addGoal(result, input);
    for (auto& recommendation : result)
    {
        if (preferences.personalizationEnabled)
        {
            if (contains(preferences.acceptedActions, recommendation.actionId)) recommendation.confidence = clamp01(recommendation.confidence + 0.05f);
            if (contains(preferences.rejectedActions, recommendation.actionId)) recommendation.confidence = clamp01(recommendation.confidence - 0.12f);
        }
    }
    std::sort(result.begin(), result.end(), [](const auto& first, const auto& second)
    { return first.confidence > second.confidence; });
    if (result.size() > maximumResults) result.resize(maximumResults);
    return result;
}

bool SafeActionSession::preview(const Recommendation& recommendation,
                                std::span<const ParameterValue> current, std::string& error)
{
    if (activePreview) { error = "reject or accept the active preview first"; return false; }
    PreviewState state; state.actionId = recommendation.actionId;
    state.before.assign(current.begin(), current.end()); state.after = state.before;
    for (const auto& item : recommendation.changes)
    {
        const auto* range = rangeFor(item.parameter);
        if (range == nullptr || ! std::isfinite(item.from) || ! std::isfinite(item.to))
        { error = "unknown or non-finite assistant parameter: " + item.parameter; return false; }
        const auto actual = valueOf(current, item.parameter, std::numeric_limits<float>::quiet_NaN());
        if (! std::isfinite(actual) || std::abs(actual - item.from) > 0.05f)
        { error = "parameter changed since suggestion: " + item.parameter; return false; }
        if (item.to < range->minimum || item.to > range->maximum
            || std::abs(item.to - item.from) > range->maximumPreviewDelta + 1.0e-5f)
        { error = "assistant change exceeds validated preview bounds: " + item.parameter; return false; }
        setValue(state.after, item.parameter, item.to);
    }
    activePreview = std::move(state); return true;
}

bool SafeActionSession::accept(std::vector<ParameterValue>& current, PreferenceProfile& preferences,
                               std::string& error)
{
    if (! activePreview) { error = "no active preview"; return false; }
    history.push_back(activePreview->before); current = activePreview->after;
    if (preferences.personalizationEnabled) preferences.acceptedActions.push_back(activePreview->actionId);
    activePreview.reset(); return true;
}

bool SafeActionSession::reject(std::vector<ParameterValue>& current, PreferenceProfile& preferences,
                               std::string& error)
{
    if (! activePreview) { error = "no active preview"; return false; }
    current = activePreview->before;
    if (preferences.personalizationEnabled) preferences.rejectedActions.push_back(activePreview->actionId);
    activePreview.reset(); return true;
}

bool SafeActionSession::undo(std::vector<ParameterValue>& current, std::string& error)
{
    if (activePreview) { error = "finish the active preview before undo"; return false; }
    if (history.empty()) { error = "undo history is empty"; return false; }
    current = history.back(); history.pop_back(); return true;
}

std::string serializePreferences(const PreferenceProfile& profile, bool pretty)
{
    auto root = std::make_unique<juce::DynamicObject>();
    root->setProperty("version", juce::String(currentAssistantVersion.data()));
    root->setProperty("enabled", profile.personalizationEnabled);
    root->setProperty("instrument", juce::String(nts::tone::toString(profile.preferredInstrument).data()));
    root->setProperty("gain", profile.preferredGain); root->setProperty("brightness", profile.preferredBrightness);
    root->setProperty("cleanBlend", profile.preferredCleanBlend);
    const auto strings = [](const std::vector<std::string>& source)
    { juce::Array<juce::var> values; for (const auto& item : source) values.add(juce::String::fromUTF8(item.c_str())); return values; };
    root->setProperty("cabinets", strings(profile.frequentlyUsedCabinets));
    root->setProperty("accepted", strings(profile.acceptedActions));
    root->setProperty("rejected", strings(profile.rejectedActions));
    return juce::JSON::toString(juce::var(root.release()), pretty).toStdString();
}

std::optional<PreferenceProfile> deserializePreferences(std::string_view json, std::string& error)
{
    const auto parsed = juce::JSON::parse(juce::String::fromUTF8(json.data(), static_cast<int>(json.size())));
    const auto* root = parsed.getDynamicObject();
    if (root == nullptr) { error = "assistant preference JSON is invalid"; return std::nullopt; }
    PreferenceProfile result;
    result.personalizationEnabled = static_cast<bool>(root->getProperty("enabled"));
    const auto instrument = root->getProperty("instrument").toString();
    result.preferredInstrument = instrument == "bass" ? nts::tone::Instrument::bass : nts::tone::Instrument::guitar;
    result.preferredGain = clamp01(static_cast<float>(root->getProperty("gain")));
    result.preferredBrightness = clamp01(static_cast<float>(root->getProperty("brightness")));
    result.preferredCleanBlend = clamp01(static_cast<float>(root->getProperty("cleanBlend")));
    const auto read = [root](const char* name)
    { std::vector<std::string> values; if (const auto* list = root->getProperty(name).getArray()) for (const auto& item : *list) values.push_back(item.toString().toStdString()); return values; };
    result.frequentlyUsedCabinets = read("cabinets"); result.acceptedActions = read("accepted");
    result.rejectedActions = read("rejected"); return result;
}

std::string serializeRecommendation(const Recommendation& recommendation, bool pretty)
{
    auto root = std::make_unique<juce::DynamicObject>();
    root->setProperty("actionId", juce::String::fromUTF8(recommendation.actionId.c_str()));
    root->setProperty("description", juce::String::fromUTF8(recommendation.diagnosis.c_str()));
    root->setProperty("confidence", recommendation.confidence);
    root->setProperty("reason", juce::String::fromUTF8(recommendation.advancedExplanation.c_str()));
    root->setProperty("expectedEffect", juce::String::fromUTF8(recommendation.expectedEffect.c_str()));
    juce::Array<juce::var> changes;
    for (const auto& item : recommendation.changes)
    {
        auto value = std::make_unique<juce::DynamicObject>();
        value->setProperty("parameter", juce::String::fromUTF8(item.parameter.c_str()));
        value->setProperty("from", item.from); value->setProperty("to", item.to); changes.add(juce::var(value.release()));
    }
    root->setProperty("changes", changes);
    return juce::JSON::toString(juce::var(root.release()), pretty).toStdString();
}

std::string_view toString(Goal value) noexcept
{
    switch (value) { case Goal::tightRhythm: return "tight rhythm"; case Goal::cleanBassSupport: return "clean bass support";
        case Goal::aggressivePickedBass: return "aggressive picked bass"; case Goal::smoothLead: return "smooth lead";
        case Goal::warmClean: return "warm clean"; case Goal::lessHarsh: return "less harsh";
        case Goal::preserveLowEnd: return "preserve low end"; case Goal::matchReference: return "match reference";
        default: return "diagnose"; }
}

std::string_view toString(Problem value) noexcept
{
    switch (value) { case Problem::inputTooQuiet: return "input-too-quiet"; case Problem::inputClipping: return "input-clipping";
        case Problem::intermittentSignal: return "intermittent-signal"; case Problem::excessivePreampGain: return "excessive-preamp-gain";
        case Problem::outputClipping: return "output-clipping"; case Problem::muddyLowMids: return "muddy-low-mids";
        case Problem::thinFundamentals: return "thin-fundamentals"; case Problem::harshUpperMids: return "harsh-upper-mids";
        case Problem::fizzyHighs: return "fizzy-highs"; case Problem::overcompressed: return "overcompressed";
        case Problem::weakTransient: return "weak-transient"; case Problem::gateChopping: return "gate-chopping";
        case Problem::bassFundamentalLoss: return "bass-fundamental-loss"; case Problem::stereoPhaseProblem: return "stereo-phase-problem";
        case Problem::excessiveLatency: return "excessive-latency"; case Problem::goalAdjustment: return "goal-adjustment";
        default: return "diagnostic"; }
}
} // namespace nts::assistant
