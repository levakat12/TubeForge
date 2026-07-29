#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "TubeForgeTheme.h"

#include "ui/AmplifierPage.h"
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
/// The gear browser and the page header both read from this table, so a module can never show
/// one name in the sidebar and another above its controls.
struct ModuleDescriptor
{
    const char* name;
    const char* tag;
    const char* blurb;
    int group;
};

constexpr std::array<ModuleDescriptor, TubeForgeAudioProcessorEditor::moduleCount> moduleTable { {
    { "Amplifier", "Core tone",
      "Set the sound here: drive, EQ and level. Everything else in TubeForge refines what you dial in on this page.", 0 },
    { "Tone Shaping", "Fine control",
      "Per-stage gain, filtering, feel, power-section behaviour and the noise gate. Reach for these once the amp is close.", 0 },
    { "Neural Capture", "Amp models",
      "Play through a captured amp. Load a model folder exported by the capture wizard, then choose what you monitor.", 1 },
    { "Tone Assistant", "Suggestions",
      "Suggests bounded changes based on what it hears. Every suggestion is previewed first; nothing is applied without you.", 1 },
    { "Profile Library", "Rigs & sharing",
      "Browse, load and share .ntone rig profiles. Imports are validated before anything can reach the audio engine.", 1 },
    { "Circuit", "Build the amp",
      "Choose real components: tubes, power topology, tone stack. Changes compile off the audio thread and crossfade in.", 2 },
    { "Tone Analyzer", "Measure a clip",
      "Analyse an isolated guitar or bass clip offline and read back its tonal fingerprint and how sure the analysis is.", 2 },
    { "Song Match", "Rebuild a tone",
      "Import a song you own, pick the part you want, and get editable rigs that get close to it.", 2 } } };

constexpr std::array groupNames { "Play", "Tools", "Pro tools" };

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

    tf::ui::configureLabel(title, "TUBEFORGE", 21.0f, true, theme::textPrimary);
    tf::ui::configureLabel(productTagline, "NEURAL AMPLIFIER STUDIO", 8.5f, true, theme::textTertiary);
    tf::ui::configureFieldCaption(presetCaption, "Current rig");
    tf::ui::configureLabel(presetName, "Default Rig", 15.0f, true, theme::textPrimary);
    tf::ui::configureLabel(mode, processor.modeName(), 10.0f, true, theme::textSecondary);
    tf::ui::configureLabel(deviceStatus, processor.deviceStatusText(), 9.0f, false, theme::textTertiary);
    tf::ui::configureLabel(diagnosticsText, "Diagnostics waiting for audio...", 10.0f, false,
                           theme::textTertiary);
    tf::ui::configureLabel(moduleTitle, moduleTable[0].name, 16.0f, true, theme::textPrimary);
    tf::ui::configureLabel(moduleSubtitle, moduleTable[0].blurb, 11.5f, false, theme::textSecondary);
    moduleSubtitle.setJustificationType(juce::Justification::topLeft);

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
    engineModeSelector.setTooltip("Which engine renders the amp: the traditional model, a loaded "
                                  "neural capture, or the physical circuit solver.");

    buildPages();
    buildGearBrowser();

    for (auto* component : std::initializer_list<juce::Component*> {
             &title, &productTagline, &presetCaption, &presetName, &presetPrevious, &presetNext,
             &presetBrowse, &diagnosticsText, &inputLabel, &outputLabel, &browserPanel, &pageHost,
             &signalChain, &studioMeter, &inputGain, &outputGain, &bypass, &proMode,
             &engineModeSelector, &audioSettings, &openProject, &saveProject })
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
    setResizeLimits(1020, 700, 1700, 1160);
    setSize(1260, 840);
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
    pages[2] = std::make_unique<NeuralCapturePage>(processor);
    pages[3] = std::make_unique<ToneAssistantPage>(processor);
    auto profileLibrary = std::make_unique<ProfileLibraryPage>(processor);
    library = profileLibrary.get();
    pages[libraryModule] = std::move(profileLibrary);
    pages[5] = std::make_unique<CircuitPage>(processor);
    pages[6] = std::make_unique<ToneAnalyzerPage>(processor);
    pages[7] = std::make_unique<SongMatchPage>(processor);

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
    pageHost.addAndMakeVisible(moduleTitle);
    pageHost.addAndMakeVisible(moduleSubtitle);
}

