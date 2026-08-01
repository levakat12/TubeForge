// The audio callback and everything it touches: preparation, the block itself,
// latency reporting and the parameter reads that feed them.

#include "PluginProcessorInternal.h"

void TubeForgeAudioProcessor::refreshEffectParameters() noexcept
{
    // Called from prepareToPlay as well as the audio callback. Without the former, a freshly
    // prepared effect still holds its struct defaults -- which are not the parameter defaults
    // -- and reports a tail for a send that is actually silent.
    nts::dsp::DelayParameters delayParameters;
    delayParameters.timeMs = parameterOf(Param::delayTime);
    delayParameters.feedback = parameterOf(Param::delayFeedback) * 0.01f;
    delayParameters.mix = parameterOf(Param::delayMix) * 0.01f;
    delayParameters.dampingHz = parameterOf(Param::delayTone);
    delayEffect.setParameters(delayParameters);

    nts::dsp::ReverbParameters reverbParameters;
    reverbParameters.size = parameterOf(Param::reverbSize) * 0.01f;
    reverbParameters.damping = parameterOf(Param::reverbDamping) * 0.01f;
    reverbParameters.mix = parameterOf(Param::reverbMix) * 0.01f;
    reverbEffect.setParameters(reverbParameters);
}

void TubeForgeAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    if (circuitCompiler.joinable())
    {
        circuitCompiler.request_stop();
        circuitCompiler.join();
    }
    currentSampleRate = sampleRate;
    currentBlockSize = samplesPerBlock;
    absoluteSamplePosition = 0;
    inputSnapshot.setSize(std::max(1, getTotalNumInputChannels()), std::max(1, samplesPerBlock),
                          false, true, false);
    // Logarithmic so a trim sweep is perceptually even rather than bunched at the top.
    inputTrimGain.prepare(sampleRate, 20.0, nts::dsp::SmoothingMode::logarithmic);

    // Decimate towards roughly 8 kHz for pitch tracking. The correlation search is O(lags x
    // frame), so working at a sixth of the rate is what keeps it cheap enough to run on the
    // message thread; a guitar's fundamental is nowhere near the reduced Nyquist.
    tunerDecimationFactor = std::max(1, static_cast<int>(std::lround(sampleRate / tunerAnalysisRate)));
    tunerDecimationCounter = 0;
    tunerDecimationAccumulator = 0.0f;
    const auto analysisRate = sampleRate / static_cast<double>(tunerDecimationFactor);
    // Low B on a five-string bass is 30.87 Hz; going a little below it leaves room for a
    // badly flat string to still register rather than vanishing off the bottom of the range.
    tunerDetector.prepare(analysisRate, static_cast<std::size_t>(analysisRate * 0.5), 28.0, 1400.0);
    tunerFrame.assign(std::max<std::size_t>(tunerDetector.recommendedFrameSamples() * 2,
                                            static_cast<std::size_t>(analysisRate * 0.25)), 0.0f);
    tunerFrameFill = 0;
    { const std::scoped_lock lock(tunerMutex); latestTunerReading = {}; }
    inputTrimGain.reset(nts::dsp::dbToLinear(parameterOf(Param::input)));

    const nts::dsp::ProcessSpec sharedSpec {
        sampleRate, static_cast<std::size_t>(samplesPerBlock),
        static_cast<std::size_t>(std::max(1, getTotalNumOutputChannels()))
    };
    sharedGate.prepare(sharedSpec);
    sharedGate.reset();
    delayEffect.prepare(sharedSpec);
    reverbEffect.prepare(sharedSpec);
    refreshEffectParameters();

    const auto outputChannelCount = static_cast<std::size_t>(std::max(1, getTotalNumOutputChannels()));
    dryBuffer.setSize(static_cast<int>(outputChannelCount), std::max(1, samplesPerBlock), false, true, false);
    dryDelay.prepare(maximumDryDelaySamples, outputChannelCount);
    // Equal-gain rather than equal-power: the two sides of this fade are the same signal,
    // so they sum coherently and an equal-power law would bulge through the middle.
    bypassMix.prepare(sampleRate, 20.0);
    bypassMix.reset(parameterOf(Param::bypass) >= 0.5f ? 1.0f : 0.0f);
    engineSwitchGain.prepare(sampleRate, 5.0);
    engineSwitchGain.reset(1.0f);
    engineSwitchPhase = EngineSwitch::idle;
    activeEngineMode = std::clamp(static_cast<int>(std::lround(parameterOf(Param::engineMode))), 0, 2);
    engine.prepare(sampleRate,
                   static_cast<std::size_t>(samplesPerBlock),
                   static_cast<std::size_t>(getTotalNumInputChannels()),
                   static_cast<std::size_t>(getTotalNumOutputChannels()));
    traditionalAmp.prepare({ sampleRate, static_cast<std::size_t>(samplesPerBlock),
                              static_cast<std::size_t>(getTotalNumOutputChannels()) });
    traditionalAmp.setParametersImmediately(currentAmpParameters());
    // prepare() reinstates the built-in responses, so any user response has to go back in --
    // and re-prepared against the new rate, which is the reason the decoded audio is kept.
    for (int slot = 0; slot < 2; ++slot) applyCabinetIr(slot);
    physicalCircuit.prepare({ sampleRate, static_cast<std::size_t>(samplesPerBlock),
                              static_cast<std::size_t>(getTotalNumOutputChannels()) });
    physicalCircuitControlHash.store(0, std::memory_order_relaxed);
    physicalCircuitRequestedHash.store(0, std::memory_order_relaxed);
    refreshPhysicalCircuit();
    neuralAmp.prepare(sampleRate, static_cast<std::size_t>(samplesPerBlock),
                      static_cast<std::size_t>(getTotalNumOutputChannels()));
    diagnostics.prepare(sampleRate, static_cast<std::uint32_t>(samplesPerBlock));
    latencyBudget.hostSamples = samplesPerBlock;
    latencyBudget.oversamplingSamples = static_cast<int>(traditionalAmp.latencySamples());
    pendingOversamplingLatencySamples.store(latencyBudget.oversamplingSamples,
                                            std::memory_order_relaxed);
    diagnostics.setLatencyBudget(latencyBudget);
    updateReportedLatency();

    logger.log({ std::chrono::system_clock::now(), nts::diagnostics::LogSeverity::info,
                 "audio", "prepared", "Audio engine prepared",
                 "sampleRate=" + std::to_string(sampleRate) + ",blockSize=" + std::to_string(samplesPerBlock) });
}

