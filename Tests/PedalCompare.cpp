/** Compares real-pedal NAM captures with a TubeForge pedal and prints where they differ.

    **Not a ctest suite, and deliberately so.** It needs a folder of captures that cannot be
    committed, and what it prints is a baseline to improve on rather than a threshold to pass. Point
    it at a manifest (`file,pack,tone,dist,level`, paths relative to the manifest, knobs 0-10):

        nts_pedal_compare --manifest <csv> --pack <name> --pedal <modelName>
                          [--input-offset-db X | --fit-input] [--wav-dir <dir>] [--di <wav>]
        nts_pedal_compare --bench --pedal <modelName>

    Captures are rendered directly through `nam::packWaveNet` and `ml::NeuralModel`, not through a
    pedal slot, so no slot compensation sits between the capture and the number. The pedal goes
    through `PedalBoard` with Mix at 100. Everything is level-matched per row before bands are
    compared, because a capture's output level depends on the author's interface, not the pedal.

    The input level has the same problem and cannot be matched by RMS, since it changes how hard the
    circuit is driven. `--fit-input` searches one input offset for the whole pack that makes the
    capture's THD-against-level curve agree with the pedal's, and `--input-offset-db` sets it by
    hand. Without either it is 0 dB.

    The test signal is a plucked-string phrase generated here (Karplus-Strong, about -12 dBFS), so
    nothing has to be shipped; `--di` adds a real recording alongside it. The other signals are
    220 Hz sines at -36, -24, -12 and -6 dBFS.
*/
#include <nts/ml/NeuralModel.h>
#include <nts/nam/Capture.h>
#include <nts/pedals/PedalBoard.h>

#include <PedalMeasure.h>

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace
{
constexpr double workingRate = 48000.0;
constexpr std::size_t blockSize = 128;
/// Silence run through both renderers first, so a pedal's fade-in and a model's warm-up are over.
constexpr std::size_t preRollSamples = 24000;
constexpr double sineHz = 220.0;
constexpr std::array<double, 4> sineLevelsDb { -36.0, -24.0, -12.0, -6.0 };
/// How much of a sine is thrown away before measuring it, for the same reason as the pre-roll.
constexpr std::size_t sineSettleSamples = 12000;

std::string num(double value)
{
    std::ostringstream text;
    text << value;
    return text.str();
}

struct Row
{
    std::string stem, pack;
    juce::File file;
    double tone {}, dist {};
    std::optional<double> level;

    [[nodiscard]] std::string label() const { return pack + "_" + stem; }
    [[nodiscard]] std::string settingKey() const
    {
        return num(tone) + "/" + num(dist) + "/" + (level ? num(*level) : "-");
    }
};

double dbToGain(double decibels) { return std::pow(10.0, decibels / 20.0); }

double rms(const std::vector<float>& signal)
{
    double total = 0.0;
    for (const auto sample : signal) total += static_cast<double>(sample) * sample;
    return std::sqrt(total / static_cast<double>(std::max<std::size_t>(signal.size(), 1)));
}

// --- signals -----------------------------------------------------------------------------------

/// A pluck: noise burst through a delay line with a two-point average, which is what a string does
/// to its own energy. Deterministic, so two runs of the tool measure the same thing.
void pluck(std::vector<float>& out, std::size_t start, double frequency, double seconds, float level,
           std::uint32_t& seed)
{
    const auto length = static_cast<std::size_t>(workingRate / frequency);
    std::vector<float> line(length);
    for (auto& sample : line)
    {
        seed = seed * 1664525u + 1013904223u;
        sample = (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f) * level;
    }
    const auto count = static_cast<std::size_t>(seconds * workingRate);
    for (std::size_t index = 0, position = 0; index < count && start + index < out.size(); ++index)
    {
        const auto next = (position + 1) % length;
        out[start + index] += line[position];
        line[position] = 0.5f * (line[position] + line[next]) * 0.998f;
        position = next;
    }
}

std::vector<float> makeGuitarPhrase()
{
    std::vector<float> phrase(static_cast<std::size_t>(6.0 * workingRate), 0.0f);
    std::uint32_t seed = 12345u;
    const auto at = [](double seconds) { return static_cast<std::size_t>(seconds * workingRate); };
    // Single notes climbing a low-string phrase, then a chord.
    constexpr std::array<double, 6> notes { 82.41, 110.0, 146.83, 196.0, 164.81, 246.94 };
    for (std::size_t index = 0; index < notes.size(); ++index)
        pluck(phrase, at(0.1 + 0.55 * static_cast<double>(index)), notes[index], 0.9, 1.0f, seed);
    for (const auto note : { 110.0, 164.81, 220.0, 277.18, 329.63 })
        pluck(phrase, at(3.6), note, 2.2, 0.6f, seed);
    float peak = 0.0f;
    for (const auto sample : phrase) peak = std::max(peak, std::abs(sample));
    const auto scale = static_cast<float>(dbToGain(-12.0)) / std::max(peak, 1e-6f);
    for (auto& sample : phrase) sample *= scale;
    return phrase;
}

std::vector<float> makeSine(double levelDb)
{
    std::vector<float> sine(static_cast<std::size_t>(workingRate));
    for (std::size_t index = 0; index < sine.size(); ++index)
        sine[index] = static_cast<float>(dbToGain(levelDb) * std::sin(2.0 * std::numbers::pi * sineHz
                                                                      * static_cast<double>(index) / workingRate));
    return sine;
}

std::optional<std::vector<float>> loadDi(const juce::File& file)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (reader == nullptr || reader->numChannels == 0) return std::nullopt;
    const auto sourceSamples = static_cast<int>(std::min<juce::int64>(
        reader->lengthInSamples, static_cast<juce::int64>(reader->sampleRate * 10.0)));
    if (sourceSamples <= 0) return std::nullopt;
    juce::AudioBuffer<float> decoded(1, sourceSamples);
    if (! reader->read(&decoded, 0, sourceSamples, 0, true, false)) return std::nullopt;
    const auto outputSamples = static_cast<int>(std::ceil(static_cast<double>(sourceSamples) * workingRate
                                                          / reader->sampleRate));
    std::vector<float> di(static_cast<std::size_t>(outputSamples));
    juce::LagrangeInterpolator interpolator;
    interpolator.process(reader->sampleRate / workingRate, decoded.getReadPointer(0), di.data(),
                         outputSamples, sourceSamples, 0);
    return di;
}

