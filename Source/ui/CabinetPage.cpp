#include "CabinetPage.h"
#include "CabinetPicker.h"
#include "../PluginProcessor.h"
#include "../TubeForgeTheme.h"

namespace theme = tf::theme;

void CabinetResponseView::setCurves(std::vector<float> slotA, std::vector<float> slotB,
                                    std::vector<float> sum, bool secondMuted)
{
    // Compared before assigning: this arrives from a 20 Hz tick and the curves only change when a
    // response is loaded or the model moves, so most calls have nothing to repaint.
    if (first == slotA && second == slotB && summed == sum && secondIsMuted == secondMuted) return;
    first = std::move(slotA);
    second = std::move(slotB);
    summed = std::move(sum);
    secondIsMuted = secondMuted;
    repaint();
}

void CabinetResponseView::paint(juce::Graphics& graphics)
{
    auto bounds = getLocalBounds().toFloat();
    theme::well(graphics, bounds, 6.0f);
    auto plot = bounds.reduced(10.0f, 8.0f);
    if (summed.size() < 2) return;

    // 24 dB above and below unity. Wide enough for a cabinet's top-end cliff to be visible as a
    // cliff, tight enough that a 3 dB microphone move is not lost in the noise of the axis.
    constexpr auto topDb = 24.0f;
    constexpr auto bottomDb = -24.0f;
    const auto toY = [&plot](float decibels)
    {
        const auto clamped = std::clamp(decibels, bottomDb, topDb);
        return plot.getBottom() - plot.getHeight() * (clamped - bottomDb) / (topDb - bottomDb);
    };

    // Decade gridlines, and unity. The 0 dB line is what every curve is read against, so it is
    // drawn brighter than the rest.
    graphics.setColour(theme::textTertiary.withAlpha(0.25f));
    for (const auto frequency : { 100.0f, 1000.0f, 10000.0f })
    {
        const auto position = std::log(frequency / 40.0f) / std::log(16000.0f / 40.0f);
        const auto x = plot.getX() + plot.getWidth() * position;
        graphics.drawVerticalLine(juce::roundToInt(x), plot.getY(), plot.getBottom());
    }
    graphics.setColour(theme::textTertiary.withAlpha(0.45f));
    graphics.drawHorizontalLine(juce::roundToInt(toY(0.0f)), plot.getX(), plot.getRight());

    const auto strokeCurve = [&](const std::vector<float>& curve, juce::Colour colour, float thickness)
    {
        if (curve.size() < 2) return;
        juce::Path path;
        for (std::size_t point = 0; point < curve.size(); ++point)
        {
            const auto x = plot.getX() + plot.getWidth() * static_cast<float>(point)
                         / static_cast<float>(curve.size() - 1);
            const auto y = toY(curve[point]);
            if (point == 0) path.startNewSubPath(x, y); else path.lineTo(x, y);
        }
        graphics.setColour(colour);
        graphics.strokePath(path, juce::PathStrokeType(thickness));
    };

    /* The two slots thin and dim, the sum bright and thick.

       The sum is what is heard and is therefore what the eye should land on; the slots are there
       to explain it. Drawing all three at equal weight makes a picture nobody can read at a
       glance, which for a plot on a page full of controls is the same as not drawing it. */
    strokeCurve(first, theme::accentDim.withAlpha(0.55f), 1.2f);
    if (! secondIsMuted) strokeCurve(second, theme::warn.withAlpha(0.45f), 1.2f);
    strokeCurve(summed, theme::accent, 2.0f);

    graphics.setColour(theme::textTertiary);
    graphics.setFont(theme::font(9.5f));
    graphics.drawText("40 Hz", bounds.reduced(8.0f, 3.0f), juce::Justification::bottomLeft);
    graphics.drawText("16 kHz", bounds.reduced(8.0f, 3.0f), juce::Justification::bottomRight);
    graphics.drawText("A / B / sum", bounds.reduced(8.0f, 3.0f), juce::Justification::topRight);
}