void TubeForgeAudioProcessor::releaseResources()
{
    if (circuitCompiler.joinable())
    {
        circuitCompiler.request_stop();
        circuitCompiler.join();
    }
    cancelPendingUpdate();
    engine.reset();
    traditionalAmp.reset();
    physicalCircuit.reset();
    neuralAmp.reset();
}

bool TubeForgeAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto input = layouts.getMainInputChannelSet();
    const auto output = layouts.getMainOutputChannelSet();
    const auto supportedChannelCount = output == juce::AudioChannelSet::mono()
                                    || output == juce::AudioChannelSet::stereo();
    return supportedChannelCount && input == output;
}

void TubeForgeAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    // Only latched here. Applying it means touching parameter objects, which is message-thread
    // work; the last program change in the block wins, which is what a foot controller sending
    // a burst of them should do anyway.
    for (const auto entry : midi)
    {
        const auto message = entry.getMessage();
        if (! message.isProgramChange()) continue;
        const auto program = message.getProgramChangeNumber();
        if (program >= 0 && program < factoryProgramCount)
        {
            pendingProgramChange.store(program, std::memory_order_relaxed);
            triggerAsyncUpdate();
        }
    }
    const auto startedAt = diagnostics.beginCallback();
    const auto assistantInputMeasurement = measureBuffer(buffer);

    runtimeParameters.inputGainDb.store(0.0f, std::memory_order_relaxed);
    runtimeParameters.outputGainDb.store(parameterOf(Param::output), std::memory_order_relaxed);
    // The core engine is now always the wet path, never the bypass. Bypass is a crossfade
    // to a delayed dry copy at the end of this function, so the output gain has to keep
    // being applied to the processed signal for there to be something to fade away from.
    runtimeParameters.bypass.store(false, std::memory_order_relaxed);

    constexpr auto maximumChannels = nts::audio::MeterState::maximumChannels;
    std::array<const float*, maximumChannels> inputs {};
    std::array<float*, maximumChannels> outputs {};
    const auto inputCount = std::min(getTotalNumInputChannels(), static_cast<int>(maximumChannels));
    const auto outputCount = std::min(getTotalNumOutputChannels(), static_cast<int>(maximumChannels));

    // Snapshot the input before the amplifier overwrites the buffer in place. The
    // engine still processes the buffer itself, so the audio path is unchanged;
    // the copy exists only to recover a true input level for metering below.
    const auto snapshotChannels = std::min(inputCount, inputSnapshot.getNumChannels());
    const auto snapshotSamples = std::min(buffer.getNumSamples(), inputSnapshot.getNumSamples());
    for (int channel = 0; channel < snapshotChannels; ++channel)
        inputSnapshot.copyFrom(channel, 0, buffer, channel, 0, snapshotSamples);

    // Feed the tuner from the untouched input, before any trim or amplifier stage: it has to
    // track the string, not what the amp does to it. Box-average while decimating so the
    // discarded samples still contribute rather than aliasing into the reading.
    for (int sample = 0; sample < snapshotSamples; ++sample)
    {
        auto sum = 0.0f;
        for (int channel = 0; channel < snapshotChannels; ++channel)
            sum += inputSnapshot.getSample(channel, sample);
        tunerDecimationAccumulator += sum / static_cast<float>(std::max(1, snapshotChannels));
        if (++tunerDecimationCounter >= tunerDecimationFactor)
        {
            // A full queue simply drops the sample: the tuner is a display, and stalling the
            // audio thread to keep it fed would be the wrong trade.
            static_cast<void>(tunerQueue.push(tunerDecimationAccumulator
                                              / static_cast<float>(tunerDecimationFactor)));
            tunerDecimationAccumulator = 0.0f;
            tunerDecimationCounter = 0;
        }
    }

    for (int channel = 0; channel < inputCount; ++channel)
        inputs[static_cast<std::size_t>(channel)] = buffer.getReadPointer(channel);
    for (int channel = 0; channel < outputCount; ++channel)
        outputs[static_cast<std::size_t>(channel)] = buffer.getWritePointer(channel);

    if (auto* hostPlayHead = getPlayHead())
    {
        if (const auto position = hostPlayHead->getPosition())
        {
            const auto playing = position->getIsPlaying();
            if (const auto time = position->getTimeInSamples())
            {
                if ((expectedNextHostSample >= 0 && *time != expectedNextHostSample)
                    || (playing && ! previousTransportPlaying))
                    neuralAmp.requestStateReset();
                expectedNextHostSample = *time + buffer.getNumSamples();
            }
            previousTransportPlaying = playing;
        }
    }

    // A mode change mutes, switches at the block boundary, and unmutes. The alternative --
    // crossfading the outgoing and incoming engines -- cannot work here: the three report
    // different latencies, so overlapping them comb-filters the transition instead of
    // blending it. Ten milliseconds of silence when someone deliberately changes amplifier
    // engine is imperceptible, and it costs neither latency nor transient smearing.
    const auto requestedEngineMode = std::clamp(
        static_cast<int>(std::lround(parameterOf(Param::engineMode))), 0, 2);
    if (engineSwitchPhase == EngineSwitch::idle && requestedEngineMode != activeEngineMode)
    {
        engineSwitchPhase = EngineSwitch::fadingOut;
        engineSwitchGain.setTarget(0.0f);
    }
    if (activeEngineMode != 2) physicalCircuit.publishPendingWithoutCrossfade();

    // Always processed, even while bypassed. The bypass crossfade needs a live wet signal
    // to fade away from, and an engine that had been parked would fade back in on state
    // frozen from whenever it last ran.
    {
        // The shared front end: one input trim ahead of whichever engine is selected.
        //
        // The traditional amplifier's own trim is zeroed in currentAmpParameters so this is
        // the only one, which makes the Input control behave identically in all three modes.
        // Hoisting it is exactly output-preserving for the traditional path because AmpVoice
        // applies its trim before capturing the dry reference: trimming here and passing unity
        // through leaves that reference holding the same samples it always did.
        {
            const auto samples = static_cast<std::size_t>(buffer.getNumSamples());
            inputTrimGain.setTarget(nts::dsp::dbToLinear(parameterOf(Param::input)));
            for (std::size_t sample = 0; sample < samples; ++sample)
            {
                const auto gain = inputTrimGain.next();
                for (int channel = 0; channel < outputCount; ++channel)
                    outputs[static_cast<std::size_t>(channel)][sample] *= gain;
            }
        }

        // The gate, for the engines that have none.
        //
        // Deliberately *not* hoisted out of the traditional path as well, even though that
        // would be tidier. AmpVoice captures its dry reference between the trim and the gate,
        // and feeds it to the loudness match; gating upstream would lower the input energy
        // that match tracks during gated passages and quietly push its gain up. The neural and
        // circuit engines have no such reference, so they can be gated here for free.
        if (activeEngineMode != 0 && parameterOf(Param::gateEnabled) >= 0.5f)
        {
            // Mapped with the same clamps AmpVoice uses, so the same control settings give the
            // same gate behaviour whichever engine is running.
            nts::dsp::NoiseGateParameters gate;
            gate.thresholdDb = parameterOf(Param::gateThreshold);
            gate.rangeDb = std::clamp(parameterOf(Param::gateDepth), -90.0f, 0.0f);
            gate.attackMs = std::clamp(static_cast<double>(parameterOf(Param::gateAttack)), 0.1, 50.0);
            gate.holdMs = std::clamp(static_cast<double>(parameterOf(Param::gateHold)), 0.0, 500.0);
            gate.releaseMs = std::clamp(static_cast<double>(parameterOf(Param::gateRelease)), 5.0, 2000.0);
            sharedGate.setParameters(gate);
            sharedGate.process(outputs.data(), static_cast<std::size_t>(outputCount),
                               static_cast<std::size_t>(buffer.getNumSamples()));
        }

        if (activeEngineMode == 1)
        {
            const std::array controls {
                std::clamp(parameterOf(Param::gain) * 0.2f - 1.0f, -1.0f, 1.0f),
                std::clamp((parameterOf(Param::bass)
                            + parameterOf(Param::mid)
                            + parameterOf(Param::treble)) / 15.0f - 1.0f, -1.0f, 1.0f),
                std::clamp((parameterOf(Param::master) + 24.0f) / 36.0f - 1.0f, -1.0f, 1.0f),
                std::clamp(parameterOf(Param::topology) * 2.0f - 1.0f, -1.0f, 1.0f),
                std::clamp(parameterOf(Param::instrument) * 2.0f - 1.0f, -1.0f, 1.0f)
            };
            neuralAmp.setControls(controls);
            neuralAmp.setMonitorMode(static_cast<nts::ml::NeuralMonitorMode>(std::clamp(
                static_cast<int>(parameterOf(Param::neuralMonitor)), 0, 2)));
            neuralAmp.setInputCompensationEnabled(
                parameterOf(Param::neuralCompensation) >= 0.5f);
            neuralAmp.process(outputs.data(), static_cast<std::size_t>(outputCount),
                              static_cast<std::size_t>(buffer.getNumSamples()));
        }
        else if (activeEngineMode == 2)
        {
            physicalCircuit.process(outputs.data(), static_cast<std::size_t>(outputCount),
                                    static_cast<std::size_t>(buffer.getNumSamples()));
        }
        else
        {
            const auto ampParameters = currentAmpParameters();
            traditionalAmp.setParameters(ampParameters);
            traditionalAmp.process(outputs.data(), static_cast<std::size_t>(outputCount),
                                   static_cast<std::size_t>(buffer.getNumSamples()));
        }

        // Delay then reverb, in that order: reverb on the repeats sounds like a room the
        // echoes happen in, whereas delaying a reverb tail smears it into mush.
        {
            refreshEffectParameters();
            delayEffect.process(outputs.data(), static_cast<std::size_t>(outputCount),
                                static_cast<std::size_t>(buffer.getNumSamples()));
            reverbEffect.process(outputs.data(), static_cast<std::size_t>(outputCount),
                                 static_cast<std::size_t>(buffer.getNumSamples()));
        }
    }
    refreshProcessingLatency(true);

    nts::audio::AudioProcessContext context {
        std::span<const float* const>(inputs.data(), static_cast<std::size_t>(inputCount)),
        std::span<float* const>(outputs.data(), static_cast<std::size_t>(outputCount)),
        static_cast<std::size_t>(buffer.getNumSamples()),
        currentSampleRate,
        absoluteSamplePosition
    };

    diagnostics.verifySampleRate(context.sampleRate, absoluteSamplePosition);
    engine.process(context);

    // Bypass, and the mute that carries an engine change. The dry side is the untouched
    // input delayed by exactly the latency the host has been told to compensate for, so
    // engaging bypass neither steps the signal nor shifts it in time against other tracks.
    {
        const auto samples = static_cast<std::size_t>(buffer.getNumSamples());
        const auto channels = static_cast<std::size_t>(std::min(outputCount, dryBuffer.getNumChannels()));
        dryDelay.setDelay(static_cast<std::size_t>(
            std::max(0, pendingOversamplingLatencySamples.load(std::memory_order_relaxed))));

        std::array<const float*, maximumChannels> dryInputs {};
        std::array<float*, maximumChannels> dryOutputs {};
        for (std::size_t channel = 0; channel < channels; ++channel)
        {
            // Channels beyond the snapshot fall back to silence rather than reading a
            // buffer that was never written.
            dryInputs[channel] = static_cast<int>(channel) < snapshotChannels
                               ? inputSnapshot.getReadPointer(static_cast<int>(channel))
                               : dryBuffer.getReadPointer(static_cast<int>(channel));
            dryOutputs[channel] = dryBuffer.getWritePointer(static_cast<int>(channel));
        }
        if (static_cast<int>(channels) > snapshotChannels) dryBuffer.clear();
        dryDelay.process(dryInputs.data(), dryOutputs.data(), channels, samples);

        // Tuning mute rides the same ramp as the engine-change mute, so it fades rather than
        // cutting. It silences the output only -- the tuner is fed from the input snapshot
        // taken before any of this, so it keeps tracking while the amp is quiet.
        if (parameterOf(Param::tunerMute) >= 0.5f) engineSwitchGain.setTarget(0.0f);
        else if (engineSwitchPhase == EngineSwitch::idle) engineSwitchGain.setTarget(1.0f);

        bypassMix.setTarget(parameterOf(Param::bypass) >= 0.5f ? 1.0f : 0.0f);
        for (std::size_t sample = 0; sample < samples; ++sample)
        {
            const auto mix = bypassMix.next();
            const auto mute = engineSwitchGain.next();
            for (std::size_t channel = 0; channel < channels; ++channel)
            {
                const auto wet = outputs[channel][sample] * mute;
                outputs[channel][sample] = wet * (1.0f - mix) + dryOutputs[channel][sample] * mix;
            }
        }
    }

    // Completed at a block boundary so no engine is swapped out from under a partly
    // written buffer. The incoming engine is reset because its state has been sitting
    // idle since it last ran, and a stale supply or filter state would fade back in.
    if (engineSwitchPhase == EngineSwitch::fadingOut && ! engineSwitchGain.isSmoothing())
    {
        activeEngineMode = requestedEngineMode;
        switch (activeEngineMode)
        {
            case 1: neuralAmp.reset(); break;
            case 2: physicalCircuit.reset(); break;
            default: traditionalAmp.reset(); break;
        }
        engineSwitchPhase = EngineSwitch::fadingIn;
        engineSwitchGain.setTarget(1.0f);
    }
    else if (engineSwitchPhase == EngineSwitch::fadingIn && ! engineSwitchGain.isSmoothing())
    {
        engineSwitchPhase = EngineSwitch::idle;
    }

    // The engine meters the buffer it was handed, which the amplifier has already
    // written to, so its input peaks are really post-amplifier levels. Republish
    // them from the untouched snapshot, and take the output peak from the buffer as it
    // now stands so that a bypassed or muted signal meters what actually leaves.
    for (int channel = 0; channel < snapshotChannels; ++channel)
    {
        const auto index = static_cast<std::size_t>(channel);
        meters.publish(index, inputSnapshot.getMagnitude(channel, 0, snapshotSamples),
                       buffer.getMagnitude(channel, 0, buffer.getNumSamples()));
    }

    diagnostics.endCallback(startedAt, static_cast<std::uint32_t>(buffer.getNumSamples()), absoluteSamplePosition);
    const auto assistantOutputMeasurement = measureBuffer(buffer);
    nts::assistant::AudioSummaryFrame assistantFrame;
    assistantFrame.inputPeak = assistantInputMeasurement.peak;
    assistantFrame.inputRms = assistantInputMeasurement.rms;
    assistantFrame.outputPeak = assistantOutputMeasurement.peak;
    assistantFrame.outputRms = assistantOutputMeasurement.rms;
    assistantFrame.zeroCrossingRate = assistantInputMeasurement.zeroCrossingRate;
    assistantFrame.channelCorrelation = assistantOutputMeasurement.correlation;
    assistantFrame.inputClipped = assistantInputMeasurement.clipped;
    assistantFrame.outputClipped = assistantOutputMeasurement.clipped;
    assistantFrame.samples = static_cast<std::uint32_t>(buffer.getNumSamples());
    assistantFrame.latencySamples = pendingOversamplingLatencySamples.load(std::memory_order_relaxed);
    assistantFrame.sampleRate = currentSampleRate;
    (void) assistantSummaryQueue.push(assistantFrame);
    absoluteSamplePosition += static_cast<std::uint64_t>(buffer.getNumSamples());
}