// --- renderers ---------------------------------------------------------------------------------

/// One capture, loaded once and rendered as often as needed.
class CaptureRenderer
{
public:
    bool load(const juce::File& file, std::string& error)
    {
        nts::nam::Capture capture;
        if (! nts::nam::readCapture(file.loadFileAsString(), capture, error)) return false;
        if (capture.spec(nts::nam::Tier::standard) == nullptr) { error = "no weights"; return false; }
        std::vector<std::byte> packed;
        if (! nts::nam::packWaveNet(*capture.spec(nts::nam::Tier::standard), packed, error)) return false;
        return model.load(packed, error);
    }

    [[nodiscard]] std::vector<float> render(const std::vector<float>& input, double inputOffsetDb)
    {
        model.reset();
        std::vector<float> padded(preRollSamples, 0.0f);
        const auto gain = static_cast<float>(dbToGain(inputOffsetDb));
        for (const auto sample : input) padded.push_back(sample * gain);
        std::vector<float> output(padded.size());
        for (std::size_t start = 0; start < padded.size(); start += 512)
        {
            const auto count = std::min<std::size_t>(512, padded.size() - start);
            model.process(std::span<const float>(padded.data() + start, count),
                          std::span<float>(output.data() + start, count));
        }
        output.erase(output.begin(), output.begin() + static_cast<std::ptrdiff_t>(preRollSamples));
        return output;
    }

private:
    nts::ml::NeuralModel model;
};

int findPedal(const std::string& name)
{
    for (std::size_t index = 0; index < nts::pedals::modelCount(); ++index)
        if (nts::pedals::pedalModel(static_cast<int>(index)).name == name) return static_cast<int>(index);
    return -1;
}