CabinetPage::CabinetPage(TubeForgeAudioProcessor& processorToUse)
    : ModulePage(processorToUse)
{
    static constexpr std::array captions { "Response A", "Response B" };
    // Parameter ids per slot. Slot B's delay is `cabinetAlignment`, which predates the per-slot
    // block and keeps its id -- see ParameterIds. Listing both here rather than deriving them
    // from the slot index is what stops that exception becoming a silent off-by-one.
    static constexpr std::array levelIds { "cabLevelA", "cabLevelB" };
    static constexpr std::array panIds { "cabPanA", "cabPanB" };
    static constexpr std::array phaseIds { "cabPhaseA", "cabPhaseB" };
    static constexpr std::array delayIds { "cabDelayA", "cabinetAlignment" };
    static constexpr std::array muteIds { "cabMuteA", "cabMuteB" };
    static constexpr std::array modelIds { "cabModelA", "cabModelB" };
    static constexpr std::array micIds { "cabMicA", "cabMicB" };
    static constexpr std::array positionIds { "cabPositionA", "cabPositionB" };
    static constexpr std::array distanceIds { "cabDistanceA", "cabDistanceB" };
    static constexpr std::array lengthIds { "cabIrLengthA", "cabIrLengthB" };
    static constexpr std::array normIds { "cabIrNormA", "cabIrNormB" };
    static constexpr std::array minPhaseIds { "cabIrMinPhaseA", "cabIrMinPhaseB" };

    for (int slot = 0; slot < 2; ++slot)
    {
        const auto index = static_cast<std::size_t>(slot);
        auto& controls = slots[index];
        auto& panel = slotPanels[index];
        tf::ui::configureFieldCaption(controls.caption, captions[index]);
        tf::ui::configureLabel(controls.status, "Built-in cabinet response", 12.0f, false,
                               theme::textSecondary);
        panel.addAndMakeVisible(controls.caption);
        panel.addAndMakeVisible(controls.load);
        panel.addAndMakeVisible(controls.browse);
        panel.addAndMakeVisible(controls.clear);
        panel.addAndMakeVisible(controls.solo);
        panel.addAndMakeVisible(controls.save);
        panel.addAndMakeVisible(controls.status);
        controls.save.onClick = [this, slot] { exportResponse(slot); };
        controls.save.setTooltip("Writes what this slot is convolving to a 24-bit WAV. The reason "
                                 "it is here is Cabinet Match: a fitted cabinet is generated, so "
                                 "without this it would live only inside one project.");
        /* Browse rather than a file dialog, with the dialog one click further in.

           The browser is the everyday path -- a shelf of responses is something you listen
           through -- and the dialog is what you want exactly once, when the response you are
           after is not in the folder you told the browser about. That ordering is the whole
           point: two Load buttons and a modal dialog is workable for three responses and
           hopeless for four hundred. */
        controls.load.onClick = [this, slot] { CabinetPicker::openOver(*this, processor, slot); };
        controls.load.setTooltip("Browses your impulse response folder. Selecting one loads it "
                                 "straight into this slot so you can listen through a pack "
                                 "without a dialog between each one.");
        controls.browse.onClick = [this, slot] { chooseImpulse(slot); };
        controls.browse.setTooltip("Opens a file dialog, for a response that is not in the folder "
                                   "the browser is pointed at.");
        controls.clear.onClick = [this, slot] { processor.clearCabinetIr(slot); };
        controls.solo.onClick = [this, slot] { toggleSolo(slot); };
        controls.solo.setTooltip("Listens to this response on its own by muting the other one. A "
                                 "listening action rather than part of the rig, so it is not saved "
                                 "with the project and releases when the page is left.");

        tf::ui::configureFieldCaption(controls.lengthCaption, "Response length");
        tf::ui::populateFromParameter(controls.length, processor.getParameters(), lengthIds[index]);
        controls.length.setTooltip("How much of the loaded response to use. Past the first few "
                                   "milliseconds an impulse response is mostly room, so a shorter "
                                   "one is drier and cheaper at the same time. Still capped by the "
                                   "performance tier, which is what that setting is for.");
        tf::ui::configureFieldCaption(controls.normalisationCaption, "Normalisation");
        tf::ui::populateFromParameter(controls.normalisation, processor.getParameters(), normIds[index]);
        controls.normalisation.setTooltip("Peak matches the loudest sample, which makes a "
                                          "room-heavy response quieter than a close-mic one at the "
                                          "same setting; RMS matches what is heard, so the blend "
                                          "control means what it says; None leaves the file as its "
                                          "author wrote it.");
        controls.minimumPhase.setTooltip("Rebuilds the response with the same frequency content but "
                                         "all of its energy at the front. Two responses from "
                                         "different sources rarely start at the same place, and "
                                         "that difference combs when they are blended. Off by "
                                         "default: for one response on its own, the phase it was "
                                         "measured with is part of what was measured.");
        panel.addAndMakeVisible(controls.lengthCaption);
        panel.addAndMakeVisible(controls.length);
        panel.addAndMakeVisible(controls.normalisationCaption);
        panel.addAndMakeVisible(controls.normalisation);
        panel.addAndMakeVisible(controls.minimumPhase);

        tf::ui::configureFieldCaption(controls.modelCaption, "Cabinet");
        tf::ui::populateFromParameter(controls.model, processor.getParameters(), modelIds[index]);
        controls.model.setTooltip("The built-in cabinet, built from a description of a speaker in a "
                                  "box rather than sampled from one -- which is what makes the "
                                  "microphone position and distance below continuous instead of a "
                                  "handful of files. Ignored while a response is loaded from disk.");
        tf::ui::configureFieldCaption(controls.micCaption, "Microphone");
        tf::ui::populateFromParameter(controls.microphone, processor.getParameters(), micIds[index]);
        controls.microphone.setTooltip("Which microphone is in front of it. These are archetypes -- a "
                                       "moving coil with a presence peak, a ribbon, a condenser -- "
                                       "not measurements of particular boxes.");
        tf::ui::configureFieldCaption(controls.positionCaption, "Position (cap to edge)");
        tf::ui::configureSlider(controls.position);
        controls.position.setTooltip("Where on the cone the microphone points. At the dust cap it is "
                                     "bright and hard and the cone breakup peaks are at their "
                                     "strongest; out at the edge the top end goes and the low-mid "
                                     "comes up. This is the most useful control on a real cabinet.");
        tf::ui::configureFieldCaption(controls.distanceCaption, "Distance (inches)");
        tf::ui::configureSlider(controls.distance);
        controls.distance.setTooltip("Grille to capsule. Under about six inches the proximity effect "
                                     "adds low end -- a great deal of it on a ribbon, much less on a "
                                     "moving coil. Further back costs top end and picks up the "
                                     "boundary the cabinet is standing on.");
        panel.addAndMakeVisible(controls.modelCaption);
        panel.addAndMakeVisible(controls.model);
        panel.addAndMakeVisible(controls.micCaption);
        panel.addAndMakeVisible(controls.microphone);
        panel.addAndMakeVisible(controls.positionCaption);
        panel.addAndMakeVisible(controls.position);
        panel.addAndMakeVisible(controls.distanceCaption);
        panel.addAndMakeVisible(controls.distance);

        tf::ui::configureFieldCaption(controls.levelCaption, "Level");
        tf::ui::configureSlider(controls.level);
        controls.level.setTooltip("This response's own level going into the blend. Two microphones "
                                  "on a cabinet are never at the same level, and setting that with "
                                  "the blend control alone also changes which one is dominant.");
        tf::ui::configureFieldCaption(controls.panCaption, "Placement");
        tf::ui::configureSlider(controls.pan);
        controls.pan.setTooltip("Where this response sits in the stereo field. Heard only once "
                                "Stereo width is up -- width decides how far apart the two "
                                "responses are placed, and this decides where each of them goes. "
                                "The defaults are the hard left/right split width has always made.");
        tf::ui::configureFieldCaption(controls.delayCaption, "Delay (samples)");
        tf::ui::configureSlider(controls.delay);
        controls.delay.setTooltip("Holds this response back. Two microphones at different distances "
                                  "from a speaker are at different times, and a few samples of that "
                                  "is most of what a mic blend sounds like. Use Align automatically "
                                  "below to measure it rather than guessing.");
        controls.phase.setTooltip("Flips this response's polarity. The first thing to try when a "
                                  "blend of two microphones sounds thinner than either one alone.");
        controls.mute.setTooltip("Silences this response. Also stops the section paying for its "
                                 "convolution, which is the cabinet's largest single cost.");
        panel.addAndMakeVisible(controls.levelCaption);
        panel.addAndMakeVisible(controls.level);
        panel.addAndMakeVisible(controls.panCaption);
        panel.addAndMakeVisible(controls.pan);
        panel.addAndMakeVisible(controls.delayCaption);
        panel.addAndMakeVisible(controls.delay);
        panel.addAndMakeVisible(controls.phase);
        panel.addAndMakeVisible(controls.mute);
        addAndMakeVisible(panel);

        auto& state = processor.getParameters();
        controls.levelAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
            state, levelIds[index], controls.level);
        controls.panAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
            state, panIds[index], controls.pan);
        controls.delayAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
            state, delayIds[index], controls.delay);
        controls.phaseAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
            state, phaseIds[index], controls.phase);
        controls.muteAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
            state, muteIds[index], controls.mute);
        controls.modelAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
            state, modelIds[index], controls.model);
        controls.micAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
            state, micIds[index], controls.microphone);
        controls.positionAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
            state, positionIds[index], controls.position);
        controls.distanceAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
            state, distanceIds[index], controls.distance);
        controls.lengthAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
            state, lengthIds[index], controls.length);
        controls.normalisationAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
            state, normIds[index], controls.normalisation);
        controls.minimumPhaseAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
            state, minPhaseIds[index], controls.minimumPhase);
    }

    tf::ui::configureFieldCaption(blendCaption, "A / B blend");
    tf::ui::configureSlider(blend);
    blend.setTooltip("Mixes the two cabinet responses. At either end only one convolver runs, which "
                     "is also half the cabinet's CPU cost.");
    tf::ui::configureFieldCaption(widthCaption, "Stereo width");
    tf::ui::configureSlider(width);
    width.setTooltip("Separates the two responses across the stereo field instead of summing them: "
                     "one guitar through two different rigs, which is how a wide rhythm sound is "
                     "actually made. Where each one goes is set by its own Placement control. At "
                     "zero the output is unchanged from blend alone.");
    autoAlign.onClick = [this] { applySuggestedAlignment(); };
    autoAlign.setTooltip("Measures how far apart the two loaded responses are by cross-correlating "
                         "them, and writes that into the later one's delay. The alignment control "
                         "has never had a measurement behind it -- this is that measurement.");
    tf::ui::configureLabel(alignmentReading, "", 11.0f, false, theme::textTertiary);

    tf::ui::configureFieldCaption(matchCaption, "Match depth");
    tf::ui::configureSlider(matchDepth);
    matchDepth.setRange(0.0, 100.0, 1.0);
    matchDepth.setValue(70.0, juce::dontSendNotification);
    matchDepth.setTextValueSuffix(" %");
    matchDepth.setTooltip("How much of the measured difference to apply. Not a host parameter: it "
                          "is an argument to the fit rather than a control on the rig, and once a "
                          "matched cabinet is loaded the depth it was built with is baked into it.");
    matchCabinet.onClick = [this] { runCabinetMatch(); };
    matchCabinet.setTooltip("Measures the difference between this rig playing the matched song's "
                            "reference and the reference itself, and builds a cabinet from it into "
                            "slot A. What that corrects for is the whole difference -- the "
                            "amplifier, the microphone, the room and the mastering as well as the "
                            "speaker -- so it is the sound of that record through your rig rather "
                            "than the cabinet that was in the room.");
    tf::ui::configureLabel(matchReading, "", 11.0f, false, theme::textTertiary);
    matchReading.setJustificationType(juce::Justification::topLeft);

    tf::ui::configureFieldCaption(lowCutCaption, "Low cut");
    tf::ui::configureSlider(lowCut);
    lowCut.setTooltip("High-pass after the cabinet. Set by the voicing until now, with no way to "
                      "see or change it; a matched rig also fits it.");
    tf::ui::configureFieldCaption(highCutCaption, "High cut");
    tf::ui::configureSlider(highCut);
    highCut.setTooltip("Low-pass after the cabinet, which is the control that decides how dark the "
                       "speaker is. Song Match fits this from the reference and, until the control "
                       "existed, had nowhere to put the answer.");
    tf::ui::configureFieldCaption(diBlendCaption, "DI blend");
    tf::ui::configureSlider(diBlend);
    diBlend.setTooltip("Blends the cabinet's input back against its output, so what is added is "
                       "still fully distorted -- it bypasses the speaker, not the drive. This is a "
                       "bass rig's DI channel, and it is not the same as the amplifier's Dry blend.");
    tf::ui::configureFieldCaption(outputTrimCaption, "Cabinet output");
    tf::ui::configureSlider(outputTrim);
    outputTrim.setTooltip("Level after the whole section, for putting a mic blend back where it "
                          "started once its parts have been changed.");

    tf::ui::configureLabel(monoFold, "Mono fold: --", 11.0f, false, theme::textSecondary);
    tf::ui::configureLabel(help,
                           "The cabinet runs after whichever engine is selected, so it applies to "
                           "the traditional amp, a neural capture and the physical circuit alike -- "
                           "which is what makes a preamp-only capture usable. Responses are decoded, "
                           "resampled to the current rate, trimmed and normalised before they are "
                           "used, then faded in, so a cabinet can be swapped while you play. The "
                           "file path is saved with the project.",
                           11.0f, false, theme::textTertiary);
    help.setJustificationType(juce::Justification::topLeft);

    addAndMakeVisible(blendPanel);
    addAndMakeVisible(voicingPanel);
    addAndMakeVisible(matchPanel);
    addAndMakeVisible(response);
    addAndMakeVisible(help);
    blendPanel.addAndMakeVisible(cabinetEnabled);
    blendPanel.addAndMakeVisible(blendCaption);
    blendPanel.addAndMakeVisible(blend);
    blendPanel.addAndMakeVisible(widthCaption);
    blendPanel.addAndMakeVisible(width);
    blendPanel.addAndMakeVisible(monoFold);
    blendPanel.addAndMakeVisible(autoAlign);
    matchPanel.addAndMakeVisible(matchCabinet);
    matchPanel.addAndMakeVisible(matchCaption);
    matchPanel.addAndMakeVisible(matchDepth);
    matchPanel.addAndMakeVisible(matchReading);
    blendPanel.addAndMakeVisible(alignmentReading);
    voicingPanel.addAndMakeVisible(lowCutCaption);
    voicingPanel.addAndMakeVisible(lowCut);
    voicingPanel.addAndMakeVisible(highCutCaption);
    voicingPanel.addAndMakeVisible(highCut);
    voicingPanel.addAndMakeVisible(diBlendCaption);
    voicingPanel.addAndMakeVisible(diBlend);
    voicingPanel.addAndMakeVisible(outputTrimCaption);
    voicingPanel.addAndMakeVisible(outputTrim);

    auto& state = processor.getParameters();
    widthAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        state, "cabinetWidth", width);
    cabinetAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        state, "cabinet", cabinetEnabled);
    blendAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        state, "cabinetBlend", blend);
    lowCutAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        state, "cabLowCut", lowCut);
    highCutAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        state, "cabHighCut", highCut);
    diBlendAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        state, "cabDiBlend", diBlend);
    outputTrimAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        state, "cabOutputTrim", outputTrim);
}

