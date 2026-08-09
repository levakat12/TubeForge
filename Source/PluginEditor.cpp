#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "TubeForgeTheme.h"

#include "ui/AmplifierPage.h"
#include "ui/AutoMatchDialog.h"
#include "ui/CabinetPage.h"
#include "ui/CapturesPage.h"
#include "ui/TunerPage.h"
#include "ui/CircuitPage.h"
#include "ui/NeuralCapturePage.h"
#include "ui/PedalboardPage.h"
#include "ui/ProfileLibraryPage.h"
#include "ui/SongMatchPage.h"
#include "ui/ToneAnalyzerPage.h"
#include "ui/ToneAssistantPage.h"
#include "ui/ToneShapingPage.h"
#include "ui/UiSupport.h"

#include <algorithm>
#include <cmath>

namespace theme = tf::theme;

namespace
{
/// The navigation icons and the caption above the page both read from this table, so a module
/// can never show one name in the toolbar and another above its controls.
struct ModuleDescriptor
{
    const char* name;
    const char* tag;
    const char* blurb;
    int group;
    tf::ui::Glyph glyph;
};

constexpr std::array<ModuleDescriptor, TubeForgeAudioProcessorEditor::moduleCount> moduleTable { {
    { "Amplifier", "Core tone",
      "Set the sound here: drive, EQ and level. Everything else in TubeForge refines what you dial in on this page.",
      0, tf::ui::Glyph::amplifier },
    { "Pedals", "In front of the amp",
      "Up to four pedals ahead of the amplifier: built-in drives and a compressor, or a Neural "
      "Amp Modeler pedal capture. Leave every slot on None for amp and cabinet alone.",
      0, tf::ui::Glyph::pedalboard },
    { "Tone Shaping", "Fine control",
      "Per-stage gain, filtering, feel, power-section behaviour and the noise gate. Reach for these once the amp is close.",
      0, tf::ui::Glyph::toneShaping },
    { "Tuner", "Get in tune",
      "Tune up before anything else. Reads the input ahead of the amplifier, so gain and cabinet do not affect it.", 0 },
    { "Cabinet", "Speaker response",
      "Load your own impulse responses, or keep the built-in cabinets. Responses fade in, so you can swap one while playing.",
      0, tf::ui::Glyph::cabinet },
    { "Neural Capture", "Amp models",
      "Play through a captured amp. Load a model folder exported by the capture wizard, then choose what you monitor.",
      1, tf::ui::Glyph::neuralCapture },
    { "Captures", "NAM library",
      "Import Neural Amp Modeler .zip archives and .nam files. Everything inside is converted "
      "here, in the plug-in, and sorted into amps and pedals by what the capture says it is.",
      1, tf::ui::Glyph::captures },
    { "Tone Assistant", "Suggestions",
      "Suggests bounded changes based on what it hears. Every suggestion is previewed first; nothing is applied without you.",
      1, tf::ui::Glyph::toneAssistant },
    { "Profile Library", "Rigs & sharing",
      "Browse, load and share .ntone rig profiles. Imports are validated before anything can reach the audio engine.",
      1, tf::ui::Glyph::profileLibrary },
    { "Circuit", "Build the amp",
      "Choose real components: tubes, power topology, tone stack. Changes compile off the audio thread and crossfade in.",
      2, tf::ui::Glyph::circuit },
    { "Tone Analyzer", "Measure a clip",
      "Analyse an isolated guitar or bass clip offline and read back its tonal fingerprint and how sure the analysis is.",
      2, tf::ui::Glyph::toneAnalyzer },
    { "Song Match", "Rebuild a tone",
      "Import a song you own, pick the part you want, and get editable rigs that get close to it.",
      2, tf::ui::Glyph::songMatch } } };

// Fixed heights for the three chrome bands. The page stage takes whatever is left over.
constexpr int navBarHeight = 54;
constexpr int railHeight = 116;
constexpr int footerHeight = 28;
constexpr int diagnosticsHeight = 20;
/** Rail cluster widths, each sized for what it actually holds rather than for symmetry.

    They were both 372, which was wrong in both directions. The left cluster carries the input trim,
    two selectors and the switch column -- 526 px of cells -- so `removeFromLeft` ran the rectangle
    dry partway through: the Performance selector was clamped to 98 px instead of 140, and the switch
    cell was then built by `withSizeKeepingCentre` from an *empty* rectangle, which re-expanded it
    around the cluster's right edge and dropped Bypass and Pro on top of that selector. The right
    cluster reserved the same 372 and used 88 of it, so the space the left one needed was sitting
    unused next to it.

    The preset block between them is still centred, because it centres itself inside whatever is
    left (`withSizeKeepingCentre` on the remaining rail) rather than relying on the two clusters
    being equal. At the 1040 px minimum window width this leaves it 366 px, against the 380 it asks
    for at its widest -- so it clamps gracefully rather than colliding.
*/
constexpr int railLeftClusterWidth = 526;
constexpr int railRightClusterWidth = 108;

juce::String statusName(nts::diagnostics::AssetLoadStatus status)
{
    switch (status)
    {
        case nts::diagnostics::AssetLoadStatus::unavailable: return "N/A";
        case nts::diagnostics::AssetLoadStatus::idle: return "Idle";
        case nts::diagnostics::AssetLoadStatus::loading: return "Loading";
        case nts::diagnostics::AssetLoadStatus::ready: return "Ready";
        case nts::diagnostics::AssetLoadStatus::failed: return "Failed";
    }
    return "Unknown";
}
} // namespace

