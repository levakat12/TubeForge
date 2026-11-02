/** Round-trip recovery of a production chain: apply a known mix, then ask how much comes back.

    **Every tuning decision in the production layer is a guess until something scores it.** The
    mapping in `applyProductionProfile` is full of constants -- a 35% reverb cap, a 90-280 ms delay
    window, a 0.35 compression threshold -- that were chosen by reasoning and have no measurement
    behind them. This is that measurement, or the beginning of it.

        nts_production_recovery

    Like `nts_rig_recovery`, the labels are exact because they were chosen: the chain in
    `ProductionChain.h` applies a known amount of a known effect and the harness asks what
    `estimateProductionProfile` says about it. No copyrighted audio, no annotation, no opinion.

    **Reported per field, never as one number.** A single score would average a level that is
    trivially recoverable against a room that is not, and would move for reasons nobody could
    attribute. Level and spectral tilt are reported in their own units against an exact answer;
    the 0..1 estimates are reported as *both* absolute error and rank agreement, because those two
    ask different questions and only the second one is fair.

    **Why the 0..1 fields get a rank column.** `roomReverbEstimate` is not calibrated against this
    harness's reverb control, and there is no reason it should be: one is a dial position in a
    synthetic Schroeder network and the other is an estimator's reading of decay. Demanding they
    agree numerically measures the arbitrary relationship between two scales. What the production
    mapping actually needs is *monotonicity* -- more room in the record must produce more room in
    the estimate -- so that is measured too, and it is the column to read.

    Three things are deliberately made easy here, and all three make these numbers a ceiling:

    - **No separation.** The reference is the render plus the chain, so no separator artifacts. The
      plan's own note says artifacts shift the measured gain character by about a category on the
      CPU fallback and will bias production estimates the same way.
    - **The true dry render is the comparison.** `estimateProductionProfile` gets the actual
      pre-production render as its candidate, where the real pipeline gets a *matched* rig's render
      and any error in that match lands in the spectral residual wearing a mix EQ's name.
    - **No room-tail reduction.** `StudioServices` calls `normalizeReference` with reduction on
      when a region reads reverberant, which removes some of the very thing being measured. Here
      it is off, so the estimator is scored on what was applied rather than on what survived.

    So a chain this cannot recover here will certainly not be recovered from a record.
    `nts_song_probe` against real material is the cross-check that keeps the generator honest, and
    it is not optional before these numbers justify a change.
*/
#include "ProductionChain.h"
#include "RecoveryFixtures.h"

#include <nts/reconstruction/SourceReconstruction.h>
#include <nts/tone/ToneAnalysis.h>
#include <nts/amp/TraditionalAmp.h>
#include <nts/ml/PackedFeatureRegressor.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <span>
#include <sstream>
#include <string>
#include <vector>

namespace
{
constexpr double sampleRate = 48000.0;
constexpr double seconds = 4.0;
using nts::recovery::ProductionSettings;
using nts::recovery::phrases;
using nts::recovery::renderPhrase;

/** Which instrument every mode in this file operates on.

    **A file-scope variable rather than a parameter on twelve functions, and set once from the
    command line before anything runs.** The target decides the fixtures, the voicings, the
    scoring rig *and* the frequency each residual band stands for, so a build where two of those
    disagreed would produce a corpus whose columns mean different things in different rows -- and
    nothing would report it, because every individual number would look reasonable.
*/
nts::reconstruction::TargetInstrument activeTarget = nts::reconstruction::TargetInstrument::guitar;

[[nodiscard]] bool bassTarget() noexcept
{ return activeTarget == nts::reconstruction::TargetInstrument::bass; }

[[nodiscard]] nts::amp::Instrument ampInstrument() noexcept
{ return bassTarget() ? nts::amp::Instrument::bass : nts::amp::Instrument::guitar; }

[[nodiscard]] const std::vector<nts::recovery::Phrase>& targetPhrases()
{
    return phrases(bassTarget() ? nts::recovery::Fixture::bass : nts::recovery::Fixture::guitar);
}

/** The one rig every scored attempt is rendered through.

    A constant rather than a variable, because this harness asks what the *production estimator*
    recovers and sweeping the amplifier would add a second source of variance to every column. The
    two choices are the middle of each instrument's range: British Crunch for guitar, and for bass
    the first bass-native voicing rather than a guitar one given a bass reading -- a bass through a
    guitar voicing is not what the estimator will be shown.
*/
[[nodiscard]] nts::amp::Topology scoringTopology() noexcept
{ return bassTarget() ? nts::amp::Topology::valveFlagship : nts::amp::Topology::britishCrunch; }

/// One field's worth of scored attempts. Errors are accumulated rather than averaged as they
/// arrive so that the count and the spread are both reportable.
struct Score
{
    std::vector<double> applied;
    std::vector<double> measured;
    std::vector<double> error;