void CabinetPage::toggleSolo(int slot)
{
    /* Solo is expressed through the other slot's Mute parameter, which means it has to remember
       what that parameter was before it took it over -- otherwise soloing A and then releasing it
       would un-mute a B the user had muted deliberately. Rather than store that, releasing solo
       simply writes false, and the rule is stated in the tooltip: solo is a listening action and
       it ends by unmuting. Storing a shadow copy of a host parameter is the kind of second source
       of truth that goes wrong when automation writes the real one underneath it. */
    const auto other = slot == 0 ? 1 : 0;
    const auto engaging = soloedSlot != slot;
    soloedSlot = engaging ? slot : -1;

    static constexpr std::array muteIds { "cabMuteA", "cabMuteB" };
    if (auto* parameter = processor.getParameters().getParameter(
            muteIds[static_cast<std::size_t>(other)]))
    {
        parameter->beginChangeGesture();
        parameter->setValueNotifyingHost(engaging ? 1.0f : 0.0f);
        parameter->endChangeGesture();
    }
    // The soloed slot itself cannot stay muted, or solo would produce silence.
    if (engaging)
        if (auto* parameter = processor.getParameters().getParameter(
                muteIds[static_cast<std::size_t>(slot)]))
        {
            parameter->beginChangeGesture();
            parameter->setValueNotifyingHost(0.0f);
            parameter->endChangeGesture();
        }
    refresh();
}

