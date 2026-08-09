#include "nts/pedals/PedalBoard.h"

#include <algorithm>
#include <cmath>
#include <numbers>

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

namespace
{
/** The model table.

    Entries 0 to 6 are the built-in archetypes and their indices are saved project state, so
    they never move and their voicings are exactly what they have always been. Named units
    append from index 7.

    On naming: the modelled units are given allusive names rather than the names of the
    products they are inspired by. What is being modelled is a circuit topology, which is not
    anybody's trademark; the name on the box is. The catalogue these are drawn from records the
    originals, and that is the right place for them.
*/
constexpr std::array<PedalModel, 62> modelTable { {
    { "Empty", "", "No pedal in this slot. Not bypassed -- absent, and it costs nothing.",
      PedalEngine::silent, 1, {}, {} },

    { "Boost", "", "A clean lift. Drive is how hard the amplifier's own front end gets pushed, "
                   "not how dirty this is.",
      PedalEngine::shaper, 1,
      { "Drive", "Tone", "Level", "Mix", "", "" },
      // Full-range, barely any curve, tone opens all the way up. A clean level lift into the
      // amplifier's own front end, which is what most of a pedalboard is for.
      { 30.0, 0.0f, 24.0f, dsp::Waveshape::hyperbolicTangent, 0.0f, 2000.0, 16000.0, -1.0f, false } },

    { "Overdrive", "", "Clips only what is above 720 Hz and sums it back with the untouched "
                       "signal. That mid hump is why it stays articulate.",
      PedalEngine::shaper, 2,
      { "Drive", "Tone", "Level", "Mix", "", "" },
      // Clips only what is above 720 Hz and sums it back with the untouched input. That
      // parallel path is the mid hump, and it is why this stays articulate where the
      // distortion below does not.
      { 720.0, 6.0f, 34.0f, dsp::Waveshape::diode, 0.0f, 1200.0, 6000.0, -7.0f, true } },

    { "Distortion", "", "Everything through a soft clipper at high gain. Tone is the only thing "
                        "standing between it and a fizz.",
      PedalEngine::shaper, 2,
      { "Drive", "Tone", "Level", "Mix", "", "" },
      { 100.0, 10.0f, 42.0f, dsp::Waveshape::softClip, 0.0f, 800.0, 8000.0, -13.0f, false } },

    { "Fuzz", "", "Highest gain, asymmetric, and high-passed hard at the input so low notes "
                  "stay notes.",
      PedalEngine::shaper, 2,
      { "Drive", "Tone", "Level", "Mix", "", "" },
      // Highest gain, asymmetric, and high-passed hard at the input because a fuzz fed full
      // low end collapses into a splutter rather than a note.
      { 60.0, 20.0f, 52.0f, dsp::Waveshape::asymmetricPolynomial, 0.18f, 700.0, 6000.0, -11.0f, false } },

    { "Compressor", "", "One knob: Drive moves threshold and ratio together. Make-up gain "
                        "follows automatically.",
      PedalEngine::compressor, 2,
      { "Amount", "Tone", "Level", "Mix", "", "" },
      // The shaping fields are unused; it runs the dynamics processor instead, and keeps only
      // the tone control, which stays open by default.
      { 20.0, 0.0f, 0.0f, dsp::Waveshape::hyperbolicTangent, 0.0f, 2000.0, 18000.0, 0.0f, false } },

    { "Neural capture", "", "Plays a converted .nam pedal capture. Drive sets the level going "
                            "into the model, which a capture is most sensitive to.",
      PedalEngine::neural, 3,
      { "Drive", "Tone", "Level", "Mix", "", "" },
      /* The model is the voicing. Drive becomes the level going into it, which a capture is
         genuinely sensitive to, and tone a post-model roll-off that is effectively open at the
         top of its travel.

         -6 to +24 rather than -12 to +12. The old range put Drive 5 at exactly 0 dB, and with
         no input compensation -- see configureNeural, which nothing was calling -- that was
         unity into a model expecting a specific level, so the knob's whole lower half did
         nothing audible and only the very top produced any character. The other models span 24
         to 32 dB into their clippers; this was the one whose drive control could not drive
         anything. Compensation now lands the calibrated level near the middle of the travel
         and the top pushes 24 dB past it.

         What this cannot fix: a `.nam` pedal capture is a snapshot at one knob position. If it
         was captured at low drive, no amount of input gain reproduces a high-drive pedal. That
         is the format, not the range. */
      { 20.0, -6.0f, 24.0f, dsp::Waveshape::hyperbolicTangent, 0.0f, 2000.0, 20000.0, 0.0f, false } },

    // ---- Overdrives ------------------------------------------------------------------------
    // The family is defined by parallel clipping: a band above the input high-pass is clipped
    // and summed back onto the untouched signal, so the low end stays clean and a mid hump
    // appears where the two meet. Move that corner and the whole character moves with it.

    { "Green Scream", "TubeForge", "The classic mid-hump overdrive. Clips above 720 Hz and sums it "
                                   "back, so the low end stays clean and the mids push forward.",
      PedalEngine::shaper, 2, { "Drive", "Tone", "Level", "Mix", "", "" },
      { 720.0, 6.0f, 34.0f, dsp::Waveshape::diode, 0.0f, 1200.0, 6000.0, -7.0f, true } },

    { "Amber Scream", "TubeForge", "The same topology with a slightly lower corner and more open "
                                   "top. Rounder and a little louder than the green one.",
      PedalEngine::shaper, 2, { "Drive", "Tone", "Level", "Mix", "", "" },
      { 620.0, 6.0f, 33.0f, dsp::Waveshape::diode, 0.0f, 1400.0, 7000.0, -6.5f, true } },

    { "Mythic Gold", "TubeForge", "Germanium clipping across a wide clean band. Very transparent "
                                  "-- it thickens the lower mids rather than distorting.",
      PedalEngine::shaper, 3, { "Gain", "Treble", "Output", "Mix", "", "" },
      // Germanium's low forward voltage and soft knee, over an almost full-range parallel path.
      { 180.0, 0.0f, 26.0f, dsp::Waveshape::germanium, 0.0f, 2200.0, 12000.0, -4.0f, true } },

    { "Sunburst OD", "TubeForge", "Asymmetric clipping, two diodes one way and one the other. "
                                  "Brighter and more harmonically busy than a symmetric box.",
      PedalEngine::shaper, 2, { "Drive", "Tone", "Level", "Mix", "", "" },
      { 560.0, 8.0f, 34.0f, dsp::Waveshape::asymmetricPolynomial, 0.12f, 1600.0, 8000.0, -7.0f, true } },

    { "Low Watt Blues", "TubeForge", "Very low gain, soft knee, almost no colour. A transparent "
                                     "crunch for stacking in front of something else.",
      PedalEngine::shaper, 2, { "Gain", "Tone", "Volume", "Mix", "", "" },
      { 320.0, 0.0f, 26.0f, dsp::Waveshape::softClip, 0.0f, 1800.0, 9000.0, -4.0f, true } },

    { "White Rail", "TubeForge", "MOSFET-style hard clipping with a switchable low end. Dynamic "
                                 "and amp-like -- it cleans up when you back off.",
      PedalEngine::shaper, 3, { "Volume", "Drive", "Tone", "Mix", "Low end", "" },
      // Not parallel: this one replaces rather than sums, which is why it is more aggressive
      // than the screamers above at the same drive. Aux A sweeps the input corner.
      { 90.0, 4.0f, 34.0f, dsp::Waveshape::hardClip, 0.05f, 1500.0, 9000.0, -8.0f, false,
        0.0f, 0.0, 0.0, 12.0f, 1.1f, true } },

    { "Sharpshooter", "TubeForge", "Tight, fast and modern, high-passed hard at the input for "
                                   "down-tuned rhythm work. Nothing loose survives it.",
      PedalEngine::shaper, 3, { "Drive", "Tone", "Level", "Mix", "", "" },
      { 220.0, 8.0f, 36.0f, dsp::Waveshape::diode, 0.0f, 1800.0, 9000.0, -7.0f, true } },

    { "Indigo Driver", "TubeForge", "Fuller and more open than a screamer, with real low end left "
                                    "in. Sits between an overdrive and a light distortion.",
      PedalEngine::shaper, 2, { "Gain", "Tone", "Level", "Mix", "", "" },
      { 140.0, 6.0f, 34.0f, dsp::Waveshape::asymmetricPolynomial, 0.08f, 1400.0, 10000.0, -7.0f, true } },

    { "Twin Monarch", "TubeForge", "Two clipping stages in one box, voiced a little apart. Aux A "
                                   "moves the second stage from soft to hard.",
      PedalEngine::shaper, 3, { "Drive", "Tone", "Level", "Mix", "Voice", "" },
      { 300.0, 4.0f, 32.0f, dsp::Waveshape::softClip, 0.0f, 1600.0, 10000.0, -6.0f, true } },

    { "Natural Sparkle", "TubeForge", "Low gain with a lifted top end and a gentle mid dip. Adds "
                                      "air rather than grit.",
      PedalEngine::shaper, 2, { "Drive", "Spectrum", "Level", "Mix", "", "" },
      { 260.0, 0.0f, 28.0f, dsp::Waveshape::softClip, 0.0f, 2600.0, 13000.0, -5.0f, true } },

    { "Double Brew", "TubeForge", "Two voices in one enclosure: a germanium overdrive stacked "
                                  "into a silicon distortion. Aux A blends between them.",
      PedalEngine::shaper, 3, { "Drive", "Tone", "Level", "Mix", "Blend", "" },
      { 200.0, 6.0f, 36.0f, dsp::Waveshape::germanium, 0.06f, 1300.0, 9000.0, -8.0f, true } },

    // ---- Distortions -----------------------------------------------------------------------
    // These replace the signal rather than summing back, which is the structural difference
    // from the overdrives above and matters far more than the choice of curve.

    { "Rodent", "TubeForge", "A deliberately slow gain stage into hard silicon clipping. The "
                             "slew ceiling is the character -- searing at the top, sludgy below.",
      PedalEngine::shaper, 3, { "Distortion", "Filter", "Volume", "Mix", "", "" },
      // The slew limiter is what makes this one itself. Everything else here is ordinary.
      { 110.0, 10.0f, 44.0f, dsp::Waveshape::hardClip, 0.0f, 700.0, 7000.0, -13.0f, false,
        0.055f, 0.0, 0.0, 12.0f, 1.1f, true } },

    { "Orange Razor", "TubeForge", "Bright, cutting and scooped through the middle. Op-amp hard "
                                   "clipping with a passive tilt after it.",
      PedalEngine::shaper, 1, { "Tone", "Level", "Distortion", "Mix", "", "" },
      { 130.0, 10.0f, 42.0f, dsp::Waveshape::hardClip, 0.0f, 900.0, 9000.0, -13.0f, false,
        0.0f, 0.0, 0.0, 12.0f, 1.1f, true } },

    { "Chainsaw", "TubeForge", "Two resonant peaks either side of the mids, both fully "
                              "adjustable. Push them together and it buzzsaws.",
      PedalEngine::shaper, 3, { "Distortion", "Tone", "Level", "Mix", "Colour low", "Colour high" },
      // The twin gyrator peaks are the whole point of this one: 100 Hz and 1.2 kHz, each with
      // its own knob and a wide range either side of flat.
      { 80.0, 14.0f, 46.0f, dsp::Waveshape::hardClip, 0.0f, 900.0, 8000.0, -15.0f, false,
        0.0f, 100.0, 1219.0, 15.0f, 1.0f, true } },

    { "Red Stack", "TubeForge", "LED clipping into a multi-stage gain structure. Thick and "
                                "compressed, like a small British stack on the edge.",
      PedalEngine::shaper, 2, { "Volume", "Tone", "Gain", "Mix", "", "" },
      { 120.0, 10.0f, 44.0f, dsp::Waveshape::ledClip, 0.0f, 1100.0, 8000.0, -12.0f, false,
        0.0f, 0.0, 0.0, 12.0f, 1.1f, true } },

    { "Violet Djent", "TubeForge", "Modern, tight and articulate at very high gain, with an "
                                   "active two-band voicing after the clipper.",
      PedalEngine::shaper, 4, { "Gain", "Treble", "Volume", "Mix", "Bass", "Middle" },
      { 150.0, 14.0f, 48.0f, dsp::Waveshape::hardClip, 0.03f, 1400.0, 9000.0, -15.0f, false,
        0.0f, 160.0, 900.0, 10.0f, 0.9f, true } },

    { "Metal Sector", "TubeForge", "A parametric mid band on top of very high gain, so the "
                                   "scoop can be put anywhere. Aux B sweeps it.",
      PedalEngine::shaper, 3, { "Level", "Distortion", "Treble", "Mix", "Mid level", "Mid freq" },
      { 100.0, 14.0f, 50.0f, dsp::Waveshape::hardClip, 0.0f, 900.0, 9000.0, -16.0f, false,
        0.0f, 200.0, 750.0, 14.0f, 1.4f, true } },

    { "Stack Attack", "TubeForge", "Cascading gain modelled on a high-power amplifier front end. "
                                   "Dense, saturated and even.",
      PedalEngine::shaper, 4, { "Gain", "Tone", "Level", "Mix", "", "" },
      { 130.0, 12.0f, 48.0f, dsp::Waveshape::softClip, 0.04f, 1200.0, 8500.0, -15.0f, false } },

    { "British Extra", "TubeForge", "A modern high-gain preamp in a box: tight low end, hard "
                                    "upper mids, and a lot of it.",
      PedalEngine::shaper, 4, { "Gain", "Tone", "Level", "Mix", "Presence", "Tight" },
      { 145.0, 14.0f, 50.0f, dsp::Waveshape::hardClip, 0.05f, 1300.0, 9500.0, -16.0f, false,
        0.0f, 0.0, 3200.0, 8.0f, 0.8f, true } },

    { "Teutonic Four", "TubeForge", "Four stages of cascading gain with a hard, glassy top. The "
                                    "most compressed of the distortions here.",
      PedalEngine::shaper, 4, { "Gain", "Treble", "Volume", "Mix", "Bass", "Middle" },
      { 160.0, 16.0f, 52.0f, dsp::Waveshape::hardClip, 0.02f, 1500.0, 9000.0, -17.0f, false,
        0.0f, 180.0, 1100.0, 12.0f, 1.0f, true } },

    // ---- Fuzzes ----------------------------------------------------------------------------
    // Highest gain, biased curves, and a hard input high-pass -- a fuzz fed the full low end
    // collapses into a splutter rather than a note.

    { "Triangle Sustain", "TubeForge", "Four stages of soft clipping with a deep mid scoop. "
                                       "Endless sustain and a wall of low end.",
      PedalEngine::shaper, 3, { "Sustain", "Tone", "Volume", "Mix", "Scoop", "" },
      { 70.0, 20.0f, 54.0f, dsp::Waveshape::softClip, 0.0f, 700.0, 6500.0, -14.0f, false,
        0.0f, 0.0, 900.0, 12.0f, 0.9f } },

    { "Germanium Smile", "TubeForge", "Two germanium transistors and almost no headroom. Cleans "
                                      "up dramatically when you roll the guitar back.",
      PedalEngine::shaper, 3, { "Fuzz", "Tone", "Volume", "Mix", "", "" },
      { 90.0, 18.0f, 50.0f, dsp::Waveshape::germanium, 0.22f, 800.0, 5500.0, -12.0f, false } },

    { "Octave Super", "TubeForge", "Heavily biased and octave-rich, with a fixed mid scoop. "
                                   "Splutters on purpose.",
      PedalEngine::shaper, 3, { "Fuzz", "Tone", "Volume", "Mix", "", "" },
      { 120.0, 22.0f, 54.0f, dsp::Waveshape::asymmetricPolynomial, 0.3f, 900.0, 7000.0, -13.0f, false,
        0.0f, 0.0, 1000.0, 10.0f, 0.7f } },

    { "Fuzz Foundry", "TubeForge", "Unstable by design. Aux A starves the supply until it gates, "
                                   "oscillates and tears.",
      PedalEngine::shaper, 4, { "Fuzz", "Tone", "Volume", "Mix", "Starve", "Bias" },
      { 140.0, 20.0f, 56.0f, dsp::Waveshape::germanium, 0.28f, 900.0, 6000.0, -13.0f, false } },

    { "Tone Bruiser", "TubeForge", "The oldest voicing here: thick, woolly and mid-forward, with "
                                   "a soft top and no real bottom.",
      PedalEngine::shaper, 3, { "Attack", "Level", "Tone", "Mix", "", "" },
      { 130.0, 18.0f, 50.0f, dsp::Waveshape::germanium, 0.2f, 700.0, 4500.0, -12.0f, false } },

    { "Fuzz Riot", "TubeForge", "Maximum gain, no restraint, and a wide open top. More of a "
                                "sound than an effect.",
      PedalEngine::shaper, 3, { "Fuzz", "Tone", "Volume", "Mix", "", "" },
      { 60.0, 24.0f, 60.0f, dsp::Waveshape::hardClip, 0.24f, 1200.0, 12000.0, -16.0f, false,
        0.0f, 0.0, 0.0, 12.0f, 1.1f, true } },

    { "Cloven", "TubeForge", "A modern take on the four-stage fuzz, with the mid scoop under a "
                             "knob instead of soldered in.",
      PedalEngine::shaper, 3, { "Fuzz", "Tone", "Level", "Mix", "Shift", "" },
      { 80.0, 20.0f, 54.0f, dsp::Waveshape::softClip, 0.1f, 800.0, 7000.0, -14.0f, false,
        0.0f, 0.0, 800.0, 12.0f, 0.8f } },

    { "Low Sustain", "TubeForge", "The four-stage fuzz voiced for bass: the low end is kept "
                                  "rather than filtered away before the clipper.",
      PedalEngine::shaper, 3, { "Sustain", "Tone", "Volume", "Mix", "Dry blend", "" },
      { 35.0, 18.0f, 50.0f, dsp::Waveshape::softClip, 0.0f, 600.0, 6000.0, -13.0f, false } },

    // ---- Boost and dynamics ----------------------------------------------------------------

    { "Studio Lift", "TubeForge", "A clean lift with a gentle top-end tilt. Nothing but level "
                                  "and a little air into whatever follows it.",
      PedalEngine::shaper, 1, { "Gain", "Treble", "Bass", "Mix", "", "" },
      { 25.0, 0.0f, 22.0f, dsp::Waveshape::hyperbolicTangent, 0.0f, 3000.0, 18000.0, -1.0f, false } },

    { "Studio Squeeze", "TubeForge", "A studio-style compressor with a blend control. Levels the "
                                     "picking without flattening the attack.",
      PedalEngine::compressor, 3, { "Sustain", "Tone", "Level", "Mix", "", "" },
      { 20.0, 0.0f, 0.0f, dsp::Waveshape::hyperbolicTangent, 0.0f, 2400.0, 18000.0, 0.0f, false } },

    // ---- Modulation ------------------------------------------------------------------------
    // Chorus and flanger are the same swept delay at different depths; a phaser is not a delay
    // at all. The shaper block is unused by all three, so it is left at its defaults.

    { "Ensemble", "TubeForge", "A gentle, wide chorus. One guitar becomes two slightly out of "
                               "tune with each other, which is the whole trick.",
      PedalEngine::modulation, 2, { "Rate", "Depth", "Level", "Mix", "", "" },
      {},
      { ModulationTopology::chorus, 14.0f, 4.0f, 0.05f, 3.0f, 0.0f, 4, 0.25f } },

    { "Liquid Vibrato", "TubeForge", "A deeper, slower chorus that tips into vibrato as the depth "
                                     "comes up and the dry path is mixed away.",
      PedalEngine::modulation, 3, { "Rate", "Depth", "Level", "Mix", "", "" },
      {},
      { ModulationTopology::chorus, 9.0f, 7.0f, 0.03f, 8.0f, 0.0f, 4, 0.5f } },

    { "Jet Flanger", "TubeForge", "A very short delay swept wide with heavy regeneration. Turn "
                                  "the feedback up and it takes off.",
      PedalEngine::modulation, 3, { "Rate", "Depth", "Level", "Mix", "Feedback", "" },
      {},
      // Sub-millisecond centre: at these lengths the comb's first notch is up in the audible
      // band, which is the difference between a flange and a chorus.
      { ModulationTopology::flanger, 1.6f, 1.4f, 0.05f, 5.0f, 0.85f, 4, 0.15f } },

    { "Deluxe Sweep", "TubeForge", "A slower, wider flange with inverted regeneration -- hollow "
                                   "and through-a-pipe rather than jet-plane.",
      PedalEngine::modulation, 3, { "Rate", "Range", "Level", "Mix", "Feedback", "" },
      {},
      // Negative feedback: the notch pattern moves to include DC, which is the hollow one.
      { ModulationTopology::flanger, 3.0f, 2.6f, 0.02f, 2.5f, -0.8f, 4, 0.2f } },

    { "Orange Phase", "TubeForge", "Four all-pass stages sweeping together. Notches rather than a "
                                   "comb, which is why it sits under a riff instead of over it.",
      PedalEngine::modulation, 2, { "Rate", "Depth", "Level", "Mix", "", "" },
      {},
      { ModulationTopology::phaser, 0.0f, 0.0f, 0.05f, 6.0f, 0.0f, 4, 0.0f } },

    // ---- Bit and sample-rate reduction -------------------------------------------------------
    // The aliasing is the effect here, so none of these anti-alias anything on purpose.

    { "Bit Mapper", "TubeForge", "Sample-rate and bit-depth reduction with a resonant filter over "
                                 "it. Everything from mild grit to unrecognisable.",
      PedalEngine::crush, 3, { "Crush", "Filter", "Level", "Mix", "Bits", "" },
      {}, {},
      { 3.0f, 16.0f, 1.0f, 40.0f, 300.0, 14000.0, 2.2f } },

    { "Octo Bit", "TubeForge", "Heavier downsampling with a narrower filter. Gets to the "
                               "eight-bit end of things much sooner.",
      PedalEngine::crush, 3, { "Crush", "Filter", "Level", "Mix", "Bits", "" },
      {}, {},
      { 2.0f, 12.0f, 2.0f, 64.0f, 400.0, 9000.0, 2.8f } },

    { "Reducer", "TubeForge", "A precise downsampler with a very resonant filter -- the "
                              "filter is as much of the sound as the reduction is.",
      PedalEngine::crush, 4, { "Rate", "Filter", "Level", "Mix", "Bits", "" },
      {}, {},
      { 4.0f, 20.0f, 1.0f, 32.0f, 200.0, 16000.0, 4.5f } },

    { "Radiation", "TubeForge", "Extreme reduction with a wide filter sweep. The most violent of "
                                "these, and the least predictable.",
      PedalEngine::crush, 4, { "Crush", "Filter", "Level", "Mix", "Bits", "" },
      {}, {},
      { 1.0f, 10.0f, 1.0f, 96.0f, 150.0, 18000.0, 3.4f } },

    { "Bit Sergeant", "TubeForge", "Low bit depth and a narrow filter, voiced to sound like an "
                                   "old sampler rather than a broken one.",
      PedalEngine::crush, 3, { "Crush", "Filter", "Level", "Mix", "Bits", "" },
      {}, {},
      { 3.0f, 8.0f, 2.0f, 24.0f, 500.0, 7000.0, 1.8f } },

    // ---- Delay -----------------------------------------------------------------------------
    // Two delays that differ almost entirely in how much bandwidth each repeat loses. The
    // fourth repeat of the analogue one is a dull ghost; the fourth repeat of the digital one
    // is the same as the first.

    { "Carbon Trace", "TubeForge", "A bucket-brigade delay. Every repeat is darker than the last, "
                                   "which is what keeps a long feedback setting musical.",
      PedalEngine::delay, 3, { "Time", "Tone", "Level", "Mix", "Repeats", "" },
      {}, {},
      { 40.0f, 600.0f, 0.85f, 3200.0f, 0.0f } },

    { "Clean Repeat", "TubeForge", "A clean digital delay. Every repeat is the same as the first, "
                                   "which is precise and can get crowded fast.",
      PedalEngine::delay, 2, { "Time", "Tone", "Level", "Mix", "Repeats", "" },
      {}, {},
      { 40.0f, 1200.0f, 0.88f, 18000.0f, 0.12f } },

    // ---- Reverb ----------------------------------------------------------------------------

    { "Spring Tank", "TubeForge", "A dispersive spring: transients arrive as a rising chirp "
                                  "rather than a copy. That boing is the whole point.",
      PedalEngine::reverb, 3, { "Decay", "Tone", "Level", "Mix", "", "" },
      {}, {}, {}, {},
      // Four all-pass stages ahead of the tank produce the dispersion. Nothing else here does.
      { 0.15f, 0.7f, 0.5f, 220.0f, 4, 1400.0 } },

    { "Slow Bloom", "TubeForge", "A soft, slow ambience that swells rather than arriving. Sits "
                                 "under a part instead of behind it.",
      PedalEngine::reverb, 4, { "Decay", "Tone", "Level", "Mix", "", "" },
      {}, {}, {}, {},
      { 0.5f, 0.97f, 0.62f, 200.0f, 0, 0.0 } },

    { "Cathedral", "TubeForge", "The longest tail here, with a low cut that keeps it out of the "
                                "way of the bass. Ambient rather than a room.",
      PedalEngine::reverb, 5, { "Decay", "Tone", "Level", "Mix", "", "" },
      {}, {}, {}, {},
      { 0.6f, 0.99f, 0.4f, 260.0f, 0, 0.0 } },

    { "Studio Verb", "TubeForge", "A neutral plate. Dense, even and short enough to use on "
                                  "everything without it becoming the sound.",
      PedalEngine::reverb, 4, { "Decay", "Tone", "Level", "Mix", "", "" },
      {}, {}, {}, {},
      { 0.2f, 0.8f, 0.35f, 150.0f, 0, 0.0 } },

    // ---- Bass DI and preamps ----------------------------------------------------------------
    // All six split the band and drive only the top of it. Aux A is the blend between the clean
    // and driven paths, which is the control every one of these boxes actually has.

    { "Deep Six", "TubeForge", "A modern bass preamp: aggressive upper-band drive over a clean "
                               "low end, with the blend right where you can reach it.",
      PedalEngine::bassPreamp, 4, { "Drive", "Tone", "Level", "Mix", "Blend", "Low" },
      {}, {}, {}, {}, {},
      { 200.0, 8.0f, 42.0f, dsp::Waveshape::hardClip, 1400.0, 9000.0, -6.0f, 6.0f } },

    { "Sand Driver", "TubeForge", "The classic DI voicing: a mid-scooped drive over an untouched "
                                  "bottom, ready to go straight to a desk.",
      PedalEngine::bassPreamp, 3, { "Drive", "Tone", "Level", "Mix", "Blend", "Low" },
      {}, {}, {}, {}, {},
      { 160.0, 6.0f, 36.0f, dsp::Waveshape::softClip, 1100.0, 8000.0, -5.0f, 5.0f } },

    { "Hammer Tone", "TubeForge", "A cleaner preamp with a broad, gentle drive. More about "
                                  "shaping the tone than adding grit.",
      PedalEngine::bassPreamp, 3, { "Drive", "Tone", "Level", "Mix", "Blend", "Low" },
      {}, {}, {}, {}, {},
      { 180.0, 2.0f, 28.0f, dsp::Waveshape::hyperbolicTangent, 1600.0, 11000.0, -3.0f, 7.0f } },

    { "Alpha Omega", "TubeForge", "Two drive voicings blended against each other -- one thick and "
                                  "one aggressive -- over the same clean bottom.",
      PedalEngine::bassPreamp, 4, { "Drive", "Tone", "Level", "Mix", "Blend", "Low" },
      {}, {}, {}, {}, {},
      { 220.0, 10.0f, 46.0f, dsp::Waveshape::asymmetricPolynomial, 1300.0, 9500.0, -7.0f, 6.0f } },

    { "Scrambler DI", "TubeForge", "A valve-voiced DI with a soft, forgiving upper band and a lot "
                                   "of low-end authority.",
      PedalEngine::bassPreamp, 3, { "Drive", "Tone", "Level", "Mix", "Blend", "Low" },
      {}, {}, {}, {}, {},
      { 150.0, 4.0f, 30.0f, dsp::Waveshape::germanium, 1000.0, 7500.0, -4.0f, 8.0f } },

    { "Valve Tone DI", "TubeForge", "The amp-in-a-box end of the family: enough upper-band drive "
                                    "to stand in for a rig, with the clean path holding it up.",
      PedalEngine::bassPreamp, 4, { "Drive", "Tone", "Level", "Mix", "Blend", "Low" },
      {}, {}, {}, {}, {},
      { 170.0, 6.0f, 38.0f, dsp::Waveshape::diode, 1200.0, 8500.0, -6.0f, 6.0f } },

    // ---- Pitch -------------------------------------------------------------------------------
    // Six defaulted blocks before the pitch one: shaper, modulation, crush, delay, reverb,
    // bassPreamp. Getting that count wrong assigns a model's numbers to the wrong engine.

    { "Poly Octave", "TubeForge", "Octaves above and below, mixed against the dry note. Long "
                                  "window, so it holds chords together rather than tracking one note.",
      PedalEngine::pitch, 4, { "Pitch", "Tone", "Level", "Mix", "", "Sub" },
      {}, {}, {}, {}, {}, {},
      { -12.0f, 12.0f, 62.0f, -12.0f, 0.0f } },

    { "Dive Bomb", "TubeForge", "Two octaves either way under one control. Short window, so it "
                                "tracks a single note tightly and warbles on anything else.",
      PedalEngine::pitch, 5, { "Pitch", "Tone", "Level", "Mix", "", "" },
      {}, {}, {}, {}, {}, {},
      { -24.0f, 24.0f, 34.0f, 0.0f, 0.0f } },

    { "Warp Delay", "TubeForge", "A pitch shifter inside its own feedback loop, so every repeat "
                                 "arrives a step further away. Ascends or descends for ever.",
      PedalEngine::pitch, 5, { "Pitch", "Tone", "Level", "Mix", "Regen", "" },
      {}, {}, {}, {}, {}, {},
      { -12.0f, 12.0f, 50.0f, 0.0f, 0.7f } },
} };

static_assert(modelTable.size() >= kindCount,
              "the built-in archetypes must all be present at the head of the model table");
} // namespace