TubeForgeAudioProcessorEditor::TubeForgeAudioProcessorEditor(TubeForgeAudioProcessor& owner)
    : juce::AudioProcessorEditor(owner), processor(owner)
{
    setLookAndFeel(&lookAndFeel);

    tf::ui::configureLabel(title, "TUBEFORGE", 16.0f, true, theme::textPrimary);
    /* The version and the build date, visible rather than buried.

       Six build trees exist in this repository and the plug-in is not copied to a system VST3
       folder, so "am I running the binary that has my change in it" is a question that comes up
       constantly and had no answer inside the running application. Compiled-in timestamps are
       normally worth avoiding -- they defeat reproducible builds -- but this is a label in an
       editor, not an artefact anyone ships hashes of, and the alternative is diagnosing stale
       binaries from screenshots. */
    tf::ui::configureLabel(productTagline,
                           juce::String("NEURAL AMPLIFIER STUDIO   v") + TUBEFORGE_VERSION_STRING,
                           8.0f, true, theme::textTertiary);
    productTagline.setTooltip(juce::String("TubeForge ") + TUBEFORGE_VERSION_STRING + "\nBuilt "
                              + __DATE__ + " " + __TIME__
                              + "\nIf this date is older than a change you expect to see, the host is "
                                "loading a different build than the one you think it is.");
    title.setTooltip(productTagline.getTooltip());

    tf::ui::configureFieldCaption(presetCaption, "Preset");
    presetCaption.setJustificationType(juce::Justification::centred);
    tf::ui::configureLabel(presetName, "Default Rig", 12.5f, false, theme::textPrimary);
    presetName.setJustificationType(juce::Justification::centred);

    tf::ui::configureFieldCaption(engineCaption, "Engine");
    tf::ui::configureLabel(mode, processor.modeName(), 9.5f, true, theme::textSecondary);
    tf::ui::configureLabel(deviceStatus, processor.deviceStatusText(), 9.5f, false, theme::textTertiary);
    tf::ui::configureLabel(diagnosticsText, "Diagnostics waiting for audio...", 9.5f, false,
                           theme::textTertiary);
    diagnosticsText.setJustificationType(juce::Justification::centredRight);

    tf::ui::configureFieldCaption(inputLabel, "Input");
    tf::ui::configureFieldCaption(outputLabel, "Output");
    inputLabel.setJustificationType(juce::Justification::centred);
    outputLabel.setJustificationType(juce::Justification::centred);
    tf::ui::configureKnob(inputGain, false);
    tf::ui::configureKnob(outputGain, false);
    inputGain.setTextValueSuffix(" dB");
    outputGain.setTextValueSuffix(" dB");
    inputGain.setTooltip("Trim into the whole chain, ahead of the pedals and every engine. Use it to "
                         "get a guitar to the level the amp expects -- a hotter signal drives the "
                         "front end harder, exactly as a louder pickup would.");
    outputGain.setTooltip("Level after everything, including the effects. Purely a volume control: "
                          "it changes nothing about the tone, so use it to match levels rather than "
                          "to find one.");
    inputLabel.setTooltip(inputGain.getTooltip());
    outputLabel.setTooltip(outputGain.getTooltip());

    tf::ui::populateFromParameter(engineModeSelector, processor.getParameters(), "engineMode");
    tf::ui::configureFieldCaption(performanceCaption, "Performance");
    tf::ui::populateFromParameter(performanceSelector, processor.getParameters(), "performanceTier");
    performanceSelector.setTooltip(
        "How much work the engine is allowed to do. Eco caps oversampling at 1x, runs a single "
        "cabinet, shortens impulse responses and approximates the saturation curves -- roughly a "
        "third of Studio's CPU. Studio lifts every limit.");
    bypass.setColour(juce::ToggleButton::tickColourId, theme::bad);
    bypass.setTooltip("Pass the dry signal straight through, bypassing the whole chain.");
    proMode.setTooltip("Show the engineering modules and the live diagnostics readout.");
    presetPrevious.setTooltip("Previous profile in the library");
    presetNext.setTooltip("Next profile in the library");
    presetBrowse.setTooltip("Open the profile library");
    resetVoicing.setTooltip("Put every amplifier control back to what the selected voicing "
                            "specifies. Keeps the instrument and the voicing; leaves the "
                            "pedalboard and the sends alone.");
    saveProject.setTooltip("Save the whole session as a .tforge project");
    openProject.setTooltip("Open a .tforge project");
    audioSettings.setTooltip("Audio device settings");
    engineModeSelector.setTooltip("Which engine renders the amp: the traditional model, a loaded "
                                  "neural capture, or the physical circuit solver.");

    buildPages();
    buildNavigation();

    for (auto* component : std::initializer_list<juce::Component*> {
             &title, &productTagline, &presetCaption, &presetName, &presetPrevious, &presetNext,
             &presetBrowse, &resetVoicing, &engineCaption, &inputLabel, &outputLabel, &inputMeter, &outputMeter,
             &pageHost, &mode, &deviceStatus, &signalChain, &diagnosticsText, &inputGain,
             &outputGain, &bypass, &proMode, &engineModeSelector, &audioSettings, &openProject,
             &saveProject, &performanceCaption, &performanceSelector })
        addAndMakeVisible(*component);

    inputAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.getParameters(), "input", inputGain);
    outputAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.getParameters(), "output", outputGain);
    bypassAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        processor.getParameters(), "bypass", bypass);
    engineModeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        processor.getParameters(), "engineMode", engineModeSelector);
    performanceAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        processor.getParameters(), "performanceTier", performanceSelector);
    // Controls the tier overrides read as unavailable rather than merely ignored, so the reason a
    // knob has stopped doing anything is visible instead of being a mystery.
    performanceSelector.onChange = [this] { applyPerformanceTierVisibility(); };

    audioSettings.onClick = [this]
    {
        const auto message = processor.modeName() == "Standalone"
            ? "Open the Audio settings tab to select ASIO/WASAPI devices, channels, sample rate, and buffer size."
            : "Audio devices, sample rate, channels, and buffer size are controlled by the plugin host.";
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "Audio settings", message);
    };
    saveProject.onClick = [this] { chooseProjectToSave(); };
    openProject.onClick = [this] { chooseProjectToOpen(); };
    proMode.onClick = [this] { applyProModeVisibility(); };
    presetPrevious.onClick = [this] { if (library != nullptr) library->selectRelative(-1); };
    presetNext.onClick = [this] { if (library != nullptr) library->selectRelative(1); };
    presetBrowse.onClick = [this] { setActiveModule(libraryModule); };
    resetVoicing.onClick = [this]
    {
        processor.loadVoicingDefaults();
        presetName.setText("Voicing default", juce::dontSendNotification);
    };

    setActiveModule(0);
    applyProModeVisibility();
    applyPerformanceTierVisibility();

    setResizable(true, true);
    setResizeLimits(1040, 700, 1800, 1200);
    setSize(1240, 820);
    // Lets the audio thread stop filling the display-only queues when nothing is draining them.
    owner.setEditorActive(true);
    startTimerHz(20);
}

