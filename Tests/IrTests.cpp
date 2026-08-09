#include "TestHarness.h"

#include <nts/dsp/Analysis.h>
#include <nts/ir/CabinetIrLoader.h>
#include <nts/ir/CabinetLibrary.h>
#include <nts/ir/CabinetMatch.h>
#include <nts/ir/CabinetModel.h>

#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <memory>
#include <numeric>
#include <random>
#include <thread>

namespace
{
/// The rendered impulse's own magnitude response at one frequency, by direct evaluation of the
/// DFT sum. Slow, and that is fine for a handful of probe frequencies -- what it buys is that the
/// test measures the *impulse* rather than re-running the model that produced it.
[[nodiscard]] float renderedMagnitudeDb(const std::vector<float>& impulse, float frequencyHz,
                                        double sampleRate)
{
    std::complex<double> sum {};
    for (std::size_t index = 0; index < impulse.size(); ++index)
    {
        const auto angle = -2.0 * 3.14159265358979323846 * frequencyHz
                         * static_cast<double>(index) / sampleRate;
        sum += std::complex<double>(impulse[index], 0.0)
             * std::complex<double>(std::cos(angle), std::sin(angle));
    }
    return static_cast<float>(20.0 * std::log10(std::max(1.0e-9, std::abs(sum))));
}

/** The model, and the one property that makes it a cabinet rather than an EQ curve.

    Three claims are worth testing and only three. That the rendered impulse actually has the shape
    the model describes -- otherwise the response plot and the sound disagree. That it is
    minimum-phase, meaning its energy is packed at the front, because that is the assumption the
    whole construction rests on. And that the controls do what their names say, since a model whose
    microphone position is inaudible is a table of numbers rather than a cabinet.
*/
void testCabinetModel(TestHarness& tests)
{
    constexpr double sampleRate = 48000.0;
    const nts::ir::CabinetModelSettings closed {};

    const auto impulse = nts::ir::renderCabinetImpulse(closed, sampleRate, 512);
    tests.expectEqual(impulse.size(), std::size_t { 512 }, "the model renders the requested length");

    auto finite = true;
    for (const auto sample : impulse) finite = finite && std::isfinite(sample);
    tests.expect(finite, "the rendered impulse is finite throughout");

    /* The rendered impulse has to match the curve the model claims.

       Compared as a *shape* rather than absolutely: the impulse is peak-normalised to -1 dBFS, so
       its absolute level is not the model's to predict. Two probes either side of the top-end
       corner, differenced, is the strongest single statement -- a closed 4x12 falls off a cliff
       above 5 kHz and any implementation that lost the reconstruction would not. */
    const auto modelSlope = nts::ir::cabinetMagnitudeDb(closed, 1000.0f)
                          - nts::ir::cabinetMagnitudeDb(closed, 8000.0f);
    const auto renderedSlope = renderedMagnitudeDb(impulse, 1000.0f, sampleRate)
                             - renderedMagnitudeDb(impulse, 8000.0f, sampleRate);
    tests.expectNear(renderedSlope, modelSlope, 3.0,
                     "the rendered impulse carries the model's own top-end slope: "
                         + std::to_string(renderedSlope) + " dB against " + std::to_string(modelSlope));
    tests.expect(modelSlope > 12.0f,
                 "a closed 4x12 loses a great deal above 5 kHz: " + std::to_string(modelSlope) + " dB");

    /* Minimum phase, stated as the thing it actually means: the energy is at the front.

       A linear-phase reconstruction of the same magnitude response would put the peak in the
       middle and ring symmetrically either side of it, which is both wrong for a speaker and
       audible as pre-echo on transients. */
    const auto totalEnergy = std::accumulate(impulse.begin(), impulse.end(), 0.0,
        [](double sum, float sample) { return sum + static_cast<double>(sample) * sample; });
    auto centroid = 0.0;
    for (std::size_t index = 0; index < impulse.size(); ++index)
        centroid += static_cast<double>(index) * impulse[index] * impulse[index];
    centroid /= std::max(1.0e-12, totalEnergy);
    std::size_t peakIndex {};
    for (std::size_t index = 0; index < impulse.size(); ++index)
        if (std::abs(impulse[index]) > std::abs(impulse[peakIndex])) peakIndex = index;
    // The centroid is the assertion that discriminates: a linear-phase reconstruction of the same
    // magnitude response has its centroid at the midpoint by construction, so anything well under
    // that is the minimum-phase one. The peak index is the second half of the same statement --
    // pre-ringing would move it away from the start.
    tests.expect(centroid < static_cast<double>(impulse.size()) * 0.25,
                 "the response is minimum-phase: its energy centroid is near the front at "
                     + std::to_string(centroid) + " of " + std::to_string(impulse.size()));
    tests.expect(peakIndex < 8,
                 "the response has no pre-ringing: its peak is at sample " + std::to_string(peakIndex));

    // Microphone position: the cap is brighter than the edge, which is the whole reason the
    // control exists and is the first thing anyone checks by ear.
    auto capPosition = closed; capPosition.position = 0.0f;
    auto edgePosition = closed; edgePosition.position = 1.0f;
    const auto capTop = nts::ir::cabinetMagnitudeDb(capPosition, 3500.0f);
    const auto edgeTop = nts::ir::cabinetMagnitudeDb(edgePosition, 3500.0f);
    tests.expect(capTop > edgeTop + 4.0f,
                 "on the dust cap is brighter than at the cone edge: "
                     + std::to_string(capTop - edgeTop) + " dB");

    // Distance: proximity effect adds bottom close up. A ribbon is figure-of-eight and gets far
    // more of it than a moving coil, which is why the two are not interchangeable up close.
    auto closeRibbon = closed; closeRibbon.microphone = nts::ir::MicrophoneKind::ribbon;
    closeRibbon.distanceInches = 1.0f;
    auto farRibbon = closeRibbon; farRibbon.distanceInches = 18.0f;
    const auto closeLow = nts::ir::cabinetMagnitudeDb(closeRibbon, 90.0f)
                        - nts::ir::cabinetMagnitudeDb(closeRibbon, 1000.0f);
    const auto farLow = nts::ir::cabinetMagnitudeDb(farRibbon, 90.0f)
                      - nts::ir::cabinetMagnitudeDb(farRibbon, 1000.0f);
    tests.expect(closeLow > farLow + 2.0f,
                 "proximity effect adds low end close up: " + std::to_string(closeLow - farLow) + " dB");

    // A bass cabinet reaches lower than a guitar one. If this ever stops holding, the table has
    // been edited into nonsense and no amount of downstream tuning will recover it.
    nts::ir::CabinetModelSettings bass; bass.cabinet = nts::ir::CabinetKind::bass8x10;
    tests.expect(nts::ir::cabinetMagnitudeDb(bass, 45.0f)
                     > nts::ir::cabinetMagnitudeDb(closed, 45.0f) + 6.0f,
                 "a bass 8x10 reaches lower than a closed 4x12");

    // Every entry in both tables has to render. A voicing appended with a typo in its numbers is
    // otherwise found by whoever first selects it.
    auto everyCombinationRenders = true;
    for (std::size_t cabinet = 0; cabinet < static_cast<std::size_t>(nts::ir::CabinetKind::count); ++cabinet)
        for (std::size_t microphone = 0;
             microphone < static_cast<std::size_t>(nts::ir::MicrophoneKind::count); ++microphone)
        {
            nts::ir::CabinetModelSettings settings;
            settings.cabinet = static_cast<nts::ir::CabinetKind>(cabinet);
            settings.microphone = static_cast<nts::ir::MicrophoneKind>(microphone);
            const auto rendered = nts::ir::renderCabinetImpulse(settings, sampleRate, 256);
            auto peak = 0.0f;
            for (const auto sample : rendered)
            {
                everyCombinationRenders = everyCombinationRenders && std::isfinite(sample);
                peak = std::max(peak, std::abs(sample));
            }
            everyCombinationRenders = everyCombinationRenders && peak > 0.5f && peak <= 1.0f;
        }
    tests.expect(everyCombinationRenders,
                 "every cabinet and microphone pairing renders a finite, normalised response");
}
/** Cabinet matching, tested against the one thing here that has an exact right answer.

    The claim is narrow and checkable: feed the estimator a signal and the *same signal through a
    known filter*, and the residual it recovers must be that filter, within the bandwidth it
    smooths over. Everything else about this feature is a judgement call -- how much correction is
    too much, whether a library pick beats a synthesised curve -- but this is arithmetic, and if it
    is wrong nothing built on top of it can be right.
*/
void testCabinetMatch(TestHarness& tests)
{
    constexpr double sampleRate = 48000.0;
    constexpr std::size_t length = 48000 * 2;

    /* Pink-ish noise rather than a sine or a sweep.

       The estimator averages power per band, so it needs energy in every band it is going to
       report on. A sine measures one of them and leaves forty reading silence; a sweep would work
       but has to be long enough to dwell in each band, and noise simply is that signal. Seeded
       deterministically so a failure is reproducible. */
    std::mt19937 generator { 20260808u };
    std::uniform_real_distribution<float> distribution { -1.0f, 1.0f };
    std::vector<float> source(length);
    auto running = 0.0f;
    for (auto& sample : source)
    {
        const auto white = distribution(generator);
        running = 0.97f * running + 0.03f * white;
        sample = 0.35f * (white * 0.4f + running * 6.0f);
    }

    /* The known filter: a one-pole low-pass at 2 kHz, applied to make the "reference".

       Chosen because its magnitude response is something the test can state in closed form and
       compare against -- -3 dB at the corner and 6 dB per octave above it -- and because a
       roll-off is the shape a cabinet difference actually takes. */
    constexpr auto cornerHz = 2000.0f;
    const auto coefficient = static_cast<float>(
        std::exp(-2.0 * 3.14159265358979323846 * cornerHz / sampleRate));
    std::vector<float> filtered(length);
    auto state = 0.0f;
    for (std::size_t index = 0; index < length; ++index)
    {
        state = source[index] * (1.0f - coefficient) + state * coefficient;
        filtered[index] = state;
    }

    const auto residual = nts::ir::measureCabinetResidual(filtered, source, sampleRate);
    tests.expect(residual.valid, "the estimator measures two comparable signals: " + residual.error);
    if (! residual.valid) return;
    tests.expectEqual(residual.residualDb.size(), nts::ir::cabinetMatchBands,
                      "one residual value per band");

    /* The recovered residual against the filter's own response.

       Compared as a *slope* between two probe bands rather than absolutely, because the estimator
       levels both spectra to their own means before differencing -- so the residual is a shape and
       carries no absolute offset. A one-pole at 2 kHz loses very close to 12 dB between 2 kHz and
       8 kHz, and that is what has to come back. */
    const auto valueAt = [&residual](float frequency)
    {
        auto best = std::size_t {};
        for (std::size_t band = 0; band < residual.frequencies.size(); ++band)
            if (std::abs(std::log(residual.frequencies[band] / frequency))
                < std::abs(std::log(residual.frequencies[best] / frequency)))
                best = band;
        return residual.residualDb[best];
    };
    const auto recoveredSlope = valueAt(2000.0f) - valueAt(8000.0f);
    const auto expectedSlope = 20.0f * std::log10(
        std::sqrt(1.0f + std::pow(8000.0f / cornerHz, 2.0f))
        / std::sqrt(1.0f + std::pow(2000.0f / cornerHz, 2.0f)));
    tests.expectNear(recoveredSlope, expectedSlope, 2.5,
                     "the residual recovers the known filter's slope: "
                         + std::to_string(recoveredSlope) + " dB against "
                         + std::to_string(expectedSlope) + " dB");

    // A signal against itself has nothing to correct, and must say so rather than fitting noise.
    const auto identical = nts::ir::measureCabinetResidual(source, source, sampleRate);
    tests.expect(identical.valid, "identical signals still measure");
    auto largestIdenticalResidual = 0.0f;
    for (const auto value : identical.residualDb)
        largestIdenticalResidual = std::max(largestIdenticalResidual, std::abs(value));
    tests.expect(largestIdenticalResidual < 0.5f,
                 "a signal against itself needs no correction: largest band "
                     + std::to_string(largestIdenticalResidual) + " dB");
    tests.expect(identical.confidence > 0.9f,
                 "and reports high confidence: " + std::to_string(identical.confidence));
    tests.expect(residual.confidence < identical.confidence,
                 "a real correction is reported less confidently than none at all");

    // Too short to measure is an error rather than a spectrum of silence, which would otherwise be
    // differenced against something real and produce a confident, meaningless answer.
    const std::vector<float> tooShort(128, 0.1f);
    tests.expect(! nts::ir::measureCabinetResidual(tooShort, source, sampleRate).valid,
                 "a reference too short to measure is refused rather than guessed at");

    /* The library pick. The residual above is a top-end loss, so whatever wins has to be darker
       than the measurement cabinet -- and the score has to mean something, in that the winner
       leaves less behind than it started with. */
    const nts::ir::CabinetModelSettings measurement {};
    const auto candidates = nts::ir::matchFromLibrary(residual, measurement);
    tests.expect(! candidates.empty(), "the library returns a shortlist");
    if (! candidates.empty())
    {
        tests.expect(candidates.front().score >= candidates.back().score,
                     "the shortlist is ordered best first");
        tests.expect(candidates.front().score > 0.0f,
                     "the best candidate removes some of the residual: "
                         + std::to_string(candidates.front().score));
        // Legacy exists so old projects keep their sound; offering it as a best match would be
        // nonsense, and it is excluded by construction.
        auto offersLegacy = false;
        for (const auto& candidate : candidates)
            offersLegacy = offersLegacy
                || candidate.settings.cabinet == nts::ir::CabinetKind::legacy;
        tests.expect(! offersLegacy, "the legacy stand-in is never offered as a match");
    }

    /* The synthesised cabinet. At zero depth it must be the measurement cabinet unchanged --
       that is what makes the depth control safe to reach for -- and at full depth it must be
       audibly darker, because the residual it is applying is a top-end loss. */
    const auto atZero = nts::ir::synthesizeMatchedCabinet(residual, measurement, 0.0f, sampleRate, 512);
    const auto atFull = nts::ir::synthesizeMatchedCabinet(residual, measurement, 1.0f, sampleRate, 512);
    const auto reference = nts::ir::renderCabinetImpulse(measurement, sampleRate, 512);
    tests.expectEqual(atZero.size(), std::size_t { 512 }, "the synthesised cabinet is the length asked for");
    auto zeroMatchesModel = atZero.size() == reference.size();
    if (zeroMatchesModel)
        for (std::size_t index = 0; index < atZero.size(); ++index)
            zeroMatchesModel = zeroMatchesModel && std::abs(atZero[index] - reference[index]) < 1.0e-5f;
    tests.expect(zeroMatchesModel,
                 "at zero depth the synthesised cabinet is the measurement cabinet unchanged");

    const auto darkness = [&](const std::vector<float>& impulse)
    {
        return renderedMagnitudeDb(impulse, 1000.0f, sampleRate)
             - renderedMagnitudeDb(impulse, 6000.0f, sampleRate);
    };
    tests.expect(darkness(atFull) > darkness(atZero) + 4.0f,
                 "at full depth it applies the measured top-end loss: "
                     + std::to_string(darkness(atFull) - darkness(atZero)) + " dB darker");
}
/** Converting a measured response to minimum phase.

    The property that makes it worth doing at all is that the *magnitude* is untouched: it removes
    the arrival-time difference that combs two blended responses without changing either one's
    tone. If that ever stops holding, the control becomes a tone control with a misleading name.
*/
void testMinimumPhaseConversion(TestHarness& tests)
{
    constexpr double sampleRate = 48000.0;

    /* A response with a deliberate delay in front of it, which is the case this exists for: a
       measurement that carries the microphone's flight time starts late, and blending it against
       one that does not is what combs. */
    std::vector<float> measured(512, 0.0f);
    measured[40] = 0.8f;
    measured[47] = -0.35f;
    for (std::size_t index = 48; index < 200; ++index)
        measured[index] = 0.25f * std::exp(-static_cast<float>(index - 48) / 30.0f)
                        * std::sin(0.2f * static_cast<float>(index));

    const auto converted = nts::ir::minimumPhaseFromImpulse(measured, measured.size());
    tests.expectEqual(converted.size(), measured.size(), "the conversion keeps the length asked for");

    // The magnitude response is preserved. Probed across the band rather than at one frequency,
    // because a construction that got the fold wrong can still agree at a single point.
    auto largestDifference = 0.0f;
    for (const auto frequency : { 80.0f, 250.0f, 800.0f, 2500.0f, 6000.0f })
        largestDifference = std::max(largestDifference,
            std::abs(renderedMagnitudeDb(converted, frequency, sampleRate)
                     - renderedMagnitudeDb(measured, frequency, sampleRate)));
    tests.expect(largestDifference < 1.0f,
                 "minimum-phase conversion preserves the magnitude response: largest difference "
                     + std::to_string(largestDifference) + " dB");

    // And the delay is gone, which is the point. The measured response has nothing in its first
    // forty samples; the converted one has its peak there.
    std::size_t convertedPeak {};
    for (std::size_t index = 0; index < converted.size(); ++index)
        if (std::abs(converted[index]) > std::abs(converted[convertedPeak])) convertedPeak = index;
    tests.expect(convertedPeak < 8,
                 "the leading delay is removed: the peak moved to sample "
                     + std::to_string(convertedPeak) + " from 40");

    // Level is matched to the input rather than normalised to a fixed target: converting a
    // response must not also change how loud it is, or every blend a user had set would move.
    const auto peakOf = [](const std::vector<float>& impulse)
    {
        auto peak = 0.0f;
        for (const auto sample : impulse) peak = std::max(peak, std::abs(sample));
        return peak;
    };
    tests.expectNear(peakOf(converted), peakOf(measured), 1.0e-4,
                     "conversion preserves the response's level");

    // A response with an exact null in its spectrum is the case that turns a naive implementation
    // into NaN, because the construction takes a logarithm. Two equal impulses a fixed distance
    // apart is the simplest way to produce one.
    std::vector<float> combed(256, 0.0f);
    combed[0] = 0.5f;
    combed[16] = -0.5f;
    const auto combedResult = nts::ir::minimumPhaseFromImpulse(combed, combed.size());
    auto finite = ! combedResult.empty();
    for (const auto sample : combedResult) finite = finite && std::isfinite(sample);
    tests.expect(finite, "a response with exact spectral nulls converts without producing NaN");
}
/** The impulse-response browser's library.

    The interesting claims are about what it *refuses* to do: it scans rather than copies, so it
    cannot disagree with the disk; it is bounded, so a user who points it at a drive root gets a
    truncated list that says it is truncated rather than a hang; and its favourites and recents are
    keyed by path, so a file that moves stops appearing instead of pointing at whatever took its
    place.
*/
void testCabinetLibrary(TestHarness& tests)
{
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getNonexistentChildFile("tubeforge-ir-library", "");
    const auto marshall = root.getChildFile("Marshall");
    const auto vox = root.getChildFile("Vox");
    marshall.createDirectory();
    vox.createDirectory();

    // Four responses across two folders, plus a file the loader cannot read. The last one is the
    // case that matters: the browser must not offer something the loader will then refuse.
    const auto write = [](const juce::File& file)
    {
        juce::AudioBuffer<float> buffer(1, 64);
        buffer.clear();
        buffer.setSample(0, 0, 0.9f);
        juce::WavAudioFormat format;
        std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
        auto writer = format.createWriterFor(stream, juce::AudioFormatWriterOptions {}
                                                         .withSampleRate(48000.0)
                                                         .withNumChannels(1)
                                                         .withBitsPerSample(16));
        if (writer != nullptr) writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());
    };
    write(marshall.getChildFile("Greenback 57 cap.wav"));
    write(marshall.getChildFile("Greenback 57 edge.wav"));
    write(vox.getChildFile("Blue 121.wav"));
    write(root.getChildFile("Loose response.wav"));
    root.getChildFile("readme.txt").replaceWithText("not an impulse response");