const PedalModel& pedalModel(int index) noexcept
{
    const auto slot = static_cast<std::size_t>(
        std::clamp(index, 0, static_cast<int>(modelTable.size()) - 1));
    return modelTable[slot];
}

std::size_t modelCount() noexcept { return modelTable.size(); }

void PedalSlot::retarget(dsp::SmoothedParameter& parameter, float& cached, float value) noexcept
{
    if (cached == value) return;
    cached = value;
    parameter.setTarget(value);
}

float PedalSlot::driveTarget() const noexcept
{
    const auto& voicing = pedalModel(activeModel).shaper;
    return dsp::dbToLinear(voicing.minDriveDb
                           + (voicing.maxDriveDb - voicing.minDriveDb) * controlToUnit(requested.drive));
}

float PedalSlot::levelTarget() const noexcept
{
    return dsp::dbToLinear(std::clamp(requested.levelDb, -36.0f, 36.0f)
                           + pedalModel(activeModel).shaper.trimDb);
}

float PedalSlot::mixTarget() const noexcept
{
    const auto engaged = ! switching && ! requested.bypassed && activeModel != 0;
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
    gyratorLow.prepare(spec);
    gyratorHigh.prepare(spec);
    crushFilter.prepare(spec);
    // Sized for the deepest modulation any model asks for, once, rather than per model: a
    // model change must not allocate, and 60 ms covers every chorus and flanger here with room.
    modulationLine.prepare(static_cast<std::size_t>(0.060 * spec.sampleRate) + 8, spec.channels);
    delayLine.prepare(spec, 2000.0f);
    reverbTank.prepare(spec);
    bassCrossover.prepare(spec);
    bassToneFilter.prepare(spec);
    // Long enough for the deepest window any pitch model asks for, plus interpolation margin.
    pitchLine.prepare(static_cast<std::size_t>(0.120 * spec.sampleRate) + 8, spec.channels);
    for (std::size_t channel = 0; channel < dsp::maximumChannels; ++channel)
    {
        lowBand[channel].assign(spec.maximumBlockSize, 0.0f);
        highBand[channel].assign(spec.maximumBlockSize, 0.0f);
    }
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
    activeModel = requested.model;
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
    gyratorLow.reset();
    gyratorHigh.reset();
    slewLimiter.reset();
    antialiased.reset();
    crushFilter.reset();
    modulationLine.reset();
    for (auto& channel : phaserStages) channel.fill({});
    modulationFeedback.fill(0.0f);
    crushHeld.fill(0.0f);
    crushCounter.fill(0.0f);
    lfoPhase = 0.0;
    delayLine.reset();
    reverbTank.reset();
    bassCrossover.reset();
    bassToneFilter.reset();
    pitchLine.reset();
    pitchFeedback.fill(0.0f);
    pitchPhase = 0.0;
    pitchPhaseSecond = 0.0;
    for (auto& channel : springStages) channel.fill({});
    compressor.reset();
    neural.reset();
    // Forces updateFilters to recompute: after a reset the filters hold their default
    // pass-through coefficients, whatever the tone control last said.
    filteredTone = -1.0f;
}