TubeForgeAudioProcessorEditor::~TubeForgeAudioProcessorEditor()
{
    processor.setEditorActive(false);
    setLookAndFeel(nullptr);
}

void TubeForgeAudioProcessorEditor::buildPages()
{
    // Order must match moduleTable.
    pages[0] = std::make_unique<AmplifierPage>(processor);
    pages[1] = std::make_unique<PedalboardPage>(processor);
    pages[2] = std::make_unique<ToneShapingPage>(processor);
    pages[3] = std::make_unique<TunerPage>(processor);
    pages[4] = std::make_unique<CabinetPage>(processor);
    pages[5] = std::make_unique<NeuralCapturePage>(processor);
    pages[6] = std::make_unique<CapturesPage>(processor);
    pages[7] = std::make_unique<ToneAssistantPage>(processor);
    auto profileLibrary = std::make_unique<ProfileLibraryPage>(processor);
    library = profileLibrary.get();
    pages[libraryModule] = std::move(profileLibrary);
    pages[9] = std::make_unique<CircuitPage>(processor);
    pages[10] = std::make_unique<ToneAnalyzerPage>(processor);
    pages[11] = std::make_unique<SongMatchPage>(processor);

    // The library is the only page with anything to say to the shell.
    library->onRigLoaded = [this](juce::String name)
    { presetName.setText(name, juce::dontSendNotification); };
    library->onProfileCountChanged = [this](int count)
    {
        presetPrevious.setEnabled(count > 1);
        presetNext.setEnabled(count > 1);
    };

    for (auto& page : pages)
        pageHost.addChildComponent(*page);
}