void CabinetPage::applySuggestedAlignment()
{
    const auto suggestion = processor.suggestedCabinetAlignment();
    // Below about a fifth, the two responses have little in common and the peak is as likely to be
    // noise as a real arrival difference. Saying so beats writing a confident-looking number.
    if (suggestion.confidence < 0.2f)
    {
        alignmentReading.setText("These two responses are too unalike to align (confidence "
                                     + juce::String(suggestion.confidence * 100.0f, 0) + "%)",
                                 juce::dontSendNotification);
        alignmentReading.setColour(juce::Label::textColourId, theme::warn);
        return;
    }

    const auto id = suggestion.delaySlotA ? "cabDelayA" : "cabinetAlignment";
    if (auto* parameter = processor.getParameters().getParameter(id))
    {
        const auto range = processor.getParameters().getParameterRange(id);
        parameter->beginChangeGesture();
        parameter->setValueNotifyingHost(
            range.convertTo0to1(static_cast<float>(suggestion.delaySamples)));
        parameter->endChangeGesture();
    }
    // The other slot goes back to zero: a measured alignment is a statement about the pair, and
    // leaving a stale delay on the other side would mean the pair is no longer what was measured.
    if (auto* other = processor.getParameters().getParameter(
            suggestion.delaySlotA ? "cabinetAlignment" : "cabDelayA"))
    {
        other->beginChangeGesture();
        other->setValueNotifyingHost(0.0f);
        other->endChangeGesture();
    }

    alignmentReading.setText("Delayed response " + juce::String(suggestion.delaySlotA ? "A" : "B")
                                 + " by " + juce::String(suggestion.delaySamples)
                                 + " samples (confidence "
                                 + juce::String(suggestion.confidence * 100.0f, 0) + "%)",
                             juce::dontSendNotification);
    alignmentReading.setColour(juce::Label::textColourId, theme::textTertiary);
}