double TubeForgeAudioProcessor::getTailLengthSeconds() const
{
    // Reported across every engine rather than the active one. A host negotiates the tail
    // once; if switching to a shorter-tailed mode narrowed it, the host would already have
    // committed to the smaller window and the longer mode's decay would be cut.
    // The effects outlast the cabinet by a wide margin -- a long reverb is seconds where the
    // cabinet is milliseconds -- so they set the tail whenever they are engaged.
    const auto tail = std::max({ traditionalAmp.tailSamples(), physicalCircuit.tailSamples(),
                                 delayEffect.tailSamples() + reverbEffect.tailSamples() });
    const auto latency = static_cast<std::size_t>(
        std::max(0, pendingOversamplingLatencySamples.load(std::memory_order_relaxed)));
    return static_cast<double>(tail + latency) / std::max(1.0, currentSampleRate);
}

void TubeForgeAudioProcessor::updateTuner()
{
    if (tunerFrame.empty()) return;

    // Drain everything queued since the last tick, keeping the most recent frame's worth.
    // Sliding rather than refilling from empty means a reading is available every tick
    // instead of once per frame length.
    auto received = false;
    for (float sample {}; tunerQueue.pop(sample);)
    {
        if (tunerFrameFill < tunerFrame.size())
        {
            tunerFrame[tunerFrameFill++] = sample;
        }
        else
        {
            std::rotate(tunerFrame.begin(), tunerFrame.begin() + 1, tunerFrame.end());
            tunerFrame.back() = sample;
        }
        received = true;
    }
    if (! received || tunerFrameFill < tunerFrame.size()) return;

    const auto pitch = tunerDetector.analyse(tunerFrame);
    TunerReading reading;
    if (pitch.voiced)
    {
        const auto note = nts::dsp::nearestNote(pitch.frequencyHz);
        reading.midiNote = note.midiNote;
        reading.cents = note.cents;
        reading.frequencyHz = pitch.frequencyHz;
        reading.confidence = pitch.confidence;
        reading.voiced = note.midiNote >= 0;
    }
    const std::scoped_lock lock(tunerMutex);
    latestTunerReading = reading;
}

