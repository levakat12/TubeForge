#pragma once

#include <nts/reconstruction/SourceReconstruction.h>

#include <cstddef>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace nts::reconstruction
{
inline constexpr std::string_view currentRefinementVersion = "tubeforge-wiener-refine-v1";

/** Options for the multi-stem Wiener post-filter.

    Separation backends decode every source independently, so the same snare
    transient is emitted in the drum stem *and* left sitting in the guitar stem.
    Nothing downstream can tell the two apart. This pass re-partitions the mixture
    across the stems so each time-frequency bin is awarded to whichever source
    actually dominates it, which is what turns "mostly guitar" into "guitar".
*/
struct StemRefinementOptions
{
    std::size_t fftSize { 4096 };
    std::size_t hopSize { 1024 };

    /** Mask sharpness. 1.0 is a magnitude-proportional split, 2.0 the MMSE-optimal
        Wiener mask, and above that the masks go progressively more binary and start
        producing musical noise. 1.0 and 2.0 are also the two values that need no
        transcendental per bin, which is what keeps a full song affordable.
    */
    float maskExponent { 2.0f };

    /** Mask re-estimation passes. Each pass sharpens the previous masks; one pass
        is the usual choice, more trades artefacts for isolation.
    */
    int iterations { 1 };

    /** Lower bound on any mask, so a stem is attenuated rather than annihilated in
        bins it does not own. Suppresses the "birdies" a hard mask produces.
    */
    float maskFloor { 0.02f };

    /** Model the part of the mixture no supplied stem explains as an extra virtual
        source. Without it, a partially loaded StemSet — the target-only Demucs path
        keeps vocals, drums and one instrument — would force every unmodelled source
        (piano, keys, the rest of `other`) into the stems that are present.
    */
    bool modelResidual { true };
};

struct StemRefinementReport
{
    bool applied {};
    std::string version { std::string(currentRefinementVersion) };

    /** Share of the mixture magnitude that no supplied stem accounted for, 0..1.
        Near zero when the StemSet partitions the mixture; substantial when stems
        were omitted, in which case that energy is parked in the virtual residual
        instead of being pushed into the target.
    */
    float residualShare {};

    /** Mean per-stem energy change, in dB. Negative is the expected direction: the
        stems gave up the bins they did not own.
    */
    float energyChangeDb {};

    std::size_t stemsRefined {};
    std::vector<std::string> warnings;
};

/** Re-masks every non-empty stem in `stems` against `mixture`, in place.

    `mixture` must be sample-aligned with the stems and at the same rate; both the
    CPU fallback and the Demucs loader already guarantee this. Stems left empty by
    the backend are skipped, not fabricated.
*/
[[nodiscard]] StemRefinementReport refineStems(StemSet& stems, const StereoAudio& mixture,
                                               const StemRefinementOptions& options = {},
                                               const ProgressCallback& progress = {},
                                               std::stop_token stopToken = {});
} // namespace nts::reconstruction
