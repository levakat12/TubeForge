#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "TubeForgeTheme.h"

#include "ui/AmplifierPage.h"
#include "ui/CabinetPage.h"
#include "ui/TunerPage.h"
#include "ui/CircuitPage.h"
#include "ui/NeuralCapturePage.h"
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
/// Both rail clusters are this wide so the preset block between them is centred in the window.
constexpr int railClusterWidth = 372;

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
    tf::ui::configureLabel(productTagline, "NEURAL AMPLIFIER STUDIO", 8.0f, true, theme::textTertiary);

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

    tf::ui::populateFromParameter(engineModeSelector, processor.getParameters(), "engineMode");
    bypass.setColour(juce::ToggleButton::tickColourId, theme::bad);
    bypass.setTooltip("Pass the dry signal straight through, bypassing the whole chain.");
    proMode.setTooltip("Show the engineering modules and the live diagnostics readout.");
    presetPrevious.setTooltip("Previous profile in the library");
    presetNext.setTooltip("Next profile in the library");
    presetBrowse.setTooltip("Open the profile library");
    saveProject.setTooltip("Save the whole session as a .tforge project");
    openProject.setTooltip("Open a .tforge project");
    audioSettings.setTooltip("Audio device settings");
    engineModeSelector.setTooltip("Which engine renders the amp: the traditional model, a loaded "
                                  "neural capture, or the physical circuit solver.");

    buildPages();
    buildNavigation();

    for (auto* component : std::initializer_list<juce::Component*> {
             &title, &productTagline, &presetCaption, &presetName, &presetPrevious, &presetNext,
             &presetBrowse, &engineCaption, &inputLabel, &outputLabel, &inputMeter, &outputMeter,
             &pageHost, &mode, &deviceStatus, &signalChain, &diagnosticsText, &inputGain,
             &outputGain, &bypass, &proMode, &engineModeSelector, &audioSettings, &openProject,
             &saveProject })
        addAndMakeVisible(*component);

    inputAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.getParameters(), "input", inputGain);
    outputAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.getParameters(), "output", outputGain);
    bypassAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        processor.getParameters(), "bypass", bypass);
    engineModeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        processor.getParameters(), "engineMode", engineModeSelector);

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

    setActiveModule(0);
    applyProModeVisibility();

    setResizable(true, true);
    setResizeLimits(1040, 700, 1800, 1200);
    setSize(1240, 820);
    startTimerHz(20);
}

TubeForgeAudioProcessorEditor::~TubeForgeAudioProcessorEditor()
{
    setLookAndFeel(nullptr);
}

void TubeForgeAudioProcessorEditor::buildPages()
{
    // Order must match moduleTable.
    pages[0] = std::make_unique<AmplifierPage>(processor);
    pages[1] = std::make_unique<ToneShapingPage>(processor);
    pages[2] = std::make_unique<TunerPage>(processor);
    pages[3] = std::make_unique<CabinetPage>(processor);
    pages[4] = std::make_unique<NeuralCapturePage>(processor);
    pages[5] = std::make_unique<ToneAssistantPage>(processor);
    auto profileLibrary = std::make_unique<ProfileLibraryPage>(processor);
    library = profileLibrary.get();
    pages[libraryModule] = std::move(profileLibrary);
    pages[7] = std::make_unique<CircuitPage>(processor);
    pages[8] = std::make_unique<ToneAnalyzerPage>(processor);
    pages[9] = std::make_unique<SongMatchPage>(processor);

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
    auto leftCluster = rail.removeFromLeft(railClusterWidth);
    auto rightCluster = rail.removeFromRight(railClusterWidth);
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
    auto engineCell = leftCluster.removeFromLeft(152).withSizeKeepingCentre(152, 44);
    engineCaption.setBounds(engineCell.removeFromTop(12));
    engineCell.removeFromTop(4);
    engineModeSelector.setBounds(engineCell.removeFromTop(28));

    leftCluster.removeFromLeft(18);
    auto switchCell = leftCluster.removeFromLeft(94).withSizeKeepingCentre(94, 64);
    bypass.setBounds(switchCell.removeFromTop(28));
    switchCell.removeFromTop(8);
    proMode.setBounds(switchCell.removeFromTop(28));

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
    auto actions = centre.removeFromTop(24);
    auto actionX = actions.getCentreX() - (actionWidth * 3 + actionGap * 2) / 2;
    for (auto* action : { &saveProject, &openProject, &presetBrowse })
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
    // The tuner's queue is written by the audio thread and has to be drained whether or not
    // anyone is looking at it, or it fills and the reading goes stale the moment it is opened.
    processor.updateTuner();

    const auto& meters = processor.meterState();
    inputMeter.setLevel(std::max(meters.inputPeak(0), meters.inputPeak(1)));
    outputMeter.setLevel(std::max(meters.outputPeak(0), meters.outputPeak(1)));
    signalChain.setEngineMode(static_cast<int>(std::lround(
        processor.getParameters().getRawParameterValue("engineMode")->load(std::memory_order_relaxed))));

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