void TubeForgeAudioProcessorEditor::buildNavigation()
{
    for (int index = 0; index < moduleCount; ++index)
    {
        const auto& descriptor = moduleTable[static_cast<std::size_t>(index)];
        auto item = std::make_unique<tf::ui::IconButton>(descriptor.glyph, descriptor.name, true);
        // The icons carry no text, so the tooltip is the only place the description can live.
        item->setTooltip(juce::String(descriptor.name) + " -- " + descriptor.blurb);
        item->onClick = [this, index] { setActiveModule(index); };
        addAndMakeVisible(*item);
        navIcons.push_back(std::move(item));
    }
}

void TubeForgeAudioProcessorEditor::setActiveModule(int index)
{
    activeModule = std::clamp(index, 0, moduleCount - 1);
    for (int page = 0; page < moduleCount; ++page)
    {
        pages[static_cast<std::size_t>(page)]->setVisible(page == activeModule);
        navIcons[static_cast<std::size_t>(page)]->setToggleState(page == activeModule,
                                                                 juce::dontSendNotification);
    }
    const auto& descriptor = moduleTable[static_cast<std::size_t>(activeModule)];
    moduleCaption = juce::String(descriptor.name) + "   /   " + descriptor.tag;
    // Pull state in now rather than leaving the page blank until the next tick.
    pages[static_cast<std::size_t>(activeModule)]->refresh();
    repaint();
}

void TubeForgeAudioProcessorEditor::applyProModeVisibility()
{
    const auto pro = proMode.getToggleState();
    for (int index = firstProModule; index < moduleCount; ++index)
        navIcons[static_cast<std::size_t>(index)]->setVisible(pro);
    diagnosticsText.setVisible(pro);
    if (! pro && activeModule >= firstProModule)
        setActiveModule(0);
    resized();
    repaint();
}

void TubeForgeAudioProcessorEditor::applyPerformanceTierVisibility()
{
    const auto limits = processor.tierLimits();
    // The pages own their own controls, so the shell tells them what the tier has taken rather
    // than reaching into them. Anything a page does not care about is ignored.
    for (auto& page : pages)
        page->setPerformanceLimits(limits.maximumOversamplingFactor, limits.singleCabinet);
    repaint();
}