void CabinetPage::runCabinetMatch()
{
    const auto outcome = processor.matchCabinetToReference(
        static_cast<float>(matchDepth.getValue()) * 0.01f);
    if (! outcome.applied)
    {
        matchReading.setText(outcome.error, juce::dontSendNotification);
        matchReading.setColour(juce::Label::textColourId, theme::warn);
        return;
    }
    /* The wording matters more than the number.

       "Closest to" rather than "this is": the correction contains the amplifier's difference, the
       microphone, the room and the mastering as well as the speaker, so naming a cabinet as though
       the fit had identified one would be a claim the measurement cannot support. Saying what it
       resembles is useful and true; saying what it is would be neither. */
    auto text = juce::String("Matched, closest to ")
              + (outcome.nearestCabinet.isEmpty() ? juce::String("a built-in cabinet")
                                                  : outcome.nearestCabinet)
              + " -- confidence " + juce::String(outcome.confidence * 100.0f, 0) + "%.";
    if (outcome.confidence < 0.35f)
        text += " Low confidence: the render and the reference are not very comparable, so this is "
                "a large correction rather than a close fit.";
    matchReading.setText(text, juce::dontSendNotification);
    matchReading.setColour(juce::Label::textColourId,
                           outcome.confidence < 0.35f ? theme::warn : theme::textTertiary);
    refresh();
}

void CabinetPage::setPerformanceLimits(int, bool singleCabinet)
{
    // Eco runs one convolution, so blending a second cabinet in is not something the engine will
    // honour. Disabled rather than moved: the stored value is untouched and comes straight back
    // when the tier is raised.
    blend.setEnabled(! singleCabinet);
    blendCaption.setText(singleCabinet ? "A / B blend (Eco: A only)" : "A / B blend",
                         juce::dontSendNotification);
    // Width needs both convolvers engaged whatever the blend says, so on the one-convolution tier
    // it is not merely ineffective but actively contrary to what the tier is for.
    width.setEnabled(! singleCabinet);
    widthCaption.setText(singleCabinet ? "Stereo width (Eco: off)" : "Stereo width",
                         juce::dontSendNotification);
    /* Slot B's whole strip, for the reason the tier exists.

       A slot's level and placement can hold its convolver engaged whatever the blend says, so
       leaving them live on Eco would let a user quietly buy back the second convolution the tier
       was chosen to avoid -- and be puzzled when the machine started dropping out again. Slot A
       is untouched: one cabinet is exactly what this tier offers. */
    auto& second = slots[1];
    for (auto* control : std::initializer_list<juce::Component*> {
             &second.level, &second.pan, &second.delay, &second.phase, &second.mute,
             &second.load, &second.clear, &second.solo, &second.model, &second.microphone,
             &second.position, &second.distance })
        control->setEnabled(! singleCabinet);
    second.caption.setText(singleCabinet ? "RESPONSE B (ECO: OFF)" : "RESPONSE B",
                           juce::dontSendNotification);
}

