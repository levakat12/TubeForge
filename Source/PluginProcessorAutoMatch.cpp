// Auto Match: the analyzer holds the rig, and the guard that warns before the user fights it.
//
// Three separable pieces live here, and the ordering of this file follows them:
//
//   1. The snapshot -- what a matched rig actually put on every owned control, recorded at the
//      moment it was applied, because "keep the matched value" needs a value to put back and
//      `appliedRecoveredRig` holds engine parameters rather than host ones.
//   2. The guard -- two AudioProcessorParameter::Listener callbacks that decide whether a
//      parameter moved because a person moved it, because Auto Match moved it, or because the
//      host did. Both can arrive on the audio thread, so nothing here allocates, locks or
//      touches the interface.
//   3. The queries the editor asks: what is held, what was released, what to say about it.
//
// See docs/auto-match-plan.md for the design and Source/AutoMatch.h for the owned set.

#include "PluginProcessorInternal.h"

namespace
{
/// A control's value as the user reads it on the panel, for the warning dialog's text.
juce::String describeParameter(const juce::AudioProcessorParameter& parameter, float plainValue)
{
    if (const auto* ranged = dynamic_cast<const juce::RangedAudioParameter*>(&parameter))
        return ranged->getText(ranged->convertTo0to1(plainValue), 32) + ranged->getLabel();
    return juce::String(plainValue, 2);
}
} // namespace

void TubeForgeAudioProcessor::attachAutoMatchGuard()
{
    for (std::size_t owned = 0; owned < tf::automatch::ownedCount; ++owned)
    {
        autoOwnedParameterIndices[owned] = -1;
        const auto id = juce::String(std::string(tf::automatch::owned[owned].id));
        auto* parameter = parameterState.getParameter(id);
        // A null here means Source/AutoMatch.h names a parameter the layout never registered,
        // which would degrade silently into a control Auto Match writes about but never guards.
        jassert(parameter != nullptr);
        if (parameter == nullptr) continue;
        autoOwnedParameterIndices[owned] = parameter->getParameterIndex();
        parameter->addListener(this);
    }
}

void TubeForgeAudioProcessor::detachAutoMatchGuard()
{
    for (std::size_t owned = 0; owned < tf::automatch::ownedCount; ++owned)
    {
        const auto id = juce::String(std::string(tf::automatch::owned[owned].id));
        if (auto* parameter = parameterState.getParameter(id)) parameter->removeListener(this);
    }
}

int TubeForgeAudioProcessor::autoOwnedIndexOf(int parameterIndex) const noexcept
{
    // A linear scan of 47 ints, reached only when one of those 47 parameters actually moves.
    // A map would be faster in the abstract and would allocate, which is the one thing a
    // callback the host may make from the audio thread cannot do.
    for (std::size_t owned = 0; owned < autoOwnedParameterIndices.size(); ++owned)
        if (autoOwnedParameterIndices[owned] == parameterIndex) return static_cast<int>(owned);
    return -1;
}

bool TubeForgeAudioProcessor::autoMatchGuarding(int ownedIndex) const noexcept
{
    if (ownedIndex < 0) return false;
    const auto mask = tf::automatch::bit(static_cast<std::size_t>(ownedIndex));
    if ((autoHeldMask.load(std::memory_order_relaxed) & mask) == 0) return false;
    if ((autoReleasedMask.load(std::memory_order_relaxed) & mask) != 0) return false;
    return autoMatchEnabled();
}

void TubeForgeAudioProcessor::parameterGestureChanged(int parameterIndex, bool gestureIsStarting)
{
    // Auto Match writing its own rig. Every programmatic bulk write is inside an
    // AutoWriteScope for exactly this reason -- see the class comment on that scope.
    if (autoWriteDepth.load(std::memory_order_acquire) > 0) return;

    const auto owned = autoOwnedIndexOf(parameterIndex);
    if (owned < 0) return;
    const auto mask = tf::automatch::bit(static_cast<std::size_t>(owned));
    if (gestureIsStarting) autoGestureMask.fetch_or(mask, std::memory_order_acq_rel);
    else autoGestureMask.fetch_and(~mask, std::memory_order_acq_rel);

    if (! gestureIsStarting || ! autoMatchGuarding(owned)) return;
    // The instrument is not a control to be warned about; it is a control that invalidates the
    // match. `parameterValueChanged` handles it, because the new value is not known yet here.
    if (tf::automatch::owned[static_cast<std::size_t>(owned)].id == "instrument") return;
    // First request wins until the editor consumes it. A second control touched while a dialog
    // is already pending is dropped rather than queued: the user is being asked one question,
    // and stacking dialogs behind a knob drag is the failure this whole design avoids.
    auto expected = -1;
    (void) autoWarningRequest.compare_exchange_strong(expected, owned, std::memory_order_acq_rel);
}