    const auto preferences = root.getChildFile("preferences.json");
    {
        nts::ir::CabinetLibrary library(preferences);
        library.setRootDirectory(root);
        tests.expectEqual(library.entries().size(), std::size_t { 4 },
                          "the scan finds every response and nothing else");
        tests.expect(! library.truncated(), "a small folder does not report a truncated scan");

        const auto folders = library.folders();
        tests.expectEqual(folders.size(), std::size_t { 2 },
                          "responses are filed under the folders their pack already uses");

        // A favourite sorts to the top. Sorting rather than filtering: it is a shortcut to the top
        // of the list, not a different list, so the count must not change.
        const auto blue = vox.getChildFile("Blue 121.wav");
        library.setFavourite(blue, true);
        library.refresh();
        tests.expectEqual(library.entries().size(), std::size_t { 4 },
                          "favouriting does not remove anything from the list");
        tests.expect(! library.entries().empty() && library.entries().front().file == blue,
                     "a favourite sorts to the top");

        library.noteUsed(marshall.getChildFile("Greenback 57 cap.wav"));
        library.noteUsed(blue);
        const auto recent = library.recents();
        tests.expect(recent.size() == 2 && recent.front().file == blue,
                     "recents are newest first");
    }

    // Reopened: the root, the favourite and the recents have to survive, because they are the
    // whole difference between a browser and a file dialog.
    {
        nts::ir::CabinetLibrary reopened(preferences);
        tests.expect(reopened.rootDirectory() == root, "the chosen folder is remembered");
        tests.expect(reopened.isFavourite(vox.getChildFile("Blue 121.wav")),
                     "favourites survive a reload");
        tests.expectEqual(reopened.recents().size(), std::size_t { 2 },
                          "recents survive a reload");

        // A recent that no longer exists is filtered, not resurrected as some other file.
        marshall.getChildFile("Greenback 57 cap.wav").deleteFile();
        tests.expectEqual(reopened.recents().size(), std::size_t { 1 },
                          "a recent whose file has gone stops being offered");
    }