void TubeForgeAudioProcessorEditor::paint(juce::Graphics& graphics)
{
    graphics.setGradientFill(juce::ColourGradient(theme::backdropTop, 0.0f, 0.0f, theme::backdropBottom,
                                                  0.0f, static_cast<float>(getHeight()), false));
    graphics.fillAll();

    // The two chrome bands at the top are one continuous surface, lifted a shade off the
    // backdrop and separated from each other -- and from the page -- by single hairlines.
    graphics.setColour(theme::shell);
    graphics.fillRect(navBounds.getUnion(railBounds));
    theme::rule(graphics, navBounds.withHeight(1).withY(navBounds.getBottom() - 1));
    theme::rule(graphics, railBounds.withHeight(1).withY(railBounds.getBottom() - 1));
    theme::rule(graphics, footerBounds.withHeight(1), 0.7f);

    for (const auto x : navDividerX)
        if (x > 0) theme::divider(graphics, x, navBounds.getCentreY(), 20, 0.9f);
    for (const auto x : railDividerX)
        if (x > 0) theme::divider(graphics, x, railBounds.getCentreY(), railBounds.getHeight() - 34);

    theme::stage(graphics, stageBounds.toFloat(), 8.0f);
    theme::caption(graphics, captionBounds, moduleCaption, theme::textSecondary);

    if (! presetName.getBounds().isEmpty())
        theme::well(graphics, presetName.getBounds().toFloat().expanded(2.0f, 4.0f), 5.0f);
}

void TubeForgeAudioProcessorEditor::resized()
{
    auto area = getLocalBounds();

    navBounds = area.removeFromTop(navBarHeight);
    railBounds = area.removeFromTop(railHeight);
    footerBounds = area.removeFromBottom(footerHeight
                                         + (diagnosticsText.isVisible() ? diagnosticsHeight : 0));
    stageBounds = area.reduced(16, 14);

    layOutNavigation();
    layOutRail();

    auto footer = footerBounds.reduced(18, 0);
    if (diagnosticsText.isVisible())
        diagnosticsText.setBounds(footer.removeFromBottom(diagnosticsHeight));
    audioSettings.setBounds(footer.removeFromLeft(22).withSizeKeepingCentre(22, 22));
    footer.removeFromLeft(10);
    mode.setBounds(footer.removeFromLeft(78));
    deviceStatus.setBounds(footer.removeFromLeft(std::max(0, footer.getWidth() - 470)));
    signalChain.setBounds(footer);

    auto stage = stageBounds.reduced(18, 14);
    captionBounds = stage.removeFromTop(14);
    stage.removeFromTop(12);
    pageHost.setBounds(stage);
    for (auto& page : pages)
        page->setBounds(pageHost.getLocalBounds());
}

void TubeForgeAudioProcessorEditor::layOutNavigation()
{
    constexpr int iconWidth = 44;
    constexpr int iconHeight = 34;
    constexpr int iconGap = 2;
    constexpr int groupGap = 20;

    auto brand = navBounds.withTrimmedLeft(20).withWidth(180).reduced(0, 9);
    title.setBounds(brand.removeFromTop(18));
    productTagline.setBounds(brand);

    // Centre the whole strip of icons in the window, including the gaps between groups, so it
    // stays centred when PRO mode adds or removes the last three.
    auto width = 0;
    auto lastGroup = -1;
    for (int index = 0; index < moduleCount; ++index)
    {
        if (! navIcons[static_cast<std::size_t>(index)]->isVisible()) continue;
        const auto group = moduleTable[static_cast<std::size_t>(index)].group;
        if (lastGroup >= 0) width += group == lastGroup ? iconGap : groupGap;
        width += iconWidth;
        lastGroup = group;
    }

    auto x = navBounds.getCentreX() - width / 2;
    const auto y = navBounds.getCentreY() - iconHeight / 2;
    navDividerX = {};
    auto divider = 0;
    lastGroup = -1;
    for (int index = 0; index < moduleCount; ++index)
    {
        auto& item = *navIcons[static_cast<std::size_t>(index)];
        if (! item.isVisible()) continue;
        const auto group = moduleTable[static_cast<std::size_t>(index)].group;
        if (lastGroup >= 0)
        {
            if (group == lastGroup)
            {
                x += iconGap;
            }
            else
            {
                if (divider < static_cast<int>(navDividerX.size()))
                    navDividerX[static_cast<std::size_t>(divider++)] = x + groupGap / 2;
                x += groupGap;
            }
        }
        item.setBounds(x, y, iconWidth, iconHeight);
        x += iconWidth;
        lastGroup = group;
    }
}