    void add(double appliedValue, double measuredValue)
    {
        applied.push_back(appliedValue);
        measured.push_back(measuredValue);
        error.push_back(measuredValue - appliedValue);
    }
    [[nodiscard]] std::size_t count() const noexcept { return error.size(); }
    [[nodiscard]] double meanAbsoluteError() const
    {
        if (error.empty()) return 0.0;
        auto total = 0.0;
        for (const auto value : error) total += std::abs(value);
        return total / static_cast<double>(error.size());
    }
    [[nodiscard]] double worstAbsoluteError() const
    {
        auto worst = 0.0;
        for (const auto value : error) worst = std::max(worst, std::abs(value));
        return worst;
    }
    /** Fraction of ordered pairs the estimate ranks the same way the applied value does.

        1.0 is perfectly monotonic and 0.5 is chance. Pairs whose applied values are equal are
        skipped rather than counted as agreement, which would inflate the figure with the many
        same-setting comparisons across phrases.
    */
    [[nodiscard]] double rankAgreement() const
    {
        std::size_t agreed {}, considered {};
        for (std::size_t first = 0; first < applied.size(); ++first)
            for (std::size_t second = first + 1; second < applied.size(); ++second)
            {
                if (std::abs(applied[first] - applied[second]) < 1.0e-9) continue;
                ++considered;
                const auto appliedRises = applied[second] > applied[first];
                const auto measuredRises = measured[second] > measured[first];
                if (appliedRises == measuredRises) ++agreed;
            }
        // Negative rather than 0.5 when there is nothing to rank: a field scored against an exact
        // answer has one applied value per attempt and no ordering to agree with, and printing
        // "chance" there would invite reading a meaningless column.
        return considered == 0 ? -1.0 : static_cast<double>(agreed) / static_cast<double>(considered);
    }
};

/** One run of the whole thing: render, produce, analyse, estimate.

    Returns nothing when the analysis fails, which is a skipped attempt rather than a zero -- a
    zero would be counted as a perfect recovery of nothing.
*/
struct Attempt
{
    nts::reconstruction::ProductionProfile profile;
    /// Kept so the feature extractor can read the raw analysis rather than re-running it.
    nts::tone::ToneAnalysisResult tone;
    bool valid {};
};

constexpr std::size_t residualBands = 41;
/* The feature vector comes from the engine rather than from a copy here.

   It used to be built in this file, which meant the corpus this harness extracts and the vector
   the plug-in feeds a model at match time were two implementations of one contract -- and the
   contract is a *column order* baked into trained weights, so a divergence would misalign a model
   against its own inputs and produce four plausible numbers. `productionFeatures` is the one
   definition; this harness and `estimateProductionProfile` both call it. */
constexpr std::size_t featureCount = nts::reconstruction::productionFeatureCount;

void appendFeatures(std::vector<float>& out, const nts::tone::ToneAnalysisResult& tone,
                    const nts::reconstruction::ProductionProfile& profile)
{
    const auto features = nts::reconstruction::productionFeatures(tone, profile);
    out.insert(out.end(), features.begin(), features.end());
}

Attempt runOnce(const std::vector<float>& dryRender, const ProductionSettings& settings)
{
    const auto [left, right] = nts::recovery::applyProductionChain(dryRender, sampleRate, settings);

    nts::reconstruction::StereoAudio produced;
    produced.sampleRate = sampleRate;
    produced.left = left;
    produced.right = right;

    // Reduction off deliberately; see the note at the top of this file.
    const auto normalized = nts::reconstruction::normalizeReference(produced, false);

    nts::tone::ToneAnalyzer analyzer;
    auto tone = analyzer.analyze({ normalized.audio.left, normalized.audio.right, sampleRate,
                                   nts::tone::SourceType::isolatedStem, 0.9f });
    if (! tone.success) return {};

    Attempt attempt;
    attempt.profile = nts::reconstruction::estimateProductionProfile(
        normalized, tone, normalized.audio.left, dryRender, sampleRate, activeTarget);
    attempt.tone = std::move(tone);
    attempt.valid = true;
    return attempt;
}

/** RMS difference between the measured residual and the shelf that was actually applied.

    Both are levelled to their own means before differencing, because `measureCabinetResidual`
    levels what it returns -- so an absolute comparison would be measuring that levelling rather
    than the shape, and would report a large error for a perfect answer.
*/
double spectralErrorDb(const nts::reconstruction::ProductionProfile& profile,
                       const ProductionSettings& settings)
{
    if (profile.spectralResidualDb.empty()) return -1.0;
    const auto bands = profile.spectralResidualDb.size();
    std::vector<double> ideal(bands);
    auto idealMean = 0.0, measuredMean = 0.0;
    for (std::size_t band = 0; band < bands; ++band)
    {
        ideal[band] = nts::recovery::shelfResponseDb(settings, profile.residualFrequencies[band],
                                                      sampleRate);
        idealMean += ideal[band];
        measuredMean += profile.spectralResidualDb[band];
    }
    idealMean /= static_cast<double>(bands);
    measuredMean /= static_cast<double>(bands);

    auto sum = 0.0;
    for (std::size_t band = 0; band < bands; ++band)
    {
        const auto difference = (profile.spectralResidualDb[band] - measuredMean)
                              - (ideal[band] - idealMean);
        sum += difference * difference;
    }
    return std::sqrt(sum / static_cast<double>(bands));
}

/** The shelf height the residual reads back, in dB.

    The span between the residual's two ends rather than its absolute level in the high bands,
    because `measureCabinetResidual` levels each spectrum to its own mean before differencing --
    so a +6 dB shelf over half the range reads as roughly +3 and -3 rather than +6 and 0. The span
    is invariant to that levelling, and the levelling is wanted: it is what makes the residual a
    shape that composes with `levelOffsetDb` instead of double-counting the loudness.
*/
double recoveredShelfDb(const nts::reconstruction::ProductionProfile& profile)
{
    auto lowSum = 0.0, highSum = 0.0;
    std::size_t lowCount {}, highCount {};
    for (std::size_t band = 0; band < profile.spectralResidualDb.size(); ++band)
    {
        const auto frequency = profile.residualFrequencies[band];
        if (frequency < 200.0f) { lowSum += profile.spectralResidualDb[band]; ++lowCount; }
        else if (frequency > 4000.0f) { highSum += profile.spectralResidualDb[band]; ++highCount; }
    }
    if (lowCount == 0 || highCount == 0) return 0.0;
    return highSum / static_cast<double>(highCount) - lowSum / static_cast<double>(lowCount);
}

/** Writes a vector of floats as little-endian `f32`, which is what numpy reads with `<f4`.

    A raw block rather than a WAV, because both sides of the parity check are arrays and a
    container would only add a header for one of them to parse.
*/
bool writeRaw(const char* path, std::span<const float> samples)
{
    std::ofstream file(path, std::ios::binary);
    if (! file) return false;
    file.write(reinterpret_cast<const char*>(samples.data()),
               static_cast<std::streamsize>(samples.size() * sizeof(float)));
    return file.good();
}

std::vector<float> readRaw(const char* path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (! file) return {};
    const auto bytes = static_cast<std::size_t>(file.tellg());
    file.seekg(0);
    std::vector<float> samples(bytes / sizeof(float));
    file.read(reinterpret_cast<char*>(samples.data()), static_cast<std::streamsize>(bytes));
    return file.good() ? samples : std::vector<float> {};
}

/** The dry renders `ml/nts_ml/production` builds its datasets from.

    **This mode exists because the amplifier is C++ and must stay in one place.** The plan calls
    for training data rendered through the engine's own amplifier at randomised rigs; a Python
    reimplementation of that amplifier would be a third copy of the component most likely to
    drift, and the drift would show up as a model that estimates production well on synthetic
    guitar and badly on the plug-in's own output. So the renders come from here, and the Python
    side owns only the chain -- which `production_parity_driver.py` holds to this file's copy.
*/
std::vector<std::vector<float>> buildRenderPool()
{
    const auto searchable = nts::reconstruction::searchableTopologies(ampInstrument());
    std::vector<std::vector<float>> pool;
    for (const auto& phrase : targetPhrases())
    {
        const auto di = renderPhrase(phrase, sampleRate, seconds);
        for (const auto topology : searchable)
        {
            const auto preset = nts::amp::makeOriginalPreset(topology, ampInstrument());
            /* Three drive settings per voicing rather than one. A production estimator sees the
               amplifier's own compression and its own spectrum as part of what it must look past,
               so a corpus at one drive setting teaches it that every guitar has the same crest
               factor -- which is exactly the confusion `compressionAmount` already has. */
            for (const auto offset : { -6.0f, 0.0f, 8.0f })
            {
                auto driven = preset;
                for (std::size_t stage = 0; stage < driven.parameters.stageCount; ++stage)
                    driven.parameters.stages[stage].driveDb += offset;
                nts::amp::TraditionalAmpProcessor processor;
                processor.prepare({ sampleRate, 256, 1 });
                processor.loadPreset(driven, 0);
                auto render = nts::amp::renderOffline(processor, di, 256);
                if (! render.empty()) pool.push_back(std::move(render));
            }
        }
    }
    return pool;
}

int emitRenders(const char* directory)
{
    const auto pool = buildRenderPool();
    for (std::size_t index = 0; index < pool.size(); ++index)
    {
        // Zero-padded, because `render_pool_from_directory` sorts by file name to keep a seeded
        // dataset identical across machines -- and "render-9" sorts after "render-10".
        std::ostringstream name;
        name << directory << "/render-" << std::setfill('0') << std::setw(3) << index << ".f32";
        if (! writeRaw(name.str().c_str(), pool[index])) return 1;
    }
    std::cout << "wrote " << pool.size() << " dry renders to " << directory << std::endl;
    return 0;
}

/** The feature matrix a corpus of settings produces, for `ml/scripts/train_production.py`.

    **The features are extracted here because the analyser is here.** `ToneAnalyzer` and
    `measureCabinetResidual` are the two things the model reads, both are C++, and a Python
    reimplementation of either would be a copy of the component whose behaviour the model is
    entirely built around -- the drift would show up as a model that works on the training corpus
    and not in the plug-in, with nothing reporting a problem.

    Takes a settings matrix rather than generating one, so the sampling stays in
    `nts_ml.production.generator` and there is one definition of what a plausible mix is. Each
    input row is nine floats -- a render index and the eight chain settings -- and each output row
    is `featureCount` floats in the order `nts_ml.models.FEATURE_NAMES` declares.

    A row whose analysis fails is written as all zeros and reported, rather than skipped: dropping
    it would silently misalign every later row against the labels the caller still holds.
*/
int emitFeatures(const char* settingsPath, const char* featuresPath)
{
    constexpr std::size_t settingsColumns = 9;
    const auto raw = readRaw(settingsPath);
    if (raw.empty() || raw.size() % settingsColumns != 0)
    {
        std::cerr << "settings matrix must be a whole number of 9-float rows" << std::endl;
        return 2;
    }
    const auto rows = raw.size() / settingsColumns;
    const auto pool = buildRenderPool();
    if (pool.empty()) { std::cerr << "no renders" << std::endl; return 1; }

    std::vector<float> features;
    features.reserve(rows * featureCount);
    std::size_t failures {};
    for (std::size_t row = 0; row < rows; ++row)
    {
        const auto* values = raw.data() + row * settingsColumns;
        const auto renderIndex = static_cast<std::size_t>(std::lround(values[0])) % pool.size();
        ProductionSettings settings;
        settings.levelOffsetDb = values[1];
        settings.tiltDb = values[2];
        settings.tiltHz = values[3];
        settings.reverbAmount = values[4];
        settings.delayAmount = values[5];
        settings.delayMs = values[6];
        settings.doubling = values[7];
        settings.compression = values[8];

        const auto attempt = runOnce(pool[renderIndex], settings);
        if (! attempt.valid || attempt.profile.spectralResidualDb.size() != residualBands)
        {
            ++failures;
            features.insert(features.end(), featureCount, 0.0f);
            continue;
        }
        appendFeatures(features, attempt.tone, attempt.profile);
        if (row % 100 == 0)
            std::cout << "  " << row << "/" << rows << std::endl;
    }
    if (! writeRaw(featuresPath, features)) return 1;
    std::cout << "wrote " << rows << " feature rows (" << featureCount << " each), "
              << failures << " failed" << std::endl;
    return 0;
}

/// Applies the chain to a raw `f32` file, for `production_parity_driver.py` to compare against.
int emitProduced(int argc, char** argv)
{
    if (argc < 12) { std::cerr << "produce needs input, output and eight settings" << std::endl; return 2; }
    const auto dry = readRaw(argv[2]);
    if (dry.empty()) { std::cerr << "could not read " << argv[2] << std::endl; return 1; }
    ProductionSettings settings;
    settings.levelOffsetDb = std::stof(argv[4]);
    settings.tiltDb = std::stof(argv[5]);
    settings.tiltHz = std::stof(argv[6]);
    settings.reverbAmount = std::stof(argv[7]);
    settings.delayAmount = std::stof(argv[8]);
    settings.delayMs = std::stof(argv[9]);
    settings.doubling = std::stof(argv[10]);
    settings.compression = std::stof(argv[11]);
    const auto [left, right] = nts::recovery::applyProductionChain(dry, sampleRate, settings);
    // Interleaved, so one file carries the pair and the reader needs no second path.
    std::vector<float> interleaved(left.size() * 2);
    for (std::size_t index = 0; index < left.size(); ++index)
    {
        interleaved[index * 2] = left[index];
        interleaved[index * 2 + 1] = right[index];
    }
    return writeRaw(argv[3], interleaved) ? 0 : 1;
}

void report(const char* name, const char* unit, const Score& score, double target)
{
    if (score.count() == 0) { std::cout << "  " << name << ": no attempts" << std::endl; return; }
    const auto mean = score.meanAbsoluteError();
    std::cout << "  " << std::left << std::setw(14) << name << std::right
              << "n=" << std::setw(4) << score.count()
              << "  mean |err| " << std::setw(6) << std::fixed << std::setprecision(3) << mean
              << " " << unit
              << "  worst " << std::setw(6) << score.worstAbsoluteError() << " " << unit
              << "  rank ";
    if (const auto rank = score.rankAgreement(); rank < 0.0) std::cout << "  n/a";
    else std::cout << std::setw(5) << std::setprecision(2) << rank;
    std::cout << (target > 0.0 ? (mean <= target ? "   within target" : "   OVER target") : "")
              << std::endl;
}
} // namespace