void CabinetPage::layOutSlot(juce::Rectangle<int> area, SlotControls& controls)
{
    /* Fields in two columns, not one.

       The page host is about four hundred pixels tall at the minimum window size, and a slot has
       four sliders, two choosers, two switches and three buttons in it. One field per row does not
       fit, and a page whose bottom half is only reachable by growing the window is a page most
       people never see the bottom of. Pairing them also groups what belongs together: the cabinet
       with its microphone, the position with the distance, the level with the placement. */
    const auto columnWidth = (area.getWidth() - 10) / 2;
    const auto pair = [columnWidth](juce::Rectangle<int> row, juce::Label& leftCaption,
                                    juce::Component& left, juce::Label& rightCaption,
                                    juce::Component& right)
    {
        tf::ui::layOutField(row.removeFromLeft(columnWidth), leftCaption, left);
        row.removeFromLeft(10);
        tf::ui::layOutField(row.removeFromLeft(columnWidth), rightCaption, right);
    };

    controls.caption.setBounds(area.removeFromTop(16));
    area.removeFromTop(3);
    auto buttons = area.removeFromTop(26);
    controls.load.setBounds(buttons.removeFromLeft(72));
    buttons.removeFromLeft(5);
    controls.browse.setBounds(buttons.removeFromLeft(50));
    buttons.removeFromLeft(5);
    controls.clear.setBounds(buttons.removeFromLeft(88));
    buttons.removeFromLeft(5);
    controls.solo.setBounds(buttons.removeFromLeft(48));
    buttons.removeFromLeft(5);
    controls.save.setBounds(buttons.removeFromLeft(std::min(buttons.getWidth(), 66)));
    area.removeFromTop(3);
    controls.status.setBounds(area.removeFromTop(15));
    area.removeFromTop(6);

    /* The model's controls are removed rather than greyed out while a file is loaded.

       A control that is present and inert reads as broken; an absent one reads as absent. This is
       the same call the Pedals page makes for a model's unused knobs, and it is why the layout is
       split out of `resized` -- the rows have to re-flow when a response is loaded or cleared, not
       only when the window changes size. */
    if (controls.model.isVisible())
    {
        pair(area.removeFromTop(36), controls.modelCaption, controls.model,
             controls.micCaption, controls.microphone);
        area.removeFromTop(4);
        pair(area.removeFromTop(36), controls.positionCaption, controls.position,
             controls.distanceCaption, controls.distance);
        area.removeFromTop(6);
    }
    else
    {
        // The mirror image: while a file is loaded these are what shape it, and the model chooser
        // above is what has nothing to do.
        pair(area.removeFromTop(36), controls.lengthCaption, controls.length,
             controls.normalisationCaption, controls.normalisation);
        area.removeFromTop(4);
        controls.minimumPhase.setBounds(area.removeFromTop(24)
                                            .withWidth(std::min(area.getWidth(), 180)));
        area.removeFromTop(18);
    }
    pair(area.removeFromTop(36), controls.levelCaption, controls.level,
         controls.panCaption, controls.pan);
    area.removeFromTop(4);
    auto lastRow = area.removeFromTop(36);
    tf::ui::layOutField(lastRow.removeFromLeft(columnWidth), controls.delayCaption, controls.delay);
    lastRow.removeFromLeft(10);
    // The two switches share the second column of the last row, which is the only place they fit
    // without pushing the panel taller than the smallest window this editor allows.
    auto toggles = lastRow.removeFromLeft(columnWidth).withTrimmedTop(14);
    controls.phase.setBounds(toggles.removeFromLeft(std::min(toggles.getWidth(), 132)));
    controls.mute.setBounds(toggles);
}

void CabinetPage::resized()
{
    auto area = getLocalBounds();

    /* Laid out from the bottom up, because the two fixed things are at the bottom.

       The help paragraph and the section controls have a size they need; the slot panels have a
       size they need; the response plot is the one element that simply gets better with more
       room. So everything else is taken out first and the plot is given what is left -- and
       hidden outright when that is not enough to draw a legible curve, which is what happens at
       the smallest window size this editor allows. */
    help.setBounds(area.removeFromBottom(46).withTrimmedLeft(13));
    area.removeFromBottom(8);

    auto lowerRow = area.removeFromBottom(std::min(168, std::max(140, area.getHeight() / 3)));
    area.removeFromBottom(10);

    // The slots sit side by side rather than stacked: they are a pair being compared, and a
    // comparison read down a column is a worse comparison than one read across a row.
    const auto slotWidth = (area.getWidth() - 12) / 2;
    auto slotRow = area.removeFromTop(std::max(232, area.getHeight() - 96));
    slotPanels[0].setBounds(slotRow.removeFromLeft(slotWidth));
    slotRow.removeFromLeft(12);
    slotPanels[1].setBounds(slotRow.removeFromLeft(slotWidth));

    area.removeFromTop(10);
    const auto plotVisible = area.getHeight() >= 70;
    response.setVisible(plotVisible);
    if (plotVisible) response.setBounds(area);

    // Three columns: what the section does, how it is voiced, and the match. Three rather than
    // two because the match needs a line of prose under it -- what it produced and how much to
    // believe it -- and prose squeezed into a third of a panel is prose nobody reads.
    const auto panelWidth = (lowerRow.getWidth() - 24) / 3;
    blendPanel.setBounds(lowerRow.removeFromLeft(panelWidth));
    lowerRow.removeFromLeft(12);
    voicingPanel.setBounds(lowerRow.removeFromLeft(panelWidth));
    lowerRow.removeFromLeft(12);
    matchPanel.setBounds(lowerRow);

    for (std::size_t index = 0; index < slots.size(); ++index)
        layOutSlot(slotPanels[index].contentArea(), slots[index]);

    auto blendArea = blendPanel.contentArea();
    cabinetEnabled.setBounds(blendArea.removeFromTop(24)
                                 .withWidth(std::min(blendArea.getWidth(), 340)));
    blendArea.removeFromTop(2);
    tf::ui::layOutField(blendArea.removeFromTop(34), blendCaption, blend);
    tf::ui::layOutField(blendArea.removeFromTop(34), widthCaption, width);
    monoFold.setBounds(blendArea.removeFromTop(15));
    blendArea.removeFromTop(3);
    auto alignRow = blendArea.removeFromTop(24);
    autoAlign.setBounds(alignRow.removeFromLeft(std::min(alignRow.getWidth(), 160)));
    alignmentReading.setBounds(alignRow.withTrimmedLeft(8));


    auto voicingArea = voicingPanel.contentArea();
    const auto voicingColumn = (voicingArea.getWidth() - 10) / 2;
    const auto voicingPair = [voicingColumn](juce::Rectangle<int> row, juce::Label& leftCaption,
                                             juce::Component& left, juce::Label& rightCaption,
                                             juce::Component& right)
    {
        tf::ui::layOutField(row.removeFromLeft(voicingColumn), leftCaption, left);
        row.removeFromLeft(10);
        tf::ui::layOutField(row.removeFromLeft(voicingColumn), rightCaption, right);
    };
    voicingPair(voicingArea.removeFromTop(36), lowCutCaption, lowCut, highCutCaption, highCut);
    voicingArea.removeFromTop(6);
    voicingPair(voicingArea.removeFromTop(36), diBlendCaption, diBlend,
                outputTrimCaption, outputTrim);

    auto matchArea = matchPanel.contentArea();
    matchCabinet.setBounds(matchArea.removeFromTop(26)
                               .withWidth(std::min(matchArea.getWidth(), 170)));
    matchArea.removeFromTop(6);
    tf::ui::layOutField(matchArea.removeFromTop(36), matchCaption, matchDepth);
    matchArea.removeFromTop(4);
    matchReading.setBounds(matchArea);
}

