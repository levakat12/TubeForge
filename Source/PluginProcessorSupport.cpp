// Construction, teardown, and the forwarders to StudioServices and the assistant.

#include "PluginProcessorInternal.h"

TubeForgeAudioProcessor::~TubeForgeAudioProcessor()
{
    // The tone-analysis and reconstruction workers now belong to StudioServices, whose own
    // destructor stops them. What remains here is the two that stage into the real-time
    // engine and so cannot move out of the processor.
    if (neuralLoader.joinable()) neuralLoader.request_stop();
    if (circuitCompiler.joinable()) circuitCompiler.request_stop();
    // Before the value tree state goes: a parameter outliving a listener that points at a
    // half-destroyed processor is the shape of crash that only shows up in a host.
    detachAutoMatchGuard();
}

TubeForgeAudioProcessor::TubeForgeAudioProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      parameterState(*this, nullptr, "TubeForgeParameters", createParameterLayout()),
      engine(runtimeParameters, meters),
      logger(logPath(), &diagnostics),
      cabinets(cabinetLibraryPreferencesPath()),
      captures(captureLibraryPath()),
      studio([this](const nts::amp::AmpParameters& rig, bool isolateChain)
             { applyRecoveredRig(rig, isolateChain); }),
      tonePackageLibrary(tonePackageLibraryPath())
{
    // Resolved once: these stay valid for the lifetime of the value tree state, and
    // looking them up by string on the audio thread is pure waste. A null here means an
    // id in parameterIdList has no matching parameter in the layout, which would
    // otherwise degrade silently to a constant zero at every read site.
    for (std::size_t index = 0; index < parameterIdList.size(); ++index)
    {
        parameterPointers[index] = parameterState.getRawParameterValue(parameterIdList[index]);
        jassert(parameterPointers[index] != nullptr);
    }

    // Resolves Source/AutoMatch.h against the real parameters and starts watching them. Done
    // here rather than when an editor opens, because a host writing an automation lane has to
    // release the control it wrote whether or not anyone has the window open.
    attachAutoMatchGuard();
    loadAutoMatchPreferences();

    for (int instrument = 0; instrument < 2; ++instrument)
        for (std::size_t topology = 0; topology < nts::amp::topologyCount; ++topology)
            factoryAmpParameters[factoryAmpIndex(instrument, static_cast<int>(topology))] =
                nts::amp::makeOriginalPreset(
                    static_cast<nts::amp::Topology>(topology),
                    instrument == 1 ? nts::amp::Instrument::bass : nts::amp::Instrument::guitar)
                    .parameters;
    logger.log({ std::chrono::system_clock::now(), nts::diagnostics::LogSeverity::info,
                 "runtime", "processor-created", "TubeForge processor created", modeName().toStdString() });
    const auto preferencesFile = assistantPreferencesFile();
    if (preferencesFile.existsAsFile())
    {
        std::string preferenceError;
        if (const auto restored = nts::assistant::deserializePreferences(
                preferencesFile.loadFileAsString().toStdString(), preferenceError))
            assistantPreferences = *restored;
    }
    std::string packageError;
    if (! tonePackageLibrary.refresh(packageError))
        logger.log({ std::chrono::system_clock::now(), nts::diagnostics::LogSeverity::warning,
                     "ecosystem", "profile-library", "Profile library unavailable", packageError });
}

// Tone analysis and song reconstruction live in StudioServices; these are pass-throughs so the
// editor keeps one façade to talk to.
void TubeForgeAudioProcessor::requestToneAnalysis(const juce::File& audioFile)
{ studio.requestToneAnalysis(audioFile); }

juce::String TubeForgeAudioProcessor::toneAnalysisStatusText() const
{ return studio.toneAnalysisStatusText(); }

juce::String TubeForgeAudioProcessor::toneNearestProfileText() const
{ return studio.toneNearestProfileText(); }

std::optional<nts::tone::ToneAnalysisResult> TubeForgeAudioProcessor::toneAnalysisSnapshot() const
{ return studio.toneAnalysisSnapshot(); }

std::size_t TubeForgeAudioProcessor::toneProfileCount() const
{ return studio.toneProfileCount(); }

void TubeForgeAudioProcessor::requestSongReconstruction(const juce::File& songFile,
                                                        nts::reconstruction::TargetInstrument target,
                                                        nts::reconstruction::StereoMode stereoMode)
{ studio.requestSongReconstruction(songFile, target, stereoMode); }

void TubeForgeAudioProcessor::cancelSongReconstruction()
{ studio.cancelSongReconstruction(); }

juce::String TubeForgeAudioProcessor::reconstructionStatusText() const
{ return studio.reconstructionStatusText(); }

bool TubeForgeAudioProcessor::requestReconstructionRegion(std::size_t index)
{ return studio.requestReconstructionRegion(index); }