TubeForgeAudioProcessor::TunerReading TubeForgeAudioProcessor::tunerReading() const noexcept
{
    const std::scoped_lock lock(tunerMutex);
    return latestTunerReading;
}

int TubeForgeAudioProcessor::automaticOversamplingFactor(
    const nts::amp::AmpParameters& parameters) const noexcept
{
    // Aliasing grows with how hard the cascade is driven, because a saturating
    // stage generates harmonics far above Nyquist that fold back inaudibly high
    // gain but audibly at high gain. Total stage drive is the cheapest honest
    // proxy for that, so the factor follows it.
    //
    // 2x and 4x report identical latency (8 samples per oversampled stage), so
    // the only boundary that can move reported latency is 1x, and that sits
    // where the amp is genuinely clean and unlikely to be automated across.
    auto cumulativeDriveDb = 0.0f;
    const auto activeStages = std::min(parameters.stageCount, parameters.stages.size());
    for (std::size_t stage = 0; stage < activeStages; ++stage)
        cumulativeDriveDb += parameters.stages[stage].driveDb;

    // Hysteresis, so a control resting on a boundary does not switch repeatedly.
    constexpr auto margin = 4.0f;
    const auto previous = autoOversamplingFactor.load(std::memory_order_relaxed);
    const auto cleanCeiling = previous == 1 ? 6.0f : 6.0f - margin;
    const auto drivenFloor = previous == 4 ? 30.0f - margin : 30.0f;

    const auto factor = cumulativeDriveDb <= cleanCeiling ? 1
                      : cumulativeDriveDb >= drivenFloor ? 4 : 2;
    autoOversamplingFactor.store(factor, std::memory_order_relaxed);
    return factor;
}