void CabinetPage::refresh()
{
    for (int slot = 0; slot < 2; ++slot)
    {
        auto& controls = slots[static_cast<std::size_t>(slot)];
        const auto status = processor.cabinetIrStatusText(slot);
        controls.status.setText(status, juce::dontSendNotification);
        // A missing or unreadable response still sounds -- the built-in one takes over -- so it
        // is a warning rather than an error, but it has to be visible or the user is left
        // wondering why their cabinet is not the one they chose.
        const auto unresolved = status.startsWith("Missing:") || status.contains("Could not")
                             || status.contains("longer than");
        controls.status.setColour(juce::Label::textColourId,
                                  unresolved ? theme::warn : theme::textSecondary);
        const auto hasUserResponse = processor.cabinetIrFile(slot) != juce::File {};
        controls.clear.setEnabled(hasUserResponse);
        controls.solo.setToggleState(soloedSlot == slot, juce::dontSendNotification);

        // The model controls describe the built-in response, so they go away when a file is
        // sounding instead. Re-laid out only when that actually changed: `resized` is not free and
        // this runs twenty times a second.
        if (controls.model.isVisible() == hasUserResponse)
        {
            for (auto* control : std::initializer_list<juce::Component*> {
                     &controls.modelCaption, &controls.model, &controls.micCaption,
                     &controls.microphone, &controls.positionCaption, &controls.position,
                     &controls.distanceCaption, &controls.distance })
                control->setVisible(! hasUserResponse);
            for (auto* control : std::initializer_list<juce::Component*> {
                     &controls.lengthCaption, &controls.length, &controls.normalisationCaption,
                     &controls.normalisation, &controls.minimumPhase })
                control->setVisible(hasUserResponse);
            resized();
        }
    }

    /* What a mix bus will do to the stereo image, which is the reading the width control needs
       next to it rather than in a manual.

       Two different responses split left and right decorrelate spectrally and fold to mono nearly
       intact. A slot delay, once width is up, is an inter-channel delay -- a comb filter under
       summing, wide on speakers and hollow the instant anything sums it, and nothing the user
       hears while monitoring in stereo tells them so. -3 dB is the uncorrelated case and
       unremarkable; past -6 dB something is cancelling. */
    response.setCurves(processor.cabinetResponseCurve(0), processor.cabinetResponseCurve(1),
                       processor.cabinetSumResponseCurve(),
                       slots[1].mute.getToggleState() || ! slots[1].level.isEnabled());

    // Nothing to match to until a song has been analysed, and a button that reports that only
    // once pressed is a button that wastes a press.
    matchCabinet.setEnabled(processor.cabinetMatchAvailable());

    /* What Auto Match is doing to this page.

       Only the released controls are coloured -- see `ModulePage::markAutoMatch` -- and the badge
       states the rest. The set the analyzer owns here is the four controls that predate the
       cabinet stage; the model and the per-slot settings are not fitted by a match and are
       deliberately not claimed. `cabinetAlignment` is slot B's delay, which is why the marking
       lands on that slider rather than on one of the section's own. */
    markAutoMatch(blend, processor.autoMatchReleased("cabinetBlend"));
    markAutoMatch(width, processor.autoMatchReleased("cabinetWidth"));
    markAutoMatch(slots[1].delay, processor.autoMatchReleased("cabinetAlignment"));
    markAutoMatch(lowCut, processor.autoMatchReleased("cabLowCut"));
    markAutoMatch(highCut, processor.autoMatchReleased("cabHighCut"));
    markAutoMatch(diBlend, processor.autoMatchReleased("cabDiBlend"));
    /* The badge says which of the three things is true, because they need different actions.

       "Holding" means the controls are the analyzer's; "holding the matched cabinet" adds that the
       response in slot A is too, which is the one Auto Match cannot put back if you replace it --
       it was generated, not loaded. "Replaced" is the state worth naming loudest: nothing is
       broken, but the cabinet the match built is gone and only pressing Match again brings it
       back. */
    const auto hold = processor.cabinetHoldState();
    setAutoMatchBadge(
        processor.autoMatchState() == tf::automatch::State::off ? juce::String()
        : hold == TubeForgeAudioProcessor::CabinetHold::held
            ? "Auto Match is holding the cabinet section and the matched response"
        : hold == TubeForgeAudioProcessor::CabinetHold::released
            ? "Auto Match is holding the cabinet section -- its matched response was replaced"
            : "Auto Match is holding the cabinet section");

    const auto foldDb = processor.cabinetMonoFoldDb();
    monoFold.setText("Mono fold: " + juce::String(foldDb, 1) + " dB"
                         + (foldDb < -6.0f ? " -- cancellation, check alignment" : ""),
                     juce::dontSendNotification);
    monoFold.setColour(juce::Label::textColourId,
                       foldDb < -6.0f ? theme::warn : theme::textSecondary);
}