bool TubeForgeAudioProcessor::applyReconstructionCandidate(std::size_t index, bool isolateChain)
{ return studio.applyReconstructionCandidate(index, isolateChain); }

juce::Result TubeForgeAudioProcessor::exportReconstruction(const juce::File& file) const
{ return studio.exportReconstruction(file); }

std::vector<nts::assistant::ParameterValue> TubeForgeAudioProcessor::assistantParameterValues() const
{
    std::vector<nts::assistant::ParameterValue> values;
    values.reserve(nts::assistant::parameterSchema().size());
    for (const auto& descriptor : nts::assistant::parameterSchema())
    {
        const auto id = std::string(descriptor.id);
        values.push_back({ id, valueOf(parameterState, id.c_str()) });
    }
    return values;
}

void TubeForgeAudioProcessor::applyAssistantParameterValues(
    std::span<const nts::assistant::ParameterValue> values)
{
    /* An accepted assistant action is the user's decision, arriving through a button rather than
       a knob -- so it must not raise the Auto Match dialog, and it must not be silently undone by
       Auto Match either. Suppressed on the way in, and every parameter it moves is handed back:
       from here on the analyzer stops holding those controls and the assistant's value stands. */
    const AutoWriteScope autoWrite(*this);
    for (const auto& value : values)
    {
        setParameterValue(parameterState, value.id.c_str(), value.value);
        releaseAutoMatchParameter(value.id);
    }
}

void TubeForgeAudioProcessor::refreshAssistant()
{
    nts::assistant::AudioSummaryFrame frame;
    bool receivedFrame {};
    while (assistantSummaryQueue.pop(frame))
    {
        assistantSummaryAccumulator.add(frame);
        receivedFrame = true;
    }
    const auto now = std::chrono::steady_clock::now();
    nts::assistant::Goal goal;
    nts::assistant::PreferenceProfile preferences;
    {
        const std::scoped_lock lock(assistantMutex);
        if (assistantActionSession.hasPreview()) return;
        goal = assistantGoal; preferences = assistantPreferences;
        if (! receivedFrame && goal == nts::assistant::Goal::diagnose) return;
        if (now - lastAssistantEvaluation < std::chrono::milliseconds(750)) return;
        lastAssistantEvaluation = now;
    }
    nts::assistant::AssistantInput input;
    input.signal = assistantSummaryAccumulator.observation();
    input.rig = currentAmpParameters(); input.parameters = assistantParameterValues(); input.goal = goal;
    input.tone = studio.toneAnalysisSnapshot();
    input.reference = studio.reconstructionReferenceTone();
    auto recommendations = assistantEngine.evaluate(input, preferences);
    const std::scoped_lock lock(assistantMutex);
    currentAssistantRecommendations = std::move(recommendations);
    if (currentAssistantRecommendations.empty())
        assistantStatus = input.signal.frames == 0
            ? "Waiting for live DI/output summary frames"
            : "No high-confidence technical problem detected";
    else
        assistantStatus = std::to_string(currentAssistantRecommendations.size())
            + " bounded suggestion(s); preview is required before acceptance";
}

void TubeForgeAudioProcessor::setAssistantGoal(nts::assistant::Goal goal)
{
    const std::scoped_lock lock(assistantMutex);
    assistantGoal = goal;
    lastAssistantEvaluation = {};
    assistantStatus = "Goal selected: " + std::string(nts::assistant::toString(goal));
}

std::vector<nts::assistant::Recommendation> TubeForgeAudioProcessor::assistantRecommendations() const
{
    const std::scoped_lock lock(assistantMutex);
    return currentAssistantRecommendations;
}

juce::String TubeForgeAudioProcessor::assistantStatusText() const
{
    const std::scoped_lock lock(assistantMutex);
    return juce::String::fromUTF8(assistantStatus.c_str());
}

bool TubeForgeAudioProcessor::previewAssistantRecommendation(std::size_t index)
{
    auto current = assistantParameterValues();
    std::vector<nts::assistant::ParameterValue> preview;
    {
        const std::scoped_lock lock(assistantMutex);
        if (index >= currentAssistantRecommendations.size())
        { assistantStatus = "No assistant suggestion selected"; return false; }
        std::string error;
        if (! assistantActionSession.preview(currentAssistantRecommendations[index], current, error))
        { assistantStatus = error; return false; }
        preview = assistantActionSession.previewState()->after;
        assistantStatus = "Previewing " + currentAssistantRecommendations[index].diagnosis
            + "; accept or reject to continue";
    }
    applyAssistantParameterValues(preview);
    return true;
}