void TubeForgeAudioProcessorEditor::layOutRail()
{
    auto rail = railBounds.reduced(20, 10);
    auto leftCluster = rail.removeFromLeft(railLeftClusterWidth);
    auto rightCluster = rail.removeFromRight(railRightClusterWidth);
    railDividerX = { leftCluster.getRight() + 10, rightCluster.getX() - 10 };

    // Input trim: caption, knob with its value underneath, peak bar. The output cluster on the
    // right is the same three rows mirrored, so the rail reads symmetrically.
    auto inputCell = leftCluster.removeFromLeft(88);
    inputLabel.setBounds(inputCell.removeFromTop(12));
    inputCell.removeFromTop(2);
    inputMeter.setBounds(inputCell.removeFromBottom(4).reduced(16, 0));
    inputCell.removeFromBottom(4);
    inputGain.setBounds(inputCell);

    leftCluster.removeFromLeft(20);

    /* The switch column is reserved from the *right* of the cluster before the selectors take
       their share, so it cannot be pushed past the edge and cannot be rebuilt from an empty
       rectangle. Taken from the left like everything else, an exhausted cluster left it zero-width
       and `withSizeKeepingCentre` then grew it back around the boundary, straddling the selector
       beside it -- the switches were drawn on top of the Performance combo. Reserving it first
       makes that arithmetically impossible rather than merely unlikely. */
    auto switchCell = leftCluster.removeFromRight(94).withSizeKeepingCentre(94, 64);
    leftCluster.removeFromRight(18);
    bypass.setBounds(switchCell.removeFromTop(28));
    switchCell.removeFromTop(8);
    proMode.setBounds(switchCell.removeFromTop(28));

    // Whatever remains is shared between the two selectors, so a cluster narrower than its content
    // shrinks them evenly instead of starving whichever one is laid out last.
    constexpr int selectorGap = 14;
    const auto selectorWidth = std::max(0, (leftCluster.getWidth() - selectorGap) / 2);
    auto engineCell = leftCluster.removeFromLeft(selectorWidth).withSizeKeepingCentre(selectorWidth, 44);
    engineCaption.setBounds(engineCell.removeFromTop(12));
    engineCell.removeFromTop(4);
    engineModeSelector.setBounds(engineCell.removeFromTop(28));

    leftCluster.removeFromLeft(selectorGap);
    auto performanceCell = leftCluster.removeFromLeft(selectorWidth).withSizeKeepingCentre(selectorWidth, 44);
    performanceCaption.setBounds(performanceCell.removeFromTop(12));
    performanceCell.removeFromTop(4);
    performanceSelector.setBounds(performanceCell.removeFromTop(28));

    auto outputCell = rightCluster.removeFromRight(88);
    outputLabel.setBounds(outputCell.removeFromTop(12));
    outputCell.removeFromTop(2);
    outputMeter.setBounds(outputCell.removeFromBottom(4).reduced(16, 0));
    outputCell.removeFromBottom(4);
    outputGain.setBounds(outputCell);

    // Presets sit in the middle: caption, a row of actions, then the name between arrows. The
    // inset keeps the arrows clear of the cluster dividers once the window is at its narrowest.
    rail = rail.reduced(16, 0);
    auto centre = rail.withSizeKeepingCentre(std::min(rail.getWidth(), 380), 78);
    presetCaption.setBounds(centre.removeFromTop(12));
    centre.removeFromTop(6);

    constexpr int actionWidth = 26;
    constexpr int actionGap = 10;
    constexpr int actionCount = 4;
    auto actions = centre.removeFromTop(24);
    auto actionX = actions.getCentreX()
                 - (actionWidth * actionCount + actionGap * (actionCount - 1)) / 2;
    for (auto* action : { &saveProject, &openProject, &presetBrowse, &resetVoicing })
    {
        action->setBounds(actionX, actions.getY(), actionWidth, actions.getHeight());
        actionX += actionWidth + actionGap;
    }

    centre.removeFromTop(6);
    auto row = centre.removeFromTop(30);
    presetPrevious.setBounds(row.removeFromLeft(26).reduced(0, 3));
    presetNext.setBounds(row.removeFromRight(26).reduced(0, 3));
    presetName.setBounds(row.reduced(8, 4));
}