nts::pedals::PedalParameters parametersFor(const Row& row, int model)
{
    nts::pedals::PedalParameters parameters;
    parameters.model = model;
    // Orange Razor's `drive` is the control labelled Distortion; its on-screen labels are shifted.
    parameters.drive = static_cast<float>(row.dist);
    parameters.tone = static_cast<float>(row.tone);
    parameters.mix = 100.0f;
    parameters.levelDb = row.level ? static_cast<float>(*row.level * 4.8 - 24.0) : 0.0f;
    return parameters;
}

std::vector<float> renderPedal(const nts::pedals::PedalParameters& parameters,
                               const std::vector<float>& input)
{
    auto board = std::make_unique<nts::pedals::PedalBoard>();
    board->prepare({ workingRate, blockSize, 1 });
    board->setParameters(0, parameters);
    std::vector<float> padded(preRollSamples, 0.0f);
    padded.insert(padded.end(), input.begin(), input.end());
    padded.resize(((padded.size() + blockSize - 1) / blockSize) * blockSize, 0.0f);
    for (std::size_t start = 0; start < padded.size(); start += blockSize)
    {
        float* channel = padded.data() + start;
        board->setParameters(0, parameters);
        board->process(&channel, 1, blockSize);
    }
    return { padded.begin() + static_cast<std::ptrdiff_t>(preRollSamples),
             padded.begin() + static_cast<std::ptrdiff_t>(preRollSamples + input.size()) };
}

// --- measurements ------------------------------------------------------------------------------

struct SineStats
{
    std::array<double, 4> thdPercent {};
    std::array<double, 5> harmonics {};
    double aliasFloorDb {};
};

SineStats measureSines(const std::array<std::vector<float>, 4>& renders)
{
    SineStats stats;
    for (std::size_t index = 0; index < renders.size(); ++index)
    {
        const std::span<const float> settled(renders[index].data() + sineSettleSamples,
                                             renders[index].size() - sineSettleSamples);
        stats.thdPercent[index] = pedalmeasure::thdPercent(settled, workingRate, sineHz);
        if (index == 2)
        {
            stats.harmonics = pedalmeasure::harmonicsDb(settled, workingRate, sineHz);
            stats.aliasFloorDb = pedalmeasure::aliasFloorDb(settled, workingRate, sineHz);
        }
    }
    return stats;
}

double thdDb(double percent) { return 20.0 * std::log10(std::max(percent, 0.01)); }

/// Mean difference between the two THD curves, in dB of THD.
double thdCurveError(const SineStats& capture, const SineStats& pedal)
{
    double total = 0.0;
    for (std::size_t index = 0; index < capture.thdPercent.size(); ++index)
        total += std::abs(thdDb(capture.thdPercent[index]) - thdDb(pedal.thdPercent[index]));
    return total / static_cast<double>(capture.thdPercent.size());
}

std::array<std::vector<float>, 4> sineSignals()
{
    return { makeSine(sineLevelsDb[0]), makeSine(sineLevelsDb[1]), makeSine(sineLevelsDb[2]),
             makeSine(sineLevelsDb[3]) };
}

/// Band levels of a render after its level has been brought to a reference RMS.
std::vector<double> bandsAt(const std::vector<float>& signal, double referenceRms)
{
    auto levels = pedalmeasure::thirdOctaveLevelsDb(signal, workingRate);
    const auto shift = 20.0 * std::log10(referenceRms / std::max(rms(signal), 1e-9));
    for (auto& level : levels) level += shift;
    return levels;
}

struct SignalResult
{
    std::string name;
    std::vector<double> difference; // pedal minus capture, per band, level matched
    std::vector<double> captureBands, pedalBands; // raw, for the knob checks
    double meanAbs {};
    std::size_t worst {};
};

struct RowResult
{
    Row row;
    bool ok { true };
    std::string error;
    std::vector<SignalResult> signals; // guitar first, DI second when given
    SineStats capture, pedal;
};