void TubeForgeAudioProcessor::parameterValueChanged(int parameterIndex, float newValue)
{
    if (autoWriteDepth.load(std::memory_order_acquire) > 0) return;

    const auto owned = autoOwnedIndexOf(parameterIndex);
    if (! autoMatchGuarding(owned)) return;
    const auto mask = tf::automatch::bit(static_cast<std::size_t>(owned));

    /* Changing instrument does not override the match, it invalidates it.

       `searchableTopologies` offers a different set of voicings per instrument, so a guitar
       match carried onto a bass rig is holding values chosen from a shortlist bass would never
       have produced. Asking "keep the matched value?" about that is the wrong question -- there
       is no matched value for an instrument the search never ran on. So the hold drops and the
       match re-runs, and the rig sits armed until it finishes.

       Latched here and started from `handleAsyncUpdate`: a host can call this on the audio
       thread, where launching a worker is not allowed. Decoded from the normalised value rather
       than read back from the parameter, because the cached raw value is updated by a different
       listener and the order the two run in is not defined. */
    if (tf::automatch::owned[static_cast<std::size_t>(owned)].id == "instrument")
    {
        clearAutoMatchHold();
        autoMatchInstrumentRequest.store(newValue >= 0.5f ? 1 : 0, std::memory_order_release);
        triggerAsyncUpdate();
        return;
    }
    // A gesture is open, so a person is holding this control and `parameterGestureChanged`
    // has already asked for the dialog. Nothing to do here.
    if ((autoGestureMask.load(std::memory_order_acquire) & mask) != 0) return;

    /* No gesture behind the write: host automation, a MIDI program change, or an editor that
       writes a parameter without one. None of those is a person to raise a modal in front of --
       a dialog opened while a DAW plays back an automation lane is a hang, not a warning. The
       control is handed back silently and the interface says so afterwards. */
    autoReleasedMask.fetch_or(mask, std::memory_order_acq_rel);
    autoExternalReleaseMask.fetch_or(mask, std::memory_order_acq_rel);
}

void TubeForgeAudioProcessor::captureAutoMatchSnapshot(bool isolatedChain)
{
    for (std::size_t owned = 0; owned < tf::automatch::ownedCount; ++owned)
        autoMatchValues[owned] = valueOf(parameterState,
                                         std::string(tf::automatch::owned[owned].id).c_str());
    autoMatchIsolatedChain = isolatedChain;
    {
        const std::scoped_lock lock(autoMatchMutex);
        autoMatchSource = studio.reconstructionSourceName().toStdString();
    }
    // Every release is cleared: this is a new rig, and a control the user took back from the
    // previous match has no meaning against this one.
    autoReleasedMask.store(0, std::memory_order_release);
    autoExternalReleaseMask.store(0, std::memory_order_release);
    autoWarningRequest.store(-1, std::memory_order_release);
    autoHeldMask.store(tf::automatch::maskFor(isolatedChain), std::memory_order_release);
}

void TubeForgeAudioProcessor::clearAutoMatchHold() noexcept
{
    autoHeldMask.store(0, std::memory_order_release);
    autoReleasedMask.store(0, std::memory_order_release);
    autoExternalReleaseMask.store(0, std::memory_order_release);
    autoWarningRequest.store(-1, std::memory_order_release);
    // The cabinet goes with the rig. What these paths invalidate is the *match*, and a matched
    // cabinet is part of one -- reporting it as still held against a rig that is no longer on the
    // amplifier would be guarding something that is not there.
    cabinetHold.store(CabinetHold::none, std::memory_order_release);
}