void TubeForgeAudioProcessorEditor::timerCallback()
{
    // Engine work that has to happen whether or not its page is showing: recompiling the
    // physical circuit feeds the audio path, and the assistant refresh drains a queue the
    // audio thread writes into. Neither can be gated on page visibility.
    processor.refreshPhysicalCircuit();
    processor.refreshNonRealtimeDiagnostics();
    processor.refreshAssistant();
    // Applies a match the moment one finishes, when Auto Match is on. Beside the assistant
    // refresh for the same reason: it is engine work that has to happen whether or not the Song
    // Match page is the one being looked at.
    processor.refreshAutoMatch();
    // The tuner's queue is written by the audio thread and has to be drained whether or not
    // anyone is looking at it, or it fills and the reading goes stale the moment it is opened.
    processor.updateTuner();

    const auto& meters = processor.meterState();
    inputMeter.setLevel(std::max(meters.inputPeak(0), meters.inputPeak(1)));
    outputMeter.setLevel(std::max(meters.outputPeak(0), meters.outputPeak(1)));
    signalChain.setEngineMode(static_cast<int>(std::lround(
        processor.getParameters().getRawParameterValue("engineMode")->load(std::memory_order_relaxed))));

    /* Veil the tone-editing pages while a song match runs.

       Pushed to all three every tick rather than only to the visible one: the veil has to be
       correct the instant a page is opened, and a page that was hidden when the match started
       would otherwise appear un-veiled until the following tick. Setting it is a no-op when
       nothing has changed, so this costs a comparison.

       Amplifier, Pedals and Tone Shaping specifically -- these are the pages whose settings
       applying a candidate overwrites. The cabinet and the tuner are left alone: a match does not
       touch the loaded impulse responses, and tuning up while one renders is entirely reasonable. */
    const auto matching = processor.songMatchInProgress();
    for (const auto index : { std::size_t { 0 }, std::size_t { 1 }, std::size_t { 2 } })
        pages[index]->setSongMatchVeil(matching,
            "Applying one of the matched rigs will replace the drive, EQ and pedal settings on "
            "these pages. You can still change them -- nothing here affects the match itself -- "
            "but anything you dial in now is likely to be overwritten.");

    refreshAutoMatchMarking();
    pollAutoMatchWarning();

    // Only the page the user is actually looking at pulls state into its views.
    pages[static_cast<std::size_t>(activeModule)]->refresh();

    if (! diagnosticsText.isVisible()) return;
    const auto diagnostics = processor.diagnosticsSnapshot();
    const auto diagnosticSummary =
        "DSP " + juce::String(diagnostics.cpuLoadPercent, 1) + "%   |   callback "
        + juce::String(diagnostics.callbackMilliseconds, 3) + " ms   |   peak "
        + juce::String(diagnostics.maximumCallbackMilliseconds, 3) + " ms   |   latency "
        + juce::String(diagnostics.currentGraphLatencySamples) + " smp   |   dropouts "
        + juce::String(diagnostics.dropoutCount) + "   |   model " + statusName(diagnostics.modelLoadStatus)
        + "   |   IR " + statusName(diagnostics.irLoadStatus);
    diagnosticsText.setText(diagnosticSummary, juce::dontSendNotification);
    diagnosticsText.setTooltip(
        diagnosticSummary + "\nDeadline " + juce::String(diagnostics.deadlineMilliseconds, 3)
        + " ms; memory " + juce::String(static_cast<double>(diagnostics.workingSetBytes) / (1024.0 * 1024.0), 1)
        + " MiB; background failures " + juce::String(diagnostics.backgroundJobFailureCount));
}

void TubeForgeAudioProcessorEditor::refreshAutoMatchMarking()
{
    const auto state = processor.autoMatchState();
    const auto holding = state == tf::automatch::State::holding
                      || state == tf::automatch::State::overridden;
    const auto released = processor.autoMatchReleasedCount();
    auto badge = juce::String();
    if (holding)
    {
        badge = "AUTO MATCH";
        if (processor.autoMatchTracking()) badge += "   /   TRACKING";
        if (released > 0) badge += "   /   " + juce::String(released) + " YOURS";
    }

    // Amplifier, Pedals and Tone Shaping: the pages a matched rig actually writes, and the same
    // three the song-match veil covers for the same reason.
    for (const auto index : { std::size_t { 0 }, std::size_t { 1 }, std::size_t { 2 } })
        pages[index]->setAutoMatchBadge(badge);

    /* A control the host moved is handed back without a dialog -- see the guard -- so this is
       the only place the user finds out it happened. Reported once, as a message rather than as
       a question, because there is nothing to decide: the write has already landed. */
    if (const auto external = processor.takeAutoMatchExternalReleases(); ! external.isEmpty())
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon,
            "Auto Match handed a control back",
            external.joinIntoString(", ") + (external.size() == 1 ? " was" : " were")
            + " changed from outside this window -- host automation, a preset or a foot "
              "controller. Auto Match has stopped holding "
            + juce::String(external.size() == 1 ? "it" : "them") + " so the two do not fight.");
}