RowResult analyseRow(const Row& row, int model, double inputOffsetDb,
                     const std::vector<std::pair<std::string, std::vector<float>>>& programs,
                     const std::optional<juce::File>& wavDir)
{
    RowResult result;
    result.row = row;
    CaptureRenderer capture;
    if (! capture.load(row.file, result.error)) { result.ok = false; return result; }
    const auto parameters = parametersFor(row, model);

    for (const auto& [name, signal] : programs)
    {
        const auto captured = capture.render(signal, inputOffsetDb);
        const auto pedal = renderPedal(parameters, signal);
        SignalResult signalResult;
        signalResult.name = name;
        signalResult.captureBands = pedalmeasure::thirdOctaveLevelsDb(captured, workingRate);
        signalResult.pedalBands = pedalmeasure::thirdOctaveLevelsDb(pedal, workingRate);
        const auto reference = rms(captured);
        const auto captureLevelled = bandsAt(captured, reference);
        const auto pedalLevelled = bandsAt(pedal, reference);
        for (std::size_t band = 0; band < pedalmeasure::bandCount; ++band)
        {
            const auto difference = pedalLevelled[band] - captureLevelled[band];
            signalResult.difference.push_back(difference);
            signalResult.meanAbs += std::abs(difference) / static_cast<double>(pedalmeasure::bandCount);
            if (std::abs(difference) > std::abs(signalResult.difference[signalResult.worst]))
                signalResult.worst = band;
        }
        result.signals.push_back(std::move(signalResult));

        if (wavDir && name == programs.front().first)
        {
            juce::WavAudioFormat wav;
            const auto write = [&](const std::vector<float>& data, const char* suffix)
            {
                const auto file = wavDir->getChildFile(juce::String(row.label()) + "_" + suffix + ".wav");
                std::unique_ptr<juce::OutputStream> stream(file.createOutputStream());
                if (stream == nullptr) return;
                auto writer = wav.createWriterFor(stream, juce::AudioFormatWriterOptions()
                                                              .withSampleRate(workingRate)
                                                              .withNumChannels(1)
                                                              .withBitsPerSample(24));
                if (writer == nullptr) return;
                const float* pointer = data.data();
                writer->writeFromFloatArrays(&pointer, 1, static_cast<int>(data.size()));
            };
            write(captured, "capture");
            write(pedal, "pedal");
        }
    }

    std::array<std::vector<float>, 4> captureSines, pedalSines;
    const auto sines = sineSignals();
    for (std::size_t index = 0; index < sines.size(); ++index)
    {
        captureSines[index] = capture.render(sines[index], inputOffsetDb);
        pedalSines[index] = renderPedal(parameters, sines[index]);
    }
    result.capture = measureSines(captureSines);
    result.pedal = measureSines(pedalSines);
    return result;
}

/// Runs `job(index)` for every index on all cores. The jobs share nothing but read-only data.
void parallelFor(std::size_t count, const std::function<void(std::size_t)>& job)
{
    std::atomic<std::size_t> next { 0 };
    std::vector<std::thread> workers;
    const auto threads = std::max<std::size_t>(1, std::min<std::size_t>(count, std::thread::hardware_concurrency()));
    for (std::size_t worker = 0; worker < threads; ++worker)
        workers.emplace_back([&]
        {
            for (auto index = next++; index < count; index = next++) job(index);
        });
    for (auto& worker : workers) worker.join();
}

// --- manifest ----------------------------------------------------------------------------------

std::vector<Row> readManifest(const juce::File& manifest)
{
    std::vector<Row> rows;
    auto lines = juce::StringArray::fromLines(manifest.loadFileAsString());
    for (int index = 1; index < lines.size(); ++index)
    {
        auto cells = juce::StringArray::fromTokens(lines[index], ",", "");
        if (cells.size() < 5) continue;
        Row row;
        row.file = manifest.getParentDirectory().getChildFile(cells[0].trim());
        row.stem = row.file.getFileNameWithoutExtension().toStdString();
        row.pack = cells[1].trim().toStdString();
        row.tone = cells[2].getDoubleValue();
        row.dist = cells[3].getDoubleValue();
        if (cells[4].trim().isNotEmpty()) row.level = cells[4].getDoubleValue();
        rows.push_back(std::move(row));
    }
    return rows;
}