void TubeForgeAudioProcessor::applyAutoMatchRig(const nts::amp::AmpParameters& rig, bool isolateChain)
{
    /* What the user has taken back has to survive the re-run.

       `applyRecoveredRig` treats every apply as a new rig and clears the releases, which is right
       when the user pressed Apply -- they chose a different candidate -- and wrong here, where
       the search re-ran because a region changed and the user's overrides are still about the
       same tone. So they are lifted out, the rig is applied, and they are put back on top. */
    struct Override { std::string_view id; float value; };
    std::vector<Override> overrides;
    const auto released = autoReleasedMask.load(std::memory_order_acquire)
                        & autoHeldMask.load(std::memory_order_acquire);
    for (std::size_t owned = 0; owned < tf::automatch::ownedCount; ++owned)
        if ((released & tf::automatch::bit(owned)) != 0)
            overrides.push_back({ tf::automatch::owned[owned].id,
                                  valueOf(parameterState,
                                          std::string(tf::automatch::owned[owned].id).c_str()) });

    applyRecoveredRig(rig, isolateChain);

    {
        const AutoWriteScope scope(*this);
        for (const auto& value : overrides)
            setParameterValue(parameterState, std::string(value.id).c_str(), value.value);
    }
    std::uint64_t restored {};
    for (const auto& value : overrides)
        if (const auto owned = tf::automatch::indexOf(value.id); owned >= 0)
            restored |= tf::automatch::bit(static_cast<std::size_t>(owned));
    autoReleasedMask.store(restored, std::memory_order_release);
}

void TubeForgeAudioProcessor::refreshAutoMatch()
{
    updateAutoMatchTracking();

    const auto generation = studio.reconstructionGeneration();
    if (generation == autoMatchAppliedGeneration) return;
    // Latched whether or not it is applied, so switching Auto Match on after a match has already
    // finished does not then apply a result the user has been sitting with. The switch means
    // "hold what I match from now on", not "reach back and take over".
    autoMatchAppliedGeneration = generation;
    if (! autoMatchEnabled()) return;

    const auto result = studio.reconstructionSnapshot();
    if (! result || ! result->success || result->candidates.empty()) return;

    applyAutoMatchRig(result->candidates.front().rigPreset.parameters, autoMatchIsolatePreference);

    logger.log({ std::chrono::system_clock::now(), nts::diagnostics::LogSeverity::info,
                 "automatch", "rig-applied", "Auto Match applied a completed song match",
                 "candidate=" + result->candidates.front().rigPreset.name });
}

bool TubeForgeAudioProcessor::autoMatchTracking() const noexcept
{
    return parameterOf(Param::autoMatchTracking) >= 0.5f;
}

void TubeForgeAudioProcessor::setAutoMatchTracking(bool enabled)
{
    setParameterValue(parameterState, ParameterIds::autoMatchTracking, enabled ? 1.0f : 0.0f);
}