void PedalSlot::updateFilters() noexcept
{
    const auto tone = std::clamp(requested.tone, 0.0f, 10.0f);
    const auto auxA = std::clamp(requested.auxA, 0.0f, 10.0f);
    const auto auxB = std::clamp(requested.auxB, 0.0f, 10.0f);
    const auto stale = filteredTone < 0.0f;
    if (! stale && activeModel == filteredModel && tone == filteredTone
        && auxA == filteredAuxA && auxB == filteredAuxB)
        return;

    const auto& voicing = pedalModel(activeModel).shaper;
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
    // The gyrator pair. A band whose frequency is zero is left flat rather than made
    // pass-through by coefficients, so a model without one costs nothing per sample.
    const auto peak = [&](dsp::Biquad& section, double frequency, float control)
    {
        if (frequency <= 0.0) return;
        const auto gainDb = (controlToUnit(control) * 2.0f - 1.0f) * voicing.gyratorRangeDb;
        section.setCoefficients(
            dsp::BiquadCoefficients::make(dsp::FilterType::peaking, spec.sampleRate,
                                          dsp::clampFrequency(frequency, spec.sampleRate),
                                          static_cast<double>(voicing.gyratorQ),
                                          static_cast<double>(gainDb)),
            interpolation);
    };
    peak(gyratorLow, voicing.gyratorLowHz, auxA);
    peak(gyratorHigh, voicing.gyratorHighHz, auxB);

    // The crusher's resonant low-pass, swept by the same tone control every other engine uses.
    const auto& crushing = pedalModel(activeModel).crush;
    crushFilter.setCoefficients(
        dsp::BiquadCoefficients::make(
            dsp::FilterType::lowPass, spec.sampleRate,
            dsp::clampFrequency(sweep(crushing.filterMinHz, crushing.filterMaxHz, controlToUnit(tone)),
                                spec.sampleRate),
            static_cast<double>(crushing.filterQ)),
        interpolation);

    /* The slew ceiling, expressed at 48 kHz and scaled to whatever is running.

       It has to scale, and the direction is easy to get backwards: the limit is per *sample*,
       so at twice the rate each step covers half the time and the same figure would let the
       signal move twice as fast per second. Dividing by the rate ratio keeps the volts-per-
       second the model was voiced for. */
    slewLimiter.setMaximumStep(voicing.slewPerSampleAt48k <= 0.0f
        ? 0.0f
        : voicing.slewPerSampleAt48k * static_cast<float>(48000.0 / std::max(1.0, spec.sampleRate)));

    filteredModel = activeModel;
    filteredTone = tone;
    filteredAuxA = auxA;
    filteredAuxB = auxB;
}