// --- fitting -----------------------------------------------------------------------------------

/// One offset for the pack: the one that makes the captures' THD curves agree best with the pedal's.
double fitInput(const std::vector<Row>& rows, int model)
{
    constexpr int lowest = -24, highest = 24;
    // A handful of evenly spaced rows is enough to find one number, and the full set would be
    // forty-nine renders of every capture.
    std::vector<const Row*> subset;
    const auto wanted = std::min<std::size_t>(rows.size(), 6);
    for (std::size_t index = 0; index < wanted; ++index)
        subset.push_back(&rows[index * rows.size() / wanted]);

    const auto sines = sineSignals();
    std::vector<SineStats> pedalStats(subset.size());
    for (std::size_t row = 0; row < subset.size(); ++row)
    {
        std::array<std::vector<float>, 4> renders;
        for (std::size_t index = 0; index < sines.size(); ++index)
            renders[index] = renderPedal(parametersFor(*subset[row], model), sines[index]);
        pedalStats[row] = measureSines(renders);
    }

    const auto offsets = static_cast<std::size_t>(highest - lowest + 1);
    std::vector<double> errors(subset.size() * offsets, 0.0);
    parallelFor(errors.size(), [&](std::size_t task)
    {
        const auto row = task / offsets;
        const auto offset = lowest + static_cast<int>(task % offsets);
        CaptureRenderer capture;
        std::string error;
        if (! capture.load(subset[row]->file, error)) { errors[task] = 1e9; return; }
        std::array<std::vector<float>, 4> renders;
        for (std::size_t index = 0; index < sines.size(); ++index)
            renders[index] = capture.render(sines[index], offset);
        errors[task] = thdCurveError(measureSines(renders), pedalStats[row]);
    });

    std::cout << "input offset fit (mean THD-curve error in dB, " << subset.size() << " captures)\n";
    int best = lowest;
    double bestError = 1e18;
    for (std::size_t offset = 0; offset < offsets; ++offset)
    {
        double total = 0.0;
        for (std::size_t row = 0; row < subset.size(); ++row) total += errors[row * offsets + offset];
        total /= static_cast<double>(subset.size());
        std::cout << (offset % 8 == 0 ? "\n  " : " ") << std::showpos << std::setw(3)
                  << lowest + static_cast<int>(offset) << std::noshowpos << ":" << std::fixed
                  << std::setprecision(1) << total;
        if (total < bestError) { bestError = total; best = lowest + static_cast<int>(offset); }
    }
    std::cout << "\n  best offset " << std::showpos << best << " dB" << std::noshowpos
              << " (error " << std::setprecision(2) << bestError << " dB)\n\n";
    return best;
}

// --- reporting ---------------------------------------------------------------------------------

void printBands(const char* heading, const std::vector<double>& values)
{
    std::cout << "    " << std::left << std::setw(10) << heading << std::right;
    for (const auto value : values) std::cout << std::setw(6) << std::fixed << std::setprecision(1) << value;
    std::cout << '\n';
}