void TubeForgeAudioProcessorEditor::pollAutoMatchWarning()
{
    const auto owned = processor.takeAutoMatchWarning();
    if (owned < 0) return;
    const auto mask = tf::automatch::bit(static_cast<std::size_t>(owned));
    // Already asked about, or the user has said they have heard enough. Either way the control
    // is still theirs to move -- what is suppressed is the question, not the change.
    const auto suppressed = processor.autoMatchWarningsSuppressed();
    if (autoMatchDialogOpen || suppressed || (autoMatchWarnedMask & mask) != 0)
    {
        if (suppressed || (autoMatchWarnedMask & mask) != 0)
            processor.releaseAutoMatchParameter(
                tf::automatch::owned[static_cast<std::size_t>(owned)].id);
        return;
    }
    autoMatchWarnedMask |= mask;
    autoMatchDialogOpen = true;

    const auto id = tf::automatch::owned[static_cast<std::size_t>(owned)].id;
    tf::ui::AutoMatchDialog::show(this, processor.autoMatchParameterName(owned),
        processor.autoMatchValueText(owned), processor.autoMatchSourceName(),
        [safeThis = juce::Component::SafePointer<TubeForgeAudioProcessorEditor>(this), id]
        (tf::ui::AutoMatchDialog::Answer answer, bool suppress)
        {
            if (safeThis == nullptr) return;
            safeThis->autoMatchDialogOpen = false;
            if (suppress) safeThis->processor.setAutoMatchWarningsSuppressed(true);
            switch (answer)
            {
                case tf::ui::AutoMatchDialog::Answer::keepMatched:
                    safeThis->processor.restoreAutoMatchParameter(id);
                    break;
                case tf::ui::AutoMatchDialog::Answer::changeAnyway:
                    safeThis->processor.releaseAutoMatchParameter(id);
                    break;
                case tf::ui::AutoMatchDialog::Answer::turnOff:
                    safeThis->processor.setAutoMatchEnabled(false);
                    break;
            }
        });
}

void TubeForgeAudioProcessorEditor::chooseProjectToSave()
{
    fileChooser = std::make_unique<juce::FileChooser>("Save TubeForge project", juce::File {}, "*.tforge");
    fileChooser->launchAsync(juce::FileBrowserComponent::saveMode
                                 | juce::FileBrowserComponent::canSelectFiles
                                 | juce::FileBrowserComponent::warnAboutOverwriting,
        [safeThis = juce::Component::SafePointer<TubeForgeAudioProcessorEditor>(this)](const juce::FileChooser& chooser)
        {
            if (safeThis == nullptr || chooser.getResult() == juce::File {}) return;
            const auto result = safeThis->processor.saveProject(
                chooser.getResult().withFileExtension("tforge"));
            if (result.failed())
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                       "Save failed", result.getErrorMessage());
        });
}

void TubeForgeAudioProcessorEditor::chooseProjectToOpen()
{
    fileChooser = std::make_unique<juce::FileChooser>("Open TubeForge project", juce::File {}, "*.tforge");
    fileChooser->launchAsync(juce::FileBrowserComponent::openMode
                                 | juce::FileBrowserComponent::canSelectFiles,
        [safeThis = juce::Component::SafePointer<TubeForgeAudioProcessorEditor>(this)](const juce::FileChooser& chooser)
        {
            if (safeThis == nullptr || chooser.getResult() == juce::File {}) return;
            const auto result = safeThis->processor.loadProject(chooser.getResult());
            if (result.failed())
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                       "Open failed", result.getErrorMessage());
            else
                safeThis->presetName.setText(chooser.getResult().getFileNameWithoutExtension(),
                                             juce::dontSendNotification);
        });
}
