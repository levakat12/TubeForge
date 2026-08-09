#pragma once

#include <cstddef>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

/** A cabinet built from a description rather than sampled from one.

    **Why a model and not a library of measured impulse responses.** Two reasons, and the second is
    the one that matters here. Shipping somebody else's measurements is a licensing question this
    project does not need an answer to; but more usefully, a model has *continuous* microphone
    position and distance, which a shelf of sampled files cannot -- and it can be searched over,
    which is what makes matching a cabinet to a reference recording a tractable problem rather than
    a shortlist of whatever files happen to be installed.

    **Why this is legitimate rather than an EQ curve wearing a cabinet's name.** A guitar speaker is
    a minimum-phase device to a good approximation. That is the property every published discussion
    of converting measured impulse responses to minimum phase leans on, and it cuts both ways: if a
    real cabinet's phase response is recoverable from its magnitude response, then a *synthesised*
    magnitude response reconstructed as minimum phase has a plausible time structure rather than the
    smeared, pre-ringing one a linear-phase filter would give it.

    So the model describes a magnitude response from physical parts -- cone resonance, baffle
    loading, cone breakup, mass roll-off, the microphone's own curve, its angle and its distance --
    and `renderCabinetImpulse` reconstructs the minimum-phase impulse from it.

    What this is **not**: a finite-element model of a speaker. It has no cone, no air and no
    magnet. It is a shaped filter with parameters a player recognises, which is a considerably more
    honest description of what most "cabinet simulators" do than most of them offer.
*/
namespace nts::ir
{
/** Enclosures. Ordering is part of the saved state, so append rather than insert.

    The last three are the cabinets the physical circuit engine used to carry as graph nodes; they
    live here now, so nothing that could be selected before this became one stage disappeared.
*/
enum class CabinetKind
{
    closed4x12,
    open2x12,
    combo1x12,
    bass4x10,
    bass8x10,
    /// Formerly `cabinet.reactive.v1` on the circuit engine.
    reactive2x12,
    /// Formerly `cabinet.open-back.v1`.
    openBack1x12,
    /// Formerly `cabinet.bass-sealed.v1`.
    bassSealed,
    /** The two synthetic decays that were the built-in cabinets before this model existed.

        **Not a voicing anybody would choose, and it has to be here anyway.** Every project saved
        before the model was added was voiced against those two impulses, and a plug-in that
        re-voices somebody's finished track on update is not shipping an improvement, it is
        shipping a regression with a changelog entry. So a project written before schema 6 selects
        this and sounds exactly as it did; a fresh instance gets a real cabinet.

        `renderCabinetImpulse` does **not** produce it -- the original samples live in
        `nts::amp::makeDefaultCabinetImpulse` and are used verbatim, because approximating them
        would defeat the entire point. What this entry contributes here is a magnitude curve for
        the response plot and for the cabinet matcher, and that one *is* an approximation.
    */
    legacy,
    count
};

/** Microphones, by the character people choose them for rather than by model number.

    Named generically on purpose. These are *archetypes* -- a moving-coil with a presence peak, a
    large-diaphragm dynamic, a figure-of-eight ribbon, a condenser -- and naming them after the
    boxes they resemble would claim a measurement that has not been made.
*/
enum class MicrophoneKind
{
    /// Moving coil, presence peak around 5 kHz, no deep bottom. The default studio answer.
    dynamicPresence,
    /// Large-diaphragm dynamic: fuller low-mid, gentler top.
    dynamicFull,
    /// Figure-of-eight ribbon: rolls off hard above 5 kHz, strong proximity effect.
    ribbon,
    /// Condenser: extended at both ends, and hears the room sooner than the others do.
    condenser,
    /// A pair out in the room rather than on the grille. Distance does most of the work.
    roomPair,
    count
};

struct CabinetModelSettings
{
    CabinetKind cabinet { CabinetKind::closed4x12 };
    MicrophoneKind microphone { MicrophoneKind::dynamicPresence };
    /** Where on the cone the microphone points: 0 is the dust cap, 1 is the cone edge.

        The single most useful control on a real cabinet, and the one a shelf of sampled impulse
        responses turns into three or four discrete files. On axis at the cap is bright and hard;
        at the edge the top end is gone and the low-mid comes up. Continuous here because that is
        what moving a microphone actually is.
    */
    float position { 0.35f };
    /// Grille to capsule, in inches. Proximity effect below about six inches, room above about
    /// twelve. Clamped to 1..24 by the renderer.
    float distanceInches { 2.0f };
    bool operator==(const CabinetModelSettings&) const = default;
};

[[nodiscard]] std::string_view cabinetName(CabinetKind kind) noexcept;
[[nodiscard]] std::string_view microphoneName(MicrophoneKind kind) noexcept;

/** The model's magnitude response at one frequency, in decibels.

    Exposed because the response plot draws it and the cabinet matcher scores against it, and
    neither should have to render an impulse and analyse it back to ask a question the model can
    answer directly. `renderCabinetImpulse` is defined in terms of this, so the curve drawn and the
    curve heard cannot drift apart.
*/
[[nodiscard]] float cabinetMagnitudeDb(const CabinetModelSettings& settings, float frequencyHz) noexcept;

/** Renders the model to a minimum-phase impulse response.

    `taps` is rounded up to a power of two internally and the result is trimmed back to it, with a
    short fade so the truncation is not a step -- a step in an impulse is broadband splatter at the
    top of the spectrum.

    Message-thread work: it allocates and runs two transforms. The result goes to the cabinet
    section through the same crossfaded load path a user's file takes, so the audio thread learns
    nothing new.
*/
[[nodiscard]] std::vector<float> renderCabinetImpulse(const CabinetModelSettings& settings,
                                                       double sampleRate, std::size_t taps = 512);

/** Builds a minimum-phase impulse response from a magnitude response.

    `magnitudeDb(frequencyHz)` is asked for the response at each analysis bin. It must be finite
    everywhere, including at the very bottom of the spectrum: the construction takes the logarithm
    of what it is handed, and a magnitude of zero is a logarithm of minus infinity.

    Shared by `renderCabinetImpulse` and by the cabinet matcher rather than written twice, because
    the two traps in it are not obvious and are silent when tripped. The cepstral method is defined
    on the **natural** logarithm -- feeding it decibels produces a filter 8.686 times too
    aggressive, which does not look wrong so much as unhinged -- and the transform has to be
    several times the requested tap count or the impulse's tail wraps onto its own head.

    The result is peak-normalised to -1 dBFS and faded out over its last eighth, so it can be
    handed straight to a convolver.
*/
[[nodiscard]] std::vector<float> minimumPhaseImpulse(
    const std::function<float(float)>& magnitudeDb, double sampleRate, std::size_t taps);

/** Rebuilds a measured response as its minimum-phase equivalent, keeping its magnitude exactly.

    **What this is for.** Two impulse responses from different sources rarely start at the same
    place: one may carry a millisecond of the microphone's flight time, another may have been
    trimmed hard, and a third may have been captured with a linear-phase filter somewhere in the
    chain. Blend two of those and the difference between their arrival times combs the sum, which
    is heard as a hollow, phasey blend that no amount of moving the Blend control fixes. Converting
    both to minimum phase removes the difference without touching either one's tone -- the
    magnitude response is preserved by construction, which is the whole point of the transform.

    Off by default, because it is not free: it discards whatever real phase structure the response
    had, and for a *single* response that structure is part of what was measured.

    `taps` is the output length. Uses the same cepstral construction as `minimumPhaseImpulse`, and
    is a separate entry point only because taking the magnitude from a transform of the response is
    O(N log N) where sampling it through a callback would be O(N) per point.
*/
[[nodiscard]] std::vector<float> minimumPhaseFromImpulse(std::span<const float> impulse,
                                                          std::size_t taps);
} // namespace nts::ir