void printRow(const RowResult& result)
{
    const auto& row = result.row;
    std::cout << row.label() << "  tone " << row.tone << "  dist " << row.dist;
    if (row.level) std::cout << "  level " << *row.level;
    std::cout << '\n';
    if (! result.ok) { std::cout << "    FAILED: " << result.error << "\n\n"; return; }

    std::cout << "    band centres Hz:";
    for (std::size_t band = 0; band < pedalmeasure::bandCount; ++band)
        std::cout << std::setw(6) << std::lround(pedalmeasure::bandCentreHz(band));
    std::cout << '\n';
    for (const auto& signal : result.signals)
    {
        printBands((signal.name + " dB").c_str(), signal.difference);
        std::cout << "      mean |diff| " << std::setprecision(2) << signal.meanAbs << " dB, worst "
                  << std::lround(pedalmeasure::bandCentreHz(signal.worst)) << " Hz at " << std::showpos
                  << signal.difference[signal.worst] << std::noshowpos << " dB\n";
    }
    std::cout << "    THD % at -36/-24/-12/-6 dBFS\n      capture";
    for (const auto value : result.capture.thdPercent) std::cout << std::setw(9) << std::setprecision(2) << value;
    std::cout << "\n      pedal  ";
    for (const auto value : result.pedal.thdPercent) std::cout << std::setw(9) << std::setprecision(2) << value;
    std::cout << "\n    harmonics 2-5 dB re fundamental at -12 dBFS\n      capture";
    for (std::size_t index = 1; index < 5; ++index)
        std::cout << std::setw(9) << std::setprecision(1) << result.capture.harmonics[index];
    std::cout << "\n      pedal  ";
    for (std::size_t index = 1; index < 5; ++index)
        std::cout << std::setw(9) << std::setprecision(1) << result.pedal.harmonics[index];
    std::cout << "\n    alias floor dB re fundamental: capture " << std::setprecision(1)
              << result.capture.aliasFloorDb << ", pedal " << result.pedal.aliasFloorDb << "\n\n";
}

void printPackSummary(const std::vector<RowResult>& results, double offset)
{
    double band = 0.0, worst = 0.0, thd = 0.0;
    std::size_t count = 0;
    for (const auto& result : results)
    {
        if (! result.ok) continue;
        band += result.signals.front().meanAbs;
        worst += std::abs(result.signals.front().difference[result.signals.front().worst]);
        thd += thdCurveError(result.capture, result.pedal);
        ++count;
    }
    const auto n = static_cast<double>(std::max<std::size_t>(count, 1));
    std::cout << "pack summary (" << count << " captures, input offset " << std::showpos << offset
              << std::noshowpos << " dB)\n  mean |band diff| " << std::setprecision(2) << band / n
              << " dB, mean worst band " << worst / n << " dB, mean THD-curve error " << thd / n
              << " dB\n\n";
}

/// Neighbouring grid points of one knob, with the other knob and level held: does the pedal move
/// the way the capture does?
void printKnobChecks(const std::vector<RowResult>& results)
{
    std::cout << "moving-knob checks (guitar phrase, band change from one grid point to the next)\n"
              << "  flag D: capture and pedal move in opposite directions (mean change, or 5+ bands)\n"
              << "  flag M: the mean changes differ by more than 3 dB\n";
    int pairs = 0, direction = 0, magnitude = 0;
    for (const auto knob : { "tone", "dist" })
    {
        const auto isTone = std::string(knob) == "tone";
        std::map<std::string, std::vector<const RowResult*>> groups;
        for (const auto& result : results)
        {
            if (! result.ok) continue;
            const auto& row = result.row;
            groups[num(isTone ? row.dist : row.tone) + "/"
                   + (row.level ? num(*row.level) : "-")].push_back(&result);
        }
        for (auto& [key, group] : groups)
        {
            std::sort(group.begin(), group.end(), [isTone](const RowResult* a, const RowResult* b)
            { return isTone ? a->row.tone < b->row.tone : a->row.dist < b->row.dist; });
            for (std::size_t index = 1; index < group.size(); ++index)
            {
                const auto& from = group[index - 1]->signals.front();
                const auto& to = group[index]->signals.front();
                double captureMean = 0.0, pedalMean = 0.0;
                int opposed = 0;
                for (std::size_t band = 0; band < pedalmeasure::bandCount; ++band)
                {
                    const auto captureStep = to.captureBands[band] - from.captureBands[band];
                    const auto pedalStep = to.pedalBands[band] - from.pedalBands[band];
                    captureMean += captureStep / pedalmeasure::bandCount;
                    pedalMean += pedalStep / pedalmeasure::bandCount;
                    if (std::abs(captureStep) > 1.0 && std::abs(pedalStep) > 1.0
                        && (captureStep > 0.0) != (pedalStep > 0.0))
                        ++opposed;
                }
                const auto flagD = opposed >= 5
                                   || (std::abs(captureMean) > 0.5 && std::abs(pedalMean) > 0.5
                                       && (captureMean > 0.0) != (pedalMean > 0.0));
                const auto flagM = std::abs(captureMean - pedalMean) > 3.0;
                ++pairs;
                direction += flagD ? 1 : 0;
                magnitude += flagM ? 1 : 0;
                std::cout << "  " << knob << " " << (isTone ? group[index - 1]->row.tone : group[index - 1]->row.dist)
                          << "->" << (isTone ? group[index]->row.tone : group[index]->row.dist) << "  (" << key
                          << ")  capture " << std::showpos << std::setprecision(1) << captureMean
                          << "  pedal " << pedalMean << std::noshowpos << " dB  opposed bands " << opposed
                          << (flagD ? "  FLAG D" : "") << (flagM ? "  FLAG M" : "") << '\n';
            }
        }
    }
    std::cout << "  " << pairs << " pairs, " << direction << " direction flags, " << magnitude
              << " magnitude flags\n\n";
}