void CabinetPage::chooseImpulse(int slot)
{
    fileChooser = std::make_unique<juce::FileChooser>("Choose a cabinet impulse response",
                                                      processor.cabinetIrFile(slot),
                                                      "*.wav;*.aiff;*.aif;*.flac");
    // Async: the modal variant blocks the message thread, which some hosts do not tolerate.
    fileChooser->launchAsync(juce::FileBrowserComponent::openMode
                                 | juce::FileBrowserComponent::canSelectFiles,
        [safeThis = juce::Component::SafePointer<CabinetPage>(this), slot](const juce::FileChooser& chooser)
        {
            if (safeThis == nullptr || chooser.getResult() == juce::File {}) return;
            safeThis->processor.requestCabinetIrLoad(slot, chooser.getResult());
        });
}

void CabinetPage::exportResponse(int slot)
{
    const auto suggested = juce::File::getSpecialLocation(juce::File::userMusicDirectory)
                               .getChildFile("TubeForge cabinet " + juce::String(slot == 0 ? "A" : "B")
                                             + ".wav");
    fileChooser = std::make_unique<juce::FileChooser>("Export this cabinet response", suggested, "*.wav");
    fileChooser->launchAsync(juce::FileBrowserComponent::saveMode
                                 | juce::FileBrowserComponent::canSelectFiles
                                 | juce::FileBrowserComponent::warnAboutOverwriting,
        [safeThis = juce::Component::SafePointer<CabinetPage>(this), slot](const juce::FileChooser& chooser)
        {
            if (safeThis == nullptr || chooser.getResult() == juce::File {}) return;
            const auto result = safeThis->processor.exportCabinetResponse(slot, chooser.getResult());
            // Reported in the slot's own status line rather than an alert: it is a small outcome
            // about one slot, and it belongs where that slot's other news appears.
            if (! result.wasOk())
                safeThis->slots[static_cast<std::size_t>(slot)].status.setText(
                    result.getErrorMessage(), juce::dontSendNotification);
        });
}

int CabinetPage::slotAt(juce::Point<int> position) const
{
    for (int slot = 0; slot < 2; ++slot)
        if (slotPanels[static_cast<std::size_t>(slot)].getBounds().contains(position)) return slot;
    return -1;
}

bool CabinetPage::isInterestedInFileDrag(const juce::StringArray& files)
{
    // Exactly one file. A multi-file drop has no obvious meaning here -- two responses onto one
    // slot is a question, not an instruction -- and guessing at it is worse than declining.
    if (files.size() != 1) return false;
    const auto extension = juce::File(files[0]).getFileExtension().toLowerCase();
    return extension == ".wav" || extension == ".aiff" || extension == ".aif" || extension == ".flac";
}

void CabinetPage::fileDragEnter(const juce::StringArray&, int x, int y)
{
    dragTargetSlot = slotAt({ x, y });
    repaint();
}

void CabinetPage::fileDragMove(const juce::StringArray&, int x, int y)
{
    if (const auto slot = slotAt({ x, y }); slot != dragTargetSlot)
    {
        dragTargetSlot = slot;
        repaint();
    }
}

void CabinetPage::fileDragExit(const juce::StringArray&)
{
    dragTargetSlot = -1;
    repaint();
}

void CabinetPage::filesDropped(const juce::StringArray& files, int x, int y)
{
    const auto slot = slotAt({ x, y });
    dragTargetSlot = -1;
    repaint();
    // A drop that lands between the panels does nothing, rather than picking a slot for the user.
    // There are only two of them and they are large; missing both is a miss.
    if (slot < 0 || files.isEmpty()) return;
    processor.requestCabinetIrLoad(slot, juce::File(files[0]));
}

void CabinetPage::paintOverChildren(juce::Graphics& graphics)
{
    if (dragTargetSlot >= 0)
    {
        // Drawn over the children so the outline reads as being around the whole panel rather than
        // behind the controls in it.
        const auto bounds = slotPanels[static_cast<std::size_t>(dragTargetSlot)]
                                .getBounds().toFloat().reduced(1.0f);
        graphics.setColour(theme::accent.withAlpha(0.12f));
        graphics.fillRoundedRectangle(bounds, 9.0f);
        graphics.setColour(theme::accent);
        graphics.drawRoundedRectangle(bounds, 9.0f, 2.0f);
    }
    /* And then the base class's own overlay, which is not optional.

       `ModulePage::paintOverChildren` draws the song-match veil and the Auto Match badge. An
       override that forgets to call it does not fail loudly -- it silently removes the one thing
       telling the user that the analyzer is holding these controls, on the page where that is
       least obvious. */
    ModulePage::paintOverChildren(graphics);
}