    root.deleteRecursively();
}
} // namespace

int main()
{
    TestHarness tests;
    testCabinetModel(tests);
    testCabinetMatch(tests);
    testMinimumPhaseConversion(tests);
    testCabinetLibrary(tests);
    const auto file = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getNonexistentChildFile("tubeforge-cabinet-test", ".wav");
    {
        juce::AudioBuffer<float> buffer(1, 128);
        buffer.clear(); buffer.setSample(0, 8, 0.8f); buffer.setSample(0, 12, -0.2f);
        juce::WavAudioFormat format;
        std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
        auto writer = stream != nullptr
            ? format.createWriterFor(stream, juce::AudioFormatWriterOptions {}
                                                 .withSampleRate(24000.0)
                                                 .withNumChannels(1)
                                                 .withBitsPerSample(16))
            : nullptr;
        tests.expect(writer != nullptr, "test cabinet WAV writer opens");
        if (writer != nullptr)
        {
            tests.expect(writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples()),
                         "test cabinet WAV is written");
        }
    }

    std::atomic<bool> completed {};
    nts::ir::CabinetLoadResult result;
    {
        nts::ir::CabinetIrLoader loader;
        nts::dsp::ImpulsePreparationOptions options;
        options.outputChannels = 2;
        loader.loadAsync(file, 48000.0, options,
                         [&result, &completed](nts::ir::CabinetLoadResult loaded)
                         {
                             result = std::move(loaded);
                             completed.store(true, std::memory_order_release);
                         });
        for (int attempt = 0; attempt < 1000 && ! completed.load(std::memory_order_acquire); ++attempt)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    tests.expect(completed.load(std::memory_order_acquire), "cabinet IR worker completes asynchronously");
    tests.expect(static_cast<bool>(result), "cabinet IR file decodes and preprocesses: " + result.error);
    if (result)
    {
        tests.expectEqual(result.impulse.channels.size(), std::size_t { 2 }, "cabinet loader converts mono to stereo");
        tests.expect(result.impulse.channels[0].size() > 128, "cabinet loader resamples to engine rate");
    }
    file.deleteFile();
    return tests.result();
}