void TubeForgeAudioProcessor::updateAutoMatchTracking()
{
    if (! autoMatchEnabled() || ! autoMatchTracking()) return;
    if (autoHeldMask.load(std::memory_order_acquire) == 0) return;

    /* Rate limited, and the limit is the point rather than an optimisation.

       Every write here is a real change gesture, so a trim recomputed at the editor's 20 Hz tick
       would fill a host's automation lane with two hundred gestures a minute and would look, on
       the panel, like a control with a fault. Twice a second is fast enough to follow a guitar
       change and slow enough to read as deliberate. */
    const auto now = std::chrono::steady_clock::now();
    if (now - lastAutoMatchTracking < std::chrono::milliseconds(500)) return;
    lastAutoMatchTracking = now;

    /* The same measurement the assistant runs on, and it is measured **before** the input trim
       is applied -- see the call to `measureBuffer` at the top of `processBlock`. That is what
       makes this open-loop: the correction is a function of the incoming instrument alone, so
       raising the trim cannot raise the measurement that asked for it. A closed loop here would
       either run away or hunt, and neither is something a player should have to diagnose. */
    const auto signal = assistantSummaryAccumulator.observation();
    if (signal.frames < 16) return;

    const auto decibels = [](float linear)
    { return 20.0f * std::log10(std::max(linear, 1.0e-7f)); };
    // Nothing is being played. Tracking a silent input would drive the trim to its bound on the
    // noise floor and leave it there for the first note.
    if (signal.inputPeak < 3.0e-4f) return;

    // Six either side of what the match set, and never further: this follows an instrument
    // change, not a different rig. Beyond that bound the answer is to re-run the match.
    constexpr auto rangeDb = 6.0f;
    // A deadband wide enough that ordinary playing dynamics do not move the control, and narrow
    // enough that swapping to a hotter guitar does.
    constexpr auto deadbandDb = 0.75f;

    const AutoWriteScope scope(*this);

    if (! autoMatchReleased("input"))
        if (const auto matched = autoMatchValueOf("input"))
        {
            // Aims the incoming peak at -6 dBFS, which is the headroom the preamp stages expect
            // and what the calibrator assumes when it suggests a trim.
            const auto desired = -6.0f - decibels(signal.inputPeak);
            const auto target = std::clamp(desired, *matched - rangeDb, *matched + rangeDb);
            if (std::abs(target - valueOf(parameterState, ParameterIds::input)) > deadbandDb)
                setParameterValue(parameterState, ParameterIds::input, target);
        }

    if (! autoMatchReleased("gateThreshold"))
        if (const auto matched = autoMatchValueOf("gateThreshold"))
        {
            /* The gate sits after the trim, so the floor it sees is the measured floor plus
               whatever the trim is doing right now -- read live rather than from the snapshot,
               because the branch above may have just moved it. Six dB above the floor is the
               usual place to sit a gate: high enough to close on hum, low enough to leave the
               tail of a note alone. */
            const auto floorDb = decibels(signal.noiseFloorRms)
                               + valueOf(parameterState, ParameterIds::input);
            const auto target = std::clamp(floorDb + 6.0f, *matched - rangeDb, *matched + rangeDb);
            if (std::abs(target - valueOf(parameterState, ParameterIds::gateThreshold)) > deadbandDb)
                setParameterValue(parameterState, ParameterIds::gateThreshold, target);
        }
}

void TubeForgeAudioProcessor::loadAutoMatchPreferences()
{
    const auto file = autoMatchPreferencesFile();
    if (! file.existsAsFile()) return;
    juce::var parsed;
    if (juce::JSON::parse(file.loadFileAsString(), parsed).failed() || ! parsed.isObject()) return;
    autoMatchSuppressWarnings = static_cast<bool>(parsed.getProperty("suppressWarnings", false));
}

void TubeForgeAudioProcessor::setAutoMatchWarningsSuppressed(bool suppressed)
{
    if (autoMatchSuppressWarnings == suppressed) return;
    autoMatchSuppressWarnings = suppressed;
    auto* root = new juce::DynamicObject();
    root->setProperty("suppressWarnings", suppressed);
    const auto file = autoMatchPreferencesFile();
    if (file.getParentDirectory().createDirectory().wasOk())
        (void) file.replaceWithText(juce::JSON::toString(juce::var(root), true));
}

nts::state::AutoMatchState TubeForgeAudioProcessor::autoMatchProjectState() const
{
    nts::state::AutoMatchState saved;
    const auto held = autoHeldMask.load(std::memory_order_acquire);
    if (held == 0) return saved;

    saved.holding = true;
    saved.isolatedChain = autoMatchIsolatedChain;
    saved.heldValues.assign(autoMatchValues.begin(), autoMatchValues.end());
    const auto released = autoReleasedMask.load(std::memory_order_acquire);
    for (std::size_t owned = 0; owned < tf::automatch::ownedCount; ++owned)
        if ((released & tf::automatch::bit(owned)) != 0)
            saved.releasedIndices.push_back(static_cast<int>(owned));
    return saved;
}

void TubeForgeAudioProcessor::restoreAutoMatchProjectState(const nts::state::AutoMatchState& saved)
{
    clearAutoMatchHold();
    if (! saved.holding || saved.heldValues.size() != tf::automatch::ownedCount) return;

    std::copy(saved.heldValues.begin(), saved.heldValues.end(), autoMatchValues.begin());
    autoMatchIsolatedChain = saved.isolatedChain;
    autoHeldMask.store(tf::automatch::maskFor(saved.isolatedChain), std::memory_order_release);

    std::uint64_t released {};
    for (const auto index : saved.releasedIndices)
        if (index >= 0 && index < static_cast<int>(tf::automatch::ownedCount))
            released |= tf::automatch::bit(static_cast<std::size_t>(index));
    autoReleasedMask.store(released, std::memory_order_release);
}