/// How far apart the packs' captures are at settings they share, as a yardstick for how close a
/// pedal could be expected to get: two real units already disagree by this much.
void printPackSpread(const std::vector<Row>& all, const std::vector<float>& guitar)
{
    std::map<std::string, std::vector<const Row*>> shared;
    for (const auto& row : all) shared[row.settingKey()].push_back(&row);
    std::vector<std::pair<std::string, std::vector<const Row*>>> usable;
    for (auto& [key, rows] : shared)
    {
        std::map<std::string, int> packs;
        for (const auto* row : rows) ++packs[row->pack];
        if (packs.size() > 1) usable.emplace_back(key, rows);
    }
    std::cout << "spread between packs at shared settings (guitar phrase, level matched, input offset 0)\n";
    if (usable.empty()) { std::cout << "  no setting is shared by more than one pack\n\n"; return; }

    std::vector<std::vector<std::vector<double>>> levels(usable.size());
    // Only the first capture per pack at each setting counts, so a pack with duplicates is not
    // measured against itself.
    std::vector<std::pair<std::size_t, const Row*>> tasks;
    for (std::size_t setting = 0; setting < usable.size(); ++setting)
    {
        std::map<std::string, const Row*> firsts;
        for (const auto* row : usable[setting].second) firsts.try_emplace(row->pack, row);
        levels[setting].resize(firsts.size());
        std::size_t slot = 0;
        for (const auto& [pack, row] : firsts) tasks.emplace_back(setting * 1000 + slot++, row);
    }
    parallelFor(tasks.size(), [&](std::size_t task)
    {
        CaptureRenderer capture;
        std::string error;
        if (! capture.load(tasks[task].second->file, error)) return;
        const auto rendered = capture.render(guitar, 0.0);
        levels[tasks[task].first / 1000][tasks[task].first % 1000] = bandsAt(rendered, 0.05);
    });

    double worst = 0.0;
    for (std::size_t setting = 0; setting < usable.size(); ++setting)
    {
        std::erase_if(levels[setting], [](const auto& bands) { return bands.empty(); });
        const auto spread = pedalmeasure::spreadDb(levels[setting]);
        worst = std::max(worst, spread);
        std::cout << "  tone/dist/level " << usable[setting].first << "  " << levels[setting].size()
                  << " packs  spread " << std::fixed << std::setprecision(1) << spread << " dB\n";
    }
    std::cout << "  largest spread " << worst << " dB over " << usable.size() << " shared settings\n\n";
}

// --- bench -------------------------------------------------------------------------------------