bool TubeForgeAudioProcessor::acceptAssistantPreview()
{
    auto current = assistantParameterValues();
    {
        const std::scoped_lock lock(assistantMutex);
        std::string error;
        if (! assistantActionSession.accept(current, assistantPreferences, error))
        { assistantStatus = error; return false; }
        if (assistantPreferences.personalizationEnabled)
        {
            const auto lookup = [&current](std::string_view id, float fallback)
            {
                const auto found = std::find_if(current.begin(), current.end(), [id](const auto& item) { return item.id == id; });
                return found == current.end() ? fallback : found->value;
            };
            assistantPreferences.preferredInstrument = valueOf(parameterState, ParameterIds::instrument) >= 0.5f
                ? nts::tone::Instrument::bass : nts::tone::Instrument::guitar;
            assistantPreferences.preferredGain = std::clamp(lookup("gain", 5.0f) * 0.1f, 0.0f, 1.0f);
            assistantPreferences.preferredBrightness = std::clamp(
                (lookup("treble", 5.0f) + lookup("presence", 5.0f)) * 0.05f, 0.0f, 1.0f);
            assistantPreferences.preferredCleanBlend = std::clamp(lookup("cleanBlend", 50.0f) * 0.01f, 0.0f, 1.0f);
            if (valueOf(parameterState, ParameterIds::cabinet) >= 0.5f)
                assistantPreferences.frequentlyUsedCabinets.push_back("TubeForge internal cabinet");
        }
        assistantStatus = "Preview accepted; exact previous state is available through Undo";
        lastAssistantEvaluation = std::chrono::steady_clock::now();
    }
    applyAssistantParameterValues(current); saveAssistantPreferences(); return true;
}

bool TubeForgeAudioProcessor::rejectAssistantPreview()
{
    auto current = assistantParameterValues();
    {
        const std::scoped_lock lock(assistantMutex);
        std::string error;
        if (! assistantActionSession.reject(current, assistantPreferences, error))
        { assistantStatus = error; return false; }
        assistantStatus = "Preview rejected; exact previous parameter state restored";
        lastAssistantEvaluation = std::chrono::steady_clock::now();
    }
    applyAssistantParameterValues(current); saveAssistantPreferences(); return true;
}

bool TubeForgeAudioProcessor::undoAssistantChange()
{
    auto current = assistantParameterValues();
    {
        const std::scoped_lock lock(assistantMutex);
        std::string error;
        if (! assistantActionSession.undo(current, error))
        { assistantStatus = error; return false; }
        assistantStatus = "Assistant change undone exactly";
        lastAssistantEvaluation = std::chrono::steady_clock::now();
    }
    applyAssistantParameterValues(current); return true;
}

bool TubeForgeAudioProcessor::assistantPreviewActive() const
{
    const std::scoped_lock lock(assistantMutex);
    return assistantActionSession.hasPreview();
}

void TubeForgeAudioProcessor::saveAssistantPreferences() const
{
    nts::assistant::PreferenceProfile preferences;
    {
        const std::scoped_lock lock(assistantMutex);
        preferences = assistantPreferences;
    }
    const auto file = assistantPreferencesFile();
    if (file.getParentDirectory().createDirectory().wasOk())
        (void) file.replaceWithText(juce::String::fromUTF8(
            nts::assistant::serializePreferences(preferences).c_str()));
}

void TubeForgeAudioProcessor::setAssistantPersonalizationEnabled(bool enabled)
{
    {
        const std::scoped_lock lock(assistantMutex);
        assistantPreferences.personalizationEnabled = enabled;
        assistantStatus = enabled ? "Local personalization enabled" : "Personalization disabled";
        lastAssistantEvaluation = {};
    }
    saveAssistantPreferences();
}

bool TubeForgeAudioProcessor::assistantPersonalizationEnabled() const
{
    const std::scoped_lock lock(assistantMutex);
    return assistantPreferences.personalizationEnabled;
}

void TubeForgeAudioProcessor::clearAssistantPreferences()
{
    {
        const std::scoped_lock lock(assistantMutex);
        const auto enabled = assistantPreferences.personalizationEnabled;
        assistantPreferences = {}; assistantPreferences.personalizationEnabled = enabled;
        assistantActionSession.clearHistory();
        assistantStatus = "Local assistant preferences and undo history cleared";
        lastAssistantEvaluation = {};
    }
    const auto file = assistantPreferencesFile();
    if (file.existsAsFile()) (void) file.deleteFile();
}

std::optional<nts::reconstruction::ReconstructionResult>
TubeForgeAudioProcessor::reconstructionSnapshot() const
{ return studio.reconstructionSnapshot(); }

std::vector<nts::reconstruction::PlayableRegion>
TubeForgeAudioProcessor::reconstructionRegionsSnapshot() const
{ return studio.reconstructionRegionsSnapshot(); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new TubeForgeAudioProcessor();
}