void TubeForgeAudioProcessorEditor::buildGearBrowser()
{
    tf::ui::configureLabel(browserTitle, "GEAR", 10.0f, true, theme::accent);
    for (std::size_t group = 0; group < browserGroupLabels.size(); ++group)
    {
        tf::ui::configureFieldCaption(browserGroupLabels[group], groupNames[group]);
        browserPanel.addAndMakeVisible(browserGroupLabels[group]);
    }
    browserPanel.addAndMakeVisible(browserTitle);
    browserPanel.addAndMakeVisible(mode);
    browserPanel.addAndMakeVisible(deviceStatus);
    for (int index = 0; index < moduleCount; ++index)
    {
        const auto& descriptor = moduleTable[static_cast<std::size_t>(index)];
        auto item = std::make_unique<tf::ui::GearBrowserItem>(descriptor.name, descriptor.tag);
        item->setTooltip(descriptor.blurb);
        item->onClick = [this, index] { setActiveModule(index); };
        browserPanel.addAndMakeVisible(*item);
        gearItems.push_back(std::move(item));
    }
}

void TubeForgeAudioProcessorEditor::setActiveModule(int index)
{
    activeModule = std::clamp(index, 0, moduleCount - 1);
    for (int page = 0; page < moduleCount; ++page)
    {
        pages[static_cast<std::size_t>(page)]->setVisible(page == activeModule);
        gearItems[static_cast<std::size_t>(page)]->setToggleState(page == activeModule,
                                                                  juce::dontSendNotification);
    }
    const auto& descriptor = moduleTable[static_cast<std::size_t>(activeModule)];
    moduleTitle.setText(descriptor.name, juce::dontSendNotification);
    moduleSubtitle.setText(descriptor.blurb, juce::dontSendNotification);
    // Pull state in now rather than leaving the page blank until the next tick.
    pages[static_cast<std::size_t>(activeModule)]->refresh();
    repaint();
}

void TubeForgeAudioProcessorEditor::applyProModeVisibility()
{
    const auto pro = proMode.getToggleState();
    for (int index = firstProModule; index < moduleCount; ++index)
        gearItems[static_cast<std::size_t>(index)]->setVisible(pro);
    browserGroupLabels[2].setVisible(pro);
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

    auto shell = getLocalBounds().toFloat().reduced(5.0f);
    graphics.setColour(theme::shell);
    graphics.fillRoundedRectangle(shell, 11.0f);
    graphics.setColour(juce::Colours::white.withAlpha(0.05f));
    graphics.drawRoundedRectangle(shell.reduced(0.5f), 11.0f, 1.0f);

    theme::glass(graphics, headerBounds.toFloat(), 9.0f, true);
    // The brand mark: a lit bar rather than a logo file, so it scales with the header.
    graphics.setColour(theme::accent);
    graphics.fillRoundedRectangle(juce::Rectangle<float>(3.0f, 26.0f).withCentre(
        { static_cast<float>(headerBounds.getX()) + 14.0f,
          static_cast<float>(headerBounds.getCentreY()) }), 1.5f);

    theme::glass(graphics, browserPanel.getBounds().toFloat(), 10.0f);
    theme::glass(graphics, pageHost.getBounds().toFloat(), 10.0f);
    theme::glass(graphics, meterRailBounds.toFloat(), 10.0f);

    if (diagnosticsText.isVisible())
        theme::well(graphics, diagnosticsText.getBounds().toFloat().expanded(8.0f, 3.0f), 5.0f);
}

void TubeForgeAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced(14);

    headerBounds = area.removeFromTop(60);
    auto header = headerBounds.reduced(10, 0);
    auto brand = header.removeFromLeft(160).withTrimmedLeft(12);
    title.setBounds(brand.removeFromTop(34).withTrimmedTop(6));
    productTagline.setBounds(brand.withTrimmedTop(-4));

    audioSettings.setBounds(header.removeFromRight(66).reduced(4, 15));
    saveProject.setBounds(header.removeFromRight(62).reduced(4, 15));
    openProject.setBounds(header.removeFromRight(62).reduced(4, 15));
    bypass.setBounds(header.removeFromRight(98).reduced(4, 14));
    proMode.setBounds(header.removeFromRight(74).reduced(4, 14));
    engineModeSelector.setBounds(header.removeFromRight(164).reduced(4, 15));

    auto preset = header.removeFromLeft(std::min(360, header.getWidth())).reduced(6, 10);
    presetPrevious.setBounds(preset.removeFromLeft(28).reduced(0, 4));
    preset.removeFromLeft(4);
    presetBrowse.setBounds(preset.removeFromRight(62).reduced(0, 4));
    presetNext.setBounds(preset.removeFromRight(32).withTrimmedRight(4).reduced(0, 4));
    presetCaption.setBounds(preset.removeFromTop(14).withTrimmedLeft(8));
    presetName.setBounds(preset.withTrimmedLeft(8));

    area.removeFromTop(10);
    if (diagnosticsText.isVisible())
    {
        diagnosticsText.setBounds(area.removeFromBottom(26).reduced(10, 3));
        area.removeFromBottom(8);
    }
    signalChain.setBounds(area.removeFromBottom(78));
    area.removeFromBottom(10);

    browserPanel.setBounds(area.removeFromLeft(198));
    area.removeFromLeft(10);
    meterRailBounds = area.removeFromRight(116);
    area.removeFromRight(10);
    pageHost.setBounds(area);

    auto browserInside = browserPanel.getLocalBounds().reduced(11, 12);
    browserTitle.setBounds(browserInside.removeFromTop(16));
    browserInside.removeFromTop(6);
    auto browserStatus = browserInside.removeFromBottom(38);
    mode.setBounds(browserStatus.removeFromTop(16));
    deviceStatus.setBounds(browserStatus);
    browserInside.removeFromBottom(8);
    int lastGroup = -1;
    for (int index = 0; index < moduleCount; ++index)
    {
        auto& item = *gearItems[static_cast<std::size_t>(index)];
        if (! item.isVisible()) continue;
        const auto group = moduleTable[static_cast<std::size_t>(index)].group;
        if (group != lastGroup)
        {
            if (lastGroup >= 0) browserInside.removeFromTop(6);
            browserGroupLabels[static_cast<std::size_t>(group)].setBounds(
                browserInside.removeFromTop(16).withTrimmedLeft(4));
            browserInside.removeFromTop(2);
            lastGroup = group;
        }
        item.setBounds(browserInside.removeFromTop(40).reduced(0, 1));
    }

    auto meter = meterRailBounds.reduced(8, 12);
    inputLabel.setBounds(meter.removeFromTop(14));
    inputGain.setBounds(meter.removeFromTop(86).reduced(6, 0));
    meter.removeFromTop(8);
    outputLabel.setBounds(meter.removeFromTop(14));
    outputGain.setBounds(meter.removeFromTop(86).reduced(6, 0));
    meter.removeFromTop(10);
    studioMeter.setBounds(meter);

    auto host = pageHost.getLocalBounds().reduced(16, 13);
    moduleTitle.setBounds(host.removeFromTop(21));
    moduleSubtitle.setBounds(host.removeFromTop(30));
    host.removeFromTop(9);
    for (auto& page : pages)
        page->setBounds(host);
}

void TubeForgeAudioProcessorEditor::timerCallback()
{
    // Engine work that has to happen whether or not its page is showing: recompiling the
    // physical circuit feeds the audio path, and the assistant refresh drains a queue the
    // audio thread writes into. Neither can be gated on page visibility.
    processor.refreshPhysicalCircuit();
    processor.refreshNonRealtimeDiagnostics();
    processor.refreshAssistant();

    const auto& meters = processor.meterState();
    studioMeter.setLevels(
        std::clamp(std::max(meters.inputPeak(0), meters.inputPeak(1)), 0.0f, 1.0f),
        std::clamp(std::max(meters.outputPeak(0), meters.outputPeak(1)), 0.0f, 1.0f));
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