void TubeForgeAudioProcessor::setAutoMatchEnabled(bool enabled)
{
    setParameterValue(parameterState, ParameterIds::autoMatch, enabled ? 1.0f : 0.0f);
    // Turning it off releases nothing and reverts nothing: the rig stays exactly as it is and
    // simply stops being held. Re-applying a candidate is what puts it back.
    if (! enabled) clearAutoMatchHold();
}

bool TubeForgeAudioProcessor::autoMatchEnabled() const noexcept
{
    // Through the cached pointer rather than a string lookup: `autoMatchGuarding` asks this on
    // every value change of every owned parameter, and a host can make that call from the audio
    // thread.
    return parameterOf(Param::autoMatch) >= 0.5f;
}

tf::automatch::State TubeForgeAudioProcessor::autoMatchState() const noexcept
{
    if (! autoMatchEnabled()) return tf::automatch::State::off;
    if (autoHeldMask.load(std::memory_order_acquire) == 0) return tf::automatch::State::armed;
    return autoReleasedMask.load(std::memory_order_acquire) == 0
        ? tf::automatch::State::holding : tf::automatch::State::overridden;
}

bool TubeForgeAudioProcessor::autoMatchOwns(std::string_view parameterId) const noexcept
{
    return autoMatchGuarding(tf::automatch::indexOf(parameterId));
}

bool TubeForgeAudioProcessor::autoMatchReleased(std::string_view parameterId) const noexcept
{
    const auto owned = tf::automatch::indexOf(parameterId);
    if (owned < 0) return false;
    const auto mask = tf::automatch::bit(static_cast<std::size_t>(owned));
    if ((autoHeldMask.load(std::memory_order_acquire) & mask) == 0) return false;
    return (autoReleasedMask.load(std::memory_order_acquire) & mask) != 0;
}

std::optional<float> TubeForgeAudioProcessor::autoMatchValueOf(std::string_view parameterId) const noexcept
{
    const auto owned = tf::automatch::indexOf(parameterId);
    if (owned < 0) return std::nullopt;
    const auto mask = tf::automatch::bit(static_cast<std::size_t>(owned));
    if ((autoHeldMask.load(std::memory_order_acquire) & mask) == 0) return std::nullopt;
    return autoMatchValues[static_cast<std::size_t>(owned)];
}

void TubeForgeAudioProcessor::releaseAutoMatchParameter(std::string_view parameterId)
{
    const auto owned = tf::automatch::indexOf(parameterId);
    if (owned < 0) return;
    autoReleasedMask.fetch_or(tf::automatch::bit(static_cast<std::size_t>(owned)),
                              std::memory_order_acq_rel);
}

void TubeForgeAudioProcessor::restoreAutoMatchParameter(std::string_view parameterId)
{
    const auto owned = tf::automatch::indexOf(parameterId);
    if (owned < 0) return;
    const auto mask = tf::automatch::bit(static_cast<std::size_t>(owned));
    if ((autoHeldMask.load(std::memory_order_acquire) & mask) == 0) return;
    {
        // Inside the scope, or putting the matched value back would itself look like a user
        // gesture and ask the same question again.
        const AutoWriteScope scope(*this);
        setParameterValue(parameterState, std::string(parameterId).c_str(),
                          autoMatchValues[static_cast<std::size_t>(owned)]);
    }
    autoReleasedMask.fetch_and(~mask, std::memory_order_acq_rel);
    autoExternalReleaseMask.fetch_and(~mask, std::memory_order_acq_rel);
}

void TubeForgeAudioProcessor::reclaimAllAutoMatchParameters()
{
    const auto held = autoHeldMask.load(std::memory_order_acquire);
    if (held == 0) return;
    {
        const AutoWriteScope scope(*this);
        for (std::size_t owned = 0; owned < tf::automatch::ownedCount; ++owned)
            if ((held & tf::automatch::bit(owned)) != 0)
                setParameterValue(parameterState, std::string(tf::automatch::owned[owned].id).c_str(),
                                  autoMatchValues[owned]);
    }
    autoReleasedMask.store(0, std::memory_order_release);
    autoExternalReleaseMask.store(0, std::memory_order_release);
}