nts::amp::AmpParameters TubeForgeAudioProcessor::currentAmpParameters() const noexcept
{
    const auto instrumentIndex = std::clamp(static_cast<int>(
        parameterOf(Param::instrument)), 0, 1);
    const auto topologyIndex = std::clamp(static_cast<int>(
        parameterOf(Param::topology)), 0, 1);
    auto parameters = factoryAmpParameters[static_cast<std::size_t>(instrumentIndex * 2 + topologyIndex)];
    // Zero: the shared front end in processBlock applies the trim for every engine, so
    // leaving it here as well would apply it twice.
    parameters.manualInputTrimDb = 0.0f;
    const auto simpleGainOffset = (parameterOf(Param::gain) - 5.0f) * 3.0f;
    parameters.toneStack.bass = parameterOf(Param::bass) * 0.1f;
    parameters.toneStack.mid = parameterOf(Param::mid) * 0.1f;
    parameters.toneStack.treble = parameterOf(Param::treble) * 0.1f;
    parameters.powerAmp.presence = parameterOf(Param::presence) * 0.1f;
    parameters.powerAmp.resonance = parameterOf(Param::resonance) * 0.1f;
    parameters.powerAmp.masterDb = parameterOf(Param::master);
    parameters.cabinet.bypass = parameterOf(Param::cabinet) < 0.5f;
    // stage1..stage4 are adjacent enumerators, so the stage index walks them directly.
    static_assert(static_cast<std::size_t>(Param::stage4) - static_cast<std::size_t>(Param::stage1) == 3,
                  "stage drive parameters must stay contiguous for this indexing to hold");
    const auto bias = parameterOf(Param::bias);
    for (std::size_t stage = 0; stage < parameters.stages.size(); ++stage)
    {
        const auto stageParam = static_cast<Param>(static_cast<std::size_t>(Param::stage1) + stage);
        parameters.stages[stage].driveDb = parameterOf(stageParam) + simpleGainOffset;
        parameters.stages[stage].bias = bias;
    }
    const auto oversamplingIndex = std::clamp(static_cast<int>(
        parameterOf(Param::oversampling)), 0, automaticOversamplingIndex);
    const auto oversamplingFactor = oversamplingIndex == automaticOversamplingIndex
        ? automaticOversamplingFactor(parameters)
        : std::array { 1, 2, 4, 8 }[static_cast<std::size_t>(oversamplingIndex)];
    for (auto& stage : parameters.stages) stage.oversamplingFactor = oversamplingFactor;
    parameters.gateEnabled = parameterOf(Param::gateEnabled) >= 0.5f;
    parameters.gateThresholdDb = parameterOf(Param::gateThreshold);
    parameters.gateDepthDb = parameterOf(Param::gateDepth);
    parameters.gateAttackMs = parameterOf(Param::gateAttack);
    parameters.gateHoldMs = parameterOf(Param::gateHold);
    parameters.gateReleaseMs = parameterOf(Param::gateRelease);
    parameters.preEq.lowCutHz = parameterOf(Param::lowCut);
    parameters.preEq.highCutHz = parameterOf(Param::highCut);
    parameters.preEq.tightness = parameterOf(Param::tightness) * 0.1f;
    parameters.preEq.pickEmphasisDb = parameterOf(Param::pickEmphasis);
    parameters.powerAmp.sag = parameterOf(Param::sag) * 0.01f;
    parameters.powerAmp.feedback = parameterOf(Param::feedback) * 0.01f;
    parameters.bass.crossoverHz = parameterOf(Param::crossover);
    parameters.bass.cleanBlend = parameterOf(Param::cleanBlend) * 0.01f;
    parameters.cabinet.delaySamplesB = static_cast<std::size_t>(
        parameterOf(Param::cabinetAlignment));
    parameters.cabinet.blend = parameterOf(Param::cabinetBlend) * 0.01f;
    return parameters;
}