int main(int argc, char** argv)
{
    /* `--bass` first, because every other mode reads `target` while it runs. Consumed here and
       then removed from the argument list, so the modes below index arguments as they always did
       and adding this flag did not have to touch any of them. */
    std::vector<char*> arguments(argv, argv + argc);
    for (auto entry = arguments.begin() + (argc > 0 ? 1 : 0); entry != arguments.end(); ++entry)
        if (std::string(*entry) == "--bass")
        {
            activeTarget = nts::reconstruction::TargetInstrument::bass;
            arguments.erase(entry);
            break;
        }
    argc = static_cast<int>(arguments.size());
    argv = arguments.data();

    /* The two modes whose output is parsed by another program print nothing but their answer.
       `--produce` writes a file and `--ranges` is read by `production_parity_driver.py`, so a
       banner on stdout is not a nicety there -- it is a parse error. */
    if (argc > 1 && std::string(argv[1]) == "--produce")
        return emitProduced(argc, argv);
    if (argc > 1 && std::string(argv[1]) == "--ranges")
    {
        const auto& ceilings = nts::recovery::productionCeilings;
        std::cout << ceilings.reverb << ' ' << ceilings.delay << ' '
                  << ceilings.doubling << ' ' << ceilings.compression << std::endl;
        return 0;
    }
    std::cout << "target: " << nts::reconstruction::toString(activeTarget) << std::endl;

    /* Two service modes beside the harness, both for `ml/nts_ml/production`.

       In this executable rather than in tools of their own because they are the same fixtures,
       the same chain and the same amplifier the harness already links -- and a separate binary
       would be a second place for the phrase list or the sample rate to be set differently. */
    if (argc > 1 && std::string(argv[1]) == "--render")
        return argc > 2 ? emitRenders(argv[2]) : (std::cerr << "render needs a directory\n", 2);
    if (argc > 3 && std::string(argv[1]) == "--features")
        return emitFeatures(argv[2], argv[3]);
    if (argc > 1 && std::string(argv[1]) == "--pool")
    {
        // How many dry renders this target's pool holds. Printed rather than duplicated as a
        // constant in the training script, because the two must agree for the render-disjoint
        // split to hold out whole renders -- and the count differs per instrument, since bass is
        // searched over every voicing and guitar over seven.
        std::cout << buildRenderPool().size() << std::endl;
        return 0;
    }

    /* One rig for every attempt, and a crunch one.

       The question here is what the *production estimator* recovers, so the amplifier is a
       constant rather than a variable -- sweeping it would add a second source of variance to
       every field without answering anything this harness asks. Crunch because it is the middle
       of the range: a clean render gives the compressor nothing to work on and a saturated one is
       already so compressed that an added compressor is unmeasurable. */
    const auto preset = nts::amp::makeOriginalPreset(scoringTopology(), ampInstrument());

    /* An optional learned estimator, scored beside the deterministic one on the same attempts.

       Optional because the deterministic path has to be measurable on a machine with no model
       installed -- that is the configuration most people run and the one P11 falls back to, and a
       harness that could only score the other one would leave it unmeasured. */
    nts::ml::PackedFeatureRegressor regressor;
    if (argc > 2 && std::string(argv[1]) == "--model")
    {
        std::string error;
        if (! regressor.loadFile(argv[2], error))
        { std::cerr << "could not load the model: " << error << std::endl; return 1; }
        std::cout << "scoring against " << argv[2] << " ("
                  << regressor.inputCount() << " features -> " << regressor.outputCount()
                  << " outputs)" << std::endl;
    }

    // Five settings per field, over ranges that occur in real mixes rather than over the whole
    // parameter space: a model or a mapping tuned on implausible mixes is confident about them.
    constexpr float levels[]      = { -12.0f, -6.0f, 0.0f, 3.0f, 6.0f };
    constexpr float tilts[]       = { -8.0f, -4.0f, -2.0f, 4.0f, 8.0f };
    /* The four 0..1 fields are swept on the **normalised** axis and scaled to the chain by
       `productionCeilings` when applied.

       They used to be swept as raw dial positions, which made the absolute-error column compare a
       dial in a synthetic chain against an estimator reading on its own scale -- two quantities
       with no reason to agree, so only the rank column meant anything. On the normalised axis both
       estimators and the applied amount are the same 0..1, the error column is a real measurement,
       and the plan's 0.15 target becomes a claim somebody can check. Rank agreement is unchanged:
       scaling the applied axis by a positive constant cannot reorder it. */
    constexpr float amounts[] = { 0.12f, 0.30f, 0.50f, 0.72f, 0.95f };

    /* Two scores for the spectrum, because they answer different questions. `spectrumHeight`
       asks whether the residual reads the shelf back at the right height, which is what the
       mapping would act on and which has a meaningful rank column. `spectrumShape` asks how far
       the whole 41-band curve is from the exact analytic response, which is the real quality
       measure and has no ordering to rank. Reporting only the first would hide a residual that
       gets the height right through a curve of the wrong shape. */
    Score level, spectrumHeight, spectrumShape, room, delay, doubling, compression;
    Score learnedRoom, learnedDelay, learnedDoubling, learnedCompression;
    std::size_t attempts {}, skipped {};

    for (const auto& phrase : targetPhrases())
    {
        const auto di = renderPhrase(phrase, sampleRate, seconds);
        nts::amp::TraditionalAmpProcessor processor;
        processor.prepare({ sampleRate, 256, 1 });
        processor.loadPreset(preset, 0);
        const auto dryRender = nts::amp::renderOffline(processor, di, 256);
        if (dryRender.empty()) continue;

        /* The zero-production baseline this phrase's level errors are measured against.

           `levelOffsetDb` is an absolute reading -- where the region sat relative to the -18 dB
           the analyser normalises to -- so it carries the render's own loudness as well as
           whatever the chain added. What the field claims to recover is the *offset*, and the
           offset is the difference from this. Scoring the absolute number instead would report
           the amplifier's output gain as a production error. */
        const auto baseline = runOnce(dryRender, {});
        if (! baseline.valid) { ++skipped; continue; }

        // `values` by reference, not by value: these are C arrays, and a by-value parameter would
        // decay each of them to a pointer with no length to iterate.
        const auto sweep = [&](auto&& configure, Score& score, auto&& measure, const auto& values)
        {
            for (const auto value : values)
            {
                ProductionSettings settings;
                configure(settings, value);
                const auto attempt = runOnce(dryRender, settings);
                ++attempts;
                if (! attempt.valid) { ++skipped; continue; }
                measure(score, settings, attempt.profile, value);
            }
        };

        sweep([](ProductionSettings& s, float v) { s.levelOffsetDb = v; }, level,
              [&](Score& s, const ProductionSettings&, const auto& profile, float value)
              { s.add(value, profile.levelOffsetDb - baseline.profile.levelOffsetDb); }, levels);

        sweep([](ProductionSettings& s, float v) { s.tiltDb = v; }, spectrumHeight,
              [](Score& s, const ProductionSettings&, const auto& profile, float value)
              { if (! profile.spectralResidualDb.empty()) s.add(value, recoveredShelfDb(profile)); },
              tilts);

        // Applied is zero for every attempt because the target is zero miss, which is why this one
        // has no rank column to read.
        sweep([](ProductionSettings& s, float v) { s.tiltDb = v; }, spectrumShape,
              [](Score& s, const ProductionSettings& settings, const auto& profile, float)
              {
                  const auto miss = spectralErrorDb(profile, settings);
                  if (miss >= 0.0) s.add(0.0, miss);
              }, tilts);

        /* Each field scored twice from the *same* attempt: once as the deterministic estimator
           reports it, once as the model predicts it from the same features. Running the model on a
           separate pass would let two runs differ by anything else that moved, and the whole claim
           being tested is a difference between two readings of one measurement. */
        const auto scoreBoth = [&](Score& deterministic, Score& learned, std::size_t column,
                                   auto&& configure, float ceiling, auto&& read)
        {
            for (const auto normalised : amounts)
            {
                ProductionSettings settings;
                configure(settings, normalised * ceiling);
                const auto attempt = runOnce(dryRender, settings);
                ++attempts;
                if (! attempt.valid) { ++skipped; continue; }
                deterministic.add(normalised, read(attempt.profile));
                if (regressor.isLoaded())
                {
                    std::vector<float> features;
                    features.reserve(featureCount);
                    appendFeatures(features, attempt.tone, attempt.profile);
                    std::vector<float> predicted(regressor.outputCount());
                    if (features.size() == regressor.inputCount()
                        && regressor.predict(features, predicted) && column < predicted.size())
                        learned.add(normalised, predicted[column]);
                }
            }
        };

        scoreBoth(room, learnedRoom, 0,
                  [](ProductionSettings& s, float v) { s.reverbAmount = v; },
                  nts::recovery::productionCeilings.reverb,
                  [](const auto& profile) { return profile.roomAmount; });
        scoreBoth(delay, learnedDelay, 1,
                  [](ProductionSettings& s, float v) { s.delayAmount = v; },
                  nts::recovery::productionCeilings.delay,
                  [](const auto& profile) { return profile.delayAmount; });
        scoreBoth(doubling, learnedDoubling, 2,
                  [](ProductionSettings& s, float v) { s.doubling = v; },
                  nts::recovery::productionCeilings.doubling,
                  [](const auto& profile) { return profile.doubling; });
        scoreBoth(compression, learnedCompression, 3,
                  [](ProductionSettings& s, float v) { s.compression = v; },
                  nts::recovery::productionCeilings.compression,
                  [](const auto& profile) { return profile.compression; });

        std::cout << "  " << phrase.name << ": swept" << std::endl;
    }

    std::cout << std::endl
              << "attempts " << attempts << ", skipped " << skipped << std::endl << std::endl;

    // Targets from the plan's "done when". They are aspirations, not thresholds this exits on:
    // the point of the harness is to record the honest baseline, and a tool that failed the build
    // for missing an untested aspiration would just get its targets edited.
    /* Level reads exactly zero here, and that is a property of the harness rather than an
       achievement of the estimator. `levelOffsetDb` is the region's RMS in dB minus the -18 the
       analyser normalises to; multiplying a signal by a gain multiplies its RMS by exactly that
       gain, and `normalizeReference` trims silence against a threshold scaled from the peak, so
       the trim is identical too. The recovery is arithmetic. What this column is actually good
       for is catching the day someone folds the offset into something non-linear -- the
       amplifier's own output gain, say -- at which point it stops being zero. */
    std::cout << "level and spectrum -- exact answers, so the error column is the whole story"
              << std::endl
              << "  (level is arithmetic here, not a measurement: see the note at this print)"
              << std::endl;
    report("level", "dB", level, 1.0);
    report("shelf height", "dB", spectrumHeight, 1.5);
    report("shelf shape", "dB", spectrumShape, 1.5);
    std::cout << std::endl
              << "0..1 estimates -- read the rank column; see the note at the top of this file"
              << std::endl;
    report("room", "", room, 0.15);
    report("delay", "", delay, 0.15);
    report("doubling", "", doubling, 0.15);
    report("compression", "", compression, 0.15);

    if (regressor.isLoaded())
    {
        std::cout << std::endl
                  << "learned estimator -- same attempts, same axis, so the rows are comparable"
                  << std::endl;
        report("room", "", learnedRoom, 0.15);
        report("delay", "", learnedDelay, 0.15);
        report("doubling", "", learnedDoubling, 0.15);
        report("compression", "", learnedCompression, 0.15);
    }

    std::cout << std::endl
              << "Recorded as a baseline, not as a pass. Nothing in the production mapping should"
              << std::endl
              << "be tuned against these until they have been cross-checked with nts_song_probe"
              << std::endl
              << "against real material -- the generator and the harness share their idea of a mix."
              << std::endl;
    return 0;
}