int TubeForgeAudioProcessor::autoMatchReleasedCount() const noexcept
{
    const auto released = autoReleasedMask.load(std::memory_order_acquire)
                        & autoHeldMask.load(std::memory_order_acquire);
    auto count = 0;
    for (std::size_t owned = 0; owned < tf::automatch::ownedCount; ++owned)
        if ((released & tf::automatch::bit(owned)) != 0) ++count;
    return count;
}

int TubeForgeAudioProcessor::takeAutoMatchWarning() noexcept
{
    return autoWarningRequest.exchange(-1, std::memory_order_acq_rel);
}

juce::String TubeForgeAudioProcessor::autoMatchParameterName(int ownedIndex) const
{
    if (ownedIndex < 0 || ownedIndex >= static_cast<int>(tf::automatch::ownedCount)) return {};
    const auto id = juce::String(std::string(
        tf::automatch::owned[static_cast<std::size_t>(ownedIndex)].id));
    // The host-visible name rather than a second table of display names here: a page's label and
    // the parameter's name can differ, and the one the user can also see in their DAW is the one
    // that identifies the control without ambiguity.
    if (const auto* parameter = parameterState.getParameter(id)) return parameter->getName(64);
    return id;
}

juce::StringArray TubeForgeAudioProcessor::takeAutoMatchExternalReleases()
{
    const auto external = autoExternalReleaseMask.exchange(0, std::memory_order_acq_rel);
    juce::StringArray names;
    for (std::size_t owned = 0; owned < tf::automatch::ownedCount; ++owned)
        if ((external & tf::automatch::bit(owned)) != 0)
            names.add(autoMatchParameterName(static_cast<int>(owned)));
    return names;
}

juce::String TubeForgeAudioProcessor::autoMatchSourceName() const
{
    const std::scoped_lock lock(autoMatchMutex);
    return juce::String::fromUTF8(autoMatchSource.c_str());
}

juce::String TubeForgeAudioProcessor::autoMatchStatusText() const
{
    switch (autoMatchState())
    {
        case tf::automatch::State::off:
            return "Auto Match is off. Applying a candidate sets the rig once and then leaves it to you.";
        case tf::automatch::State::armed:
            return "Auto Match is on and waiting for a match. Import a song and apply a candidate.";
        case tf::automatch::State::holding:
        case tf::automatch::State::overridden:
            break;
    }

    auto held = 0;
    const auto heldMask = autoHeldMask.load(std::memory_order_acquire);
    for (std::size_t owned = 0; owned < tf::automatch::ownedCount; ++owned)
        if ((heldMask & tf::automatch::bit(owned)) != 0) ++held;
    const auto released = autoMatchReleasedCount();

    juce::String source;
    {
        const std::scoped_lock lock(autoMatchMutex);
        source = juce::String::fromUTF8(autoMatchSource.c_str());
    }
    auto text = "Auto Match is holding " + juce::String(held - released) + " of " + juce::String(held)
              + " controls";
    if (source.isNotEmpty()) text += " from " + source;
    if (released > 0) text += "   /   " + juce::String(released) + " handed back to you";
    return text;
}

/** The value a warning dialog quotes back. Kept here beside the snapshot rather than in the
    editor, because the editor has the parameter but not the matched value, and formatting a
    raw float as "6.40" where the panel says "6.4 dB" is how a dialog stops being believable.
*/
juce::String TubeForgeAudioProcessor::autoMatchValueText(int ownedIndex) const
{
    if (ownedIndex < 0 || ownedIndex >= static_cast<int>(tf::automatch::ownedCount)) return {};
    const auto id = juce::String(std::string(
        tf::automatch::owned[static_cast<std::size_t>(ownedIndex)].id));
    const auto* parameter = parameterState.getParameter(id);
    if (parameter == nullptr) return {};
    return describeParameter(*parameter, autoMatchValues[static_cast<std::size_t>(ownedIndex)]);
}