void TubeForgeAudioProcessor::refreshProcessingLatency(bool notifyHostFromAudioThread) noexcept
{
    // The engine that is actually running, not the one the parameter asks for: during a
    // mode change the two differ for the length of the fade, and reporting the incoming
    // engine's latency early would misalign the dry path against the outgoing audio.
    const auto mode = activeEngineMode;
    const auto processingLatency = mode == 0 ? static_cast<int>(traditionalAmp.latencySamples())
                                            : mode == 2 ? static_cast<int>(physicalCircuit.latencySamples()) : 0;
    const auto previous = pendingOversamplingLatencySamples.exchange(processingLatency,
                                                                      std::memory_order_relaxed);
    if (processingLatency != previous && notifyHostFromAudioThread)
        triggerAsyncUpdate();
}

void TubeForgeAudioProcessor::handleAsyncUpdate()
{
    if (const auto program = pendingProgramChange.exchange(-1, std::memory_order_relaxed); program >= 0)
    {
        setCurrentProgram(program);
        updateHostDisplay();
    }

    latencyBudget.oversamplingSamples = pendingOversamplingLatencySamples.load(std::memory_order_relaxed);
    diagnostics.setLatencyBudget(latencyBudget);
    updateReportedLatency();
}

void TubeForgeAudioProcessor::updateReportedLatency()
{
    const auto processingLatency = latencyBudget.processingSamples();
    if (getLatencySamples() != processingLatency)
        setLatencySamples(processingLatency);
}