int runBench(int model, const std::vector<float>& guitar)
{
    constexpr std::size_t seconds = 10;
    constexpr std::size_t total = seconds * 48000;
    auto board = std::make_unique<nts::pedals::PedalBoard>();
    board->prepare({ workingRate, blockSize, 2 });
    nts::pedals::PedalParameters parameters;
    parameters.model = model;
    for (std::size_t slot = 0; slot < nts::pedals::slotCount; ++slot) board->setParameters(slot, parameters);

    std::array<std::vector<float>, 2> channels;
    for (auto& channel : channels) channel.assign(blockSize, 0.0f);
    std::array<float*, 2> pointers { channels[0].data(), channels[1].data() };
    // Warm the caches and the smoothers before the clock starts.
    for (int block = 0; block < 400; ++block) board->process(pointers.data(), 2, blockSize);

    const auto start = std::chrono::steady_clock::now();
    for (std::size_t done = 0; done < total; done += blockSize)
    {
        for (std::size_t sample = 0; sample < blockSize; ++sample)
            channels[0][sample] = channels[1][sample] = guitar[(done + sample) % guitar.size()];
        board->process(pointers.data(), 2, blockSize);
    }
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::cout << "bench: four slots of " << nts::pedals::pedalModel(model).name << ", " << seconds
              << " s at 48 kHz in " << blockSize << "-sample blocks: " << std::fixed << std::setprecision(3)
              << elapsed << " s, " << std::setprecision(2) << 100.0 * elapsed / static_cast<double>(seconds)
              << "% of one core\n";
    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    std::map<std::string, std::string> options;
    for (int index = 1; index < argc; ++index)
    {
        const std::string name { argv[index] };
        if (name == "--fit-input" || name == "--bench") options[name] = "1";
        else if (name.rfind("--", 0) == 0 && index + 1 < argc) options[name] = argv[++index];
        else { std::cerr << "unknown argument " << name << '\n'; return 2; }
    }
    if (! options.count("--pedal") || (! options.count("--bench") && ! options.count("--manifest")))
    {
        std::cerr << "usage: nts_pedal_compare --manifest <csv> --pack <name> --pedal <modelName>\n"
                     "           [--input-offset-db X | --fit-input] [--wav-dir <dir>] [--di <wav>]\n"
                     "       nts_pedal_compare --bench --pedal <modelName>\n";
        return 2;
    }
    const auto model = findPedal(options["--pedal"]);
    if (model < 0) { std::cerr << "no pedal named " << options["--pedal"] << '\n'; return 2; }

    const auto guitar = makeGuitarPhrase();
    if (options.count("--bench")) return runBench(model, guitar);

    const auto manifest = juce::File::getCurrentWorkingDirectory().getChildFile(juce::String(options["--manifest"]));
    if (! manifest.existsAsFile()) { std::cerr << "no manifest at " << manifest.getFullPathName() << '\n'; return 2; }
    const auto all = readManifest(manifest);
    std::vector<Row> rows;
    for (const auto& row : all)
        if (options["--pack"].empty() || row.pack == options["--pack"]) rows.push_back(row);
    if (rows.empty()) { std::cerr << "no rows for pack '" << options["--pack"] << "'\n"; return 2; }

    std::vector<std::pair<std::string, std::vector<float>>> programs;
    programs.emplace_back("guitar", guitar);
    if (options.count("--di"))
    {
        auto di = loadDi(juce::File::getCurrentWorkingDirectory().getChildFile(juce::String(options["--di"])));
        if (! di) { std::cerr << "could not read " << options["--di"] << '\n'; return 2; }
        programs.emplace_back("di", std::move(*di));
    }
    std::optional<juce::File> wavDir;
    if (options.count("--wav-dir"))
    {
        wavDir = juce::File::getCurrentWorkingDirectory().getChildFile(juce::String(options["--wav-dir"]));
        wavDir->createDirectory();
    }

    std::cout << "pedal: " << nts::pedals::pedalModel(model).name << "   pack: " << options["--pack"]
              << "   " << rows.size() << " captures\n"
              << "mapping: drive = dist, tone = tone, mix = 100, level = knob * 4.8 - 24 dB (0 dB if empty)\n\n";

    double offset = options.count("--input-offset-db") ? std::stod(options["--input-offset-db"]) : 0.0;
    if (options.count("--fit-input")) offset = fitInput(rows, model);

    std::vector<RowResult> results(rows.size());
    parallelFor(rows.size(), [&](std::size_t index)
    { results[index] = analyseRow(rows[index], model, offset, programs, wavDir); });

    for (const auto& result : results) printRow(result);
    printPackSummary(results, offset);
    printKnobChecks(results);
    printPackSpread(all, guitar);
    return 0;
}