void PedalSlot::setParameters(const PedalParameters& parameters) noexcept
{
    requested = parameters;
    // An index past the end of the table means an empty slot rather than the last model: a
    // project saved by a later build naming a pedal this one does not have should be silent,
    // not surprising.
    if (requested.model < 0 || static_cast<std::size_t>(requested.model) >= modelCount())
        requested.model = 0;
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

    // A model change rides the blend down to dry first and only then swaps, so the outgoing
    // pedal fades out instead of being cut out from under the signal.
    if (requested.model != activeModel && ! switching)
        switching = true;
    if (switching && ! wetMix.isSmoothing() && wetMix.value() <= 0.0f)
    {
        activeModel = requested.model;
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
    if (activeModel == 0 || (cachedMix <= 0.0f && ! wetMix.isSmoothing()))
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

    // Dispatched on the engine rather than on the model, which is the point of the split: a
    // pedal added to the table costs a row and no branch.
    switch (pedalModel(activeModel).engine)
    {
        case PedalEngine::compressor: renderCompressor(channels, count, samples); break;
        case PedalEngine::neural:     renderNeural(channels, count, samples); break;
        case PedalEngine::shaper:     renderShaped(channels, count, samples); break;
        case PedalEngine::modulation: renderModulation(channels, count, samples); break;
        case PedalEngine::crush:      renderCrush(channels, count, samples); break;
        case PedalEngine::delay:      renderDelay(channels, count, samples); break;
        case PedalEngine::reverb:     renderReverb(channels, count, samples); break;
        case PedalEngine::bassPreamp: renderBassPreamp(channels, count, samples); break;
        case PedalEngine::pitch:      renderPitch(channels, count, samples); break;
        case PedalEngine::silent:     break;
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
    const auto& voicing = pedalModel(activeModel).shaper;
    // Ahead of the clipper, because that is where a slow op-amp sits: it rounds the signal
    // going *into* the nonlinearity, which is why it takes the edge off without dulling the
    // quiet playing that never reaches the ceiling.
    const auto slewing = slewLimiter.engaged();
    const auto integrating = voicing.antiAlias && dsp::supportsAntiderivative(voicing.shape);

    // One call site for the curve, so the two paths below cannot end up disagreeing about
    // whether a model is anti-aliased.
    const auto shape = [&](float value, float gain, std::size_t channel)
    {
        return integrating ? antialiased.process(value, voicing.shape, gain, channel)
                           : dsp::shapeSample(value, voicing.shape, gain);
    };

    if (voicing.parallelClip)
    {
        for (std::size_t channel = 0; channel < count; ++channel)
            std::copy_n(dryScratch[channel].begin(), samples, bandScratch[channel].begin());
        inputFilter.process(bandPointers.data(), count, samples);
        for (std::size_t sample = 0; sample < samples; ++sample)
        {
            const auto gain = driveGain.next();
            for (std::size_t channel = 0; channel < count; ++channel)
            {
                auto driven = bandScratch[channel][sample];
                if (slewing) driven = slewLimiter.process(driven, channel);
                channels[channel][sample] = dryScratch[channel][sample] + shape(driven, gain, channel);
            }
        }
    }
    else
    {
        inputFilter.process(channels, count, samples);
        const auto bias = voicing.bias;
        for (std::size_t sample = 0; sample < samples; ++sample)
        {
            const auto gain = driveGain.next();
            /* Cancels the DC that a biased curve sits on. Only a model that actually uses bias
               pays for the second evaluation -- and it is deliberately the plain curve rather
               than the integrated one: this is a constant, so its "average across the sample"
               is itself, and running it through the stateful shaper would corrupt the history
               the real signal depends on. */
            const auto offset = bias == 0.0f ? 0.0f : dsp::shapeSample(bias, voicing.shape, gain);
            for (std::size_t channel = 0; channel < count; ++channel)
            {
                auto driven = channels[channel][sample];
                if (slewing) driven = slewLimiter.process(driven, channel);
                channels[channel][sample] = shape(driven + bias, gain, channel) - offset;
            }
        }
    }
    // The gyrator peaks sit after the clipper, which is what makes them a voicing rather than
    // a way of aiming the distortion.
    if (voicing.gyratorLowHz > 0.0) gyratorLow.process(channels, count, samples);
    if (voicing.gyratorHighHz > 0.0) gyratorHigh.process(channels, count, samples);
    toneFilter.process(channels, count, samples);
}

void PedalSlot::renderModulation(float* const* channels, std::size_t count, std::size_t samples) noexcept
{
    const auto& voicing = pedalModel(activeModel).modulation;
    // Drive is Rate, tone is Depth, aux A is Feedback. The model names them; the engine only
    // needs to know which slider is which, and that mapping is the same for every model here.
    const auto rate = static_cast<double>(sweep(voicing.minRateHz, voicing.maxRateHz,
                                                controlToUnit(requested.drive)));
    const auto depth = controlToUnit(requested.tone);
    const auto regeneration = voicing.maxFeedback * controlToUnit(requested.auxA);
    const auto increment = rate / std::max(1.0, spec.sampleRate);

    if (voicing.topology == ModulationTopology::phaser)
    {
        /* All-pass sections, not a delay. A phaser's notches are where the phase-shifted copy
           cancels the dry one, and they are spaced logarithmically -- which is why a phaser
           sweeps differently from a flanger even at the same rate. */
        const auto sections = static_cast<std::size_t>(
            std::clamp(voicing.stages, 1, static_cast<int>(maximumPhaserStages)));
        for (std::size_t sample = 0; sample < samples; ++sample)
        {
            const auto phase = lfoPhase + static_cast<double>(sample) * increment;
            // A triangle rather than a sine: the classic sweep spends its time at the ends.
            const auto sweepUnit = 2.0 * std::abs(phase - std::floor(phase) - 0.5);
            const auto frequency = dsp::clampFrequency(
                sweep(200.0, 2000.0, static_cast<float>(sweepUnit * depth + (1.0 - depth) * 0.5)),
                spec.sampleRate);

            // The first-order all-pass coefficient for that corner. Computed per sample rather
            // than per block on purpose: this is the sweep, and quantising it to block
            // boundaries is audible as a staircase at slow rates.
            const auto tangent = std::tan(std::numbers::pi * frequency / spec.sampleRate);
            const auto coefficient = static_cast<float>((tangent - 1.0) / (tangent + 1.0));

            for (std::size_t channel = 0; channel < count; ++channel)
            {
                const auto dry = channels[channel][sample];
                auto value = dry + regeneration * modulationFeedback[channel];
                for (std::size_t section = 0; section < sections; ++section)
                    value = phaserStages[channel][section].process(value, coefficient);
                modulationFeedback[channel] = std::isfinite(value) ? value : 0.0f;
                // Summed with the dry path: the notches only exist where the two cancel.
                channels[channel][sample] = 0.5f * (dry + value);
            }
        }
    }
    else
    {
        const auto centre = static_cast<float>(voicing.centreMs * 0.001 * spec.sampleRate);
        const auto swing = static_cast<float>(voicing.depthMs * 0.001 * spec.sampleRate) * depth;
        for (std::size_t sample = 0; sample < samples; ++sample)
        {
            const auto phase = lfoPhase + static_cast<double>(sample) * increment;
            for (std::size_t channel = 0; channel < count; ++channel)
            {
                // The stereo offset is what makes one mono guitar come out wide, and it is the
                // only thing separating the two channels -- everything else here is identical.
                const auto offset = static_cast<double>(channel) * voicing.stereoSpread;
                const auto angle = 2.0 * std::numbers::pi * (phase + offset);
                const auto delay = centre + swing * static_cast<float>(std::sin(angle));

                const auto driven = channels[channel][sample]
                                  + regeneration * modulationFeedback[channel];
                const auto wet = modulationLine.processSample(driven, delay, channel);
                modulationFeedback[channel] = std::isfinite(wet) ? wet : 0.0f;
                channels[channel][sample] = wet;
            }
        }
    }

    // Kept in 0..1 rather than accumulating, so the phase never loses precision in a long
    // session -- at 48 kHz an unwrapped double would still be exact, but a float would not.
    lfoPhase += static_cast<double>(samples) * increment;
    lfoPhase -= std::floor(lfoPhase);
    toneFilter.process(channels, count, samples);
}

void PedalSlot::renderCrush(float* const* channels, std::size_t count, std::size_t samples) noexcept
{
    const auto& voicing = pedalModel(activeModel).crush;
    // Drive is the sample-rate divisor, aux A the bit depth, tone the filter. Deliberately no
    // anti-aliasing anywhere: the aliasing here *is* the effect.
    const auto divisor = std::max(1.0f, voicing.minDivisor
        + (voicing.maxDivisor - voicing.minDivisor) * controlToUnit(requested.drive));
    const auto bits = std::clamp(voicing.minBits
        + (voicing.maxBits - voicing.minBits) * (1.0f - controlToUnit(requested.auxA)),
        1.0f, 24.0f);
    const auto levels = std::pow(2.0f, bits - 1.0f);

    for (std::size_t sample = 0; sample < samples; ++sample)
        for (std::size_t channel = 0; channel < count; ++channel)
        {
            // Sample and hold. The counter is fractional so the divisor can sweep smoothly
            // instead of jumping between integer rates, which is audible as a zipper.
            auto& counter = crushCounter[channel];
            if (counter <= 0.0f)
            {
                crushHeld[channel] = channels[channel][sample];
                counter += divisor;
            }
            counter -= 1.0f;

            const auto quantised = std::round(std::clamp(crushHeld[channel], -1.5f, 1.5f) * levels) / levels;
            channels[channel][sample] = quantised;
        }

    crushFilter.process(channels, count, samples);
    toneFilter.process(channels, count, samples);
}

void PedalSlot::renderDelay(float* const* channels, std::size_t count, std::size_t samples) noexcept
{
    const auto& voicing = pedalModel(activeModel).delay;
    dsp::DelayParameters parameters;
    // Geometric, so the short end of the control has as much resolution as the long end --
    // the difference between 40 and 80 ms matters far more than between 700 and 740.
    parameters.timeMs = static_cast<float>(sweep(voicing.minTimeMs, voicing.maxTimeMs,
                                                 controlToUnit(requested.drive)));
    parameters.feedback = voicing.maxFeedback * controlToUnit(requested.auxA);
    // Full wet: the slot's own blend is what mixes this against the dry path, so letting the
    // effect mix as well would apply the control twice.
    parameters.mix = 1.0f;
    // The tone control moves the repeat bandwidth around whatever the model's own ceiling is,
    // so an analogue line stays analogue at every setting rather than turning digital at 10.
    parameters.dampingHz = static_cast<float>(
        std::min(static_cast<double>(voicing.repeatDampingHz),
                 sweep(800.0, 20000.0, controlToUnit(requested.tone))));
    parameters.stereoSpread = voicing.stereoSpread;
    delayLine.setParameters(parameters);
    delayLine.process(channels, count, samples);
}

void PedalSlot::renderReverb(float* const* channels, std::size_t count, std::size_t samples) noexcept
{
    const auto& voicing = pedalModel(activeModel).reverb;

    /* The spring, if this model is one. A tank disperses high frequencies more slowly than low
       ones, so a transient arrives as a rising chirp rather than as a copy of itself -- which
       is why a spring "boings" and a room does not. A chain of all-pass sections ahead of the
       tank produces exactly that, and it is the whole difference between the two. */
    const auto sections = static_cast<std::size_t>(
        std::clamp(voicing.springStages, 0, static_cast<int>(maximumSpringStages)));
    if (sections > 0)
    {
        const auto tangent = std::tan(std::numbers::pi
            * dsp::clampFrequency(voicing.springCentreHz, spec.sampleRate) / spec.sampleRate);
        const auto coefficient = static_cast<float>((tangent - 1.0) / (tangent + 1.0));
        for (std::size_t sample = 0; sample < samples; ++sample)
            for (std::size_t channel = 0; channel < count; ++channel)
            {
                auto value = channels[channel][sample];
                for (std::size_t section = 0; section < sections; ++section)
                    value = springStages[channel][section].process(value, coefficient);
                channels[channel][sample] = value;
            }
    }

    dsp::ReverbParameters parameters;
    parameters.size = voicing.minSize
                    + (voicing.maxSize - voicing.minSize) * controlToUnit(requested.drive);
    // Inverted: the tone control opens up as it is turned clockwise everywhere else in this
    // engine, and damping is the one parameter where more means darker.
    parameters.damping = std::clamp(voicing.damping + (0.5f - controlToUnit(requested.tone)) * 0.8f,
                                    0.0f, 1.0f);
    parameters.mix = 1.0f;
    parameters.lowCutHz = voicing.lowCutHz;
    reverbTank.setParameters(parameters);
    reverbTank.process(channels, count, samples);
}

void PedalSlot::renderBassPreamp(float* const* channels, std::size_t count, std::size_t samples) noexcept
{
    const auto& voicing = pedalModel(activeModel).bassPreamp;
    if (lowBand[0].size() < samples) return;

    for (std::size_t channel = 0; channel < count; ++channel)
    {
        lowPointers[channel] = lowBand[channel].data();
        highPointers[channel] = highBand[channel].data();
    }
    bassCrossover.process(channels, lowPointers.data(), highPointers.data(), count, samples);

    // Only the upper band is driven. Overdriving the fundamental of a low B turns it to mush
    // and loses the note; overdriving what is above the crossover leaves the note intact and
    // puts the grit on top of it. That split is what a bass DI box is.
    const auto driveDb = voicing.minDriveDb
        + (voicing.maxDriveDb - voicing.minDriveDb) * controlToUnit(requested.drive);
    const auto gain = dsp::dbToLinear(driveDb);
    for (std::size_t sample = 0; sample < samples; ++sample)
        for (std::size_t channel = 0; channel < count; ++channel)
            highBand[channel][sample] = dsp::shapeSample(highBand[channel][sample], voicing.shape, gain);

    bassToneFilter.process(highPointers.data(), count, samples);

    // Aux A is the blend between the two paths, which is the control every one of these boxes
    // actually has. Aux B lifts the clean path's bottom.
    const auto blend = controlToUnit(requested.auxA);
    const auto shelf = dsp::dbToLinear(voicing.lowShelfDb * (controlToUnit(requested.auxB) * 2.0f - 1.0f));
    const auto trim = dsp::dbToLinear(voicing.trimDb);
    for (std::size_t sample = 0; sample < samples; ++sample)
        for (std::size_t channel = 0; channel < count; ++channel)
            channels[channel][sample] =
                trim * (lowBand[channel][sample] * shelf * (1.0f - blend * 0.5f)
                        + highBand[channel][sample] * blend);
}

void PedalSlot::renderPitch(float* const* channels, std::size_t count, std::size_t samples) noexcept
{
    const auto& voicing = pedalModel(activeModel).pitch;
    const auto window = std::max(64.0f, voicing.windowMs * 0.001f * static_cast<float>(spec.sampleRate));

    // Quantised to whole semitones. A pitch shifter that lands between notes is an effect
    // nobody asked for, and the control is far easier to set when it detents.
    const auto semitones = std::round(voicing.minSemitones
        + (voicing.maxSemitones - voicing.minSemitones) * controlToUnit(requested.drive));
    const auto ratio = std::pow(2.0, static_cast<double>(semitones) / 12.0);
    const auto secondRatio = std::pow(2.0, static_cast<double>(voicing.secondVoiceSemitones) / 12.0);
    const auto regeneration = voicing.maxFeedback * controlToUnit(requested.auxA);
    const auto secondLevel = voicing.secondVoiceSemitones == 0.0f ? 0.0f
                                                                  : controlToUnit(requested.auxB);

    /* A tap walking towards the write head plays back faster and sounds higher; one walking
       away sounds lower. The walk rate is (1 - ratio) per sample, normalised by the window, so
       a tap crosses the whole window in exactly the time the pitch shift requires. */
    const auto advance = (1.0 - ratio) / static_cast<double>(window);
    const auto advanceSecond = (1.0 - secondRatio) / static_cast<double>(window);

    // Hann at fifty per cent overlap sums to exactly one, which is what makes the wrap
    // inaudible: as one tap fades out at the end of its window the other is at full level in
    // the middle of its own.
    const auto hann = [](double phase)
    {
        return 0.5f - 0.5f * static_cast<float>(std::cos(2.0 * std::numbers::pi * phase));
    };
    const auto wrap = [](double phase) { return phase - std::floor(phase); };

    for (std::size_t sample = 0; sample < samples; ++sample)
    {
        const auto phase = wrap(pitchPhase + static_cast<double>(sample) * advance);
        const auto offsetPhase = wrap(phase + 0.5);
        const auto secondPhase = wrap(pitchPhaseSecond + static_cast<double>(sample) * advanceSecond);
        const auto secondOffset = wrap(secondPhase + 0.5);

        for (std::size_t channel = 0; channel < count; ++channel)
        {
            // One write per sample per channel; every tap then reads the same history.
            pitchLine.write(channels[channel][sample] + regeneration * pitchFeedback[channel], channel);

            const auto tap = [&](double where)
            {
                return pitchLine.readAt(static_cast<float>(where) * window + 2.0f, channel);
            };
            auto shifted = hann(phase) * tap(phase) + hann(offsetPhase) * tap(offsetPhase);
            if (secondLevel > 0.0f)
                shifted += secondLevel * (hann(secondPhase) * tap(secondPhase)
                                          + hann(secondOffset) * tap(secondOffset));
            pitchLine.advance(channel);

            const auto safe = std::isfinite(shifted) ? shifted : 0.0f;
            pitchFeedback[channel] = safe;
            channels[channel][sample] = safe;
        }
    }

    pitchPhase = wrap(pitchPhase + static_cast<double>(samples) * advance);
    pitchPhaseSecond = wrap(pitchPhaseSecond + static_cast<double>(samples) * advanceSecond);
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

int PedalBoard::activeCost() const noexcept
{
    auto total = 0;
    for (const auto& pedal : slots)
        if (pedal.active()) total += pedalModel(pedal.modelIndex()).tier;
    return total;
}
} // namespace nts::pedals
