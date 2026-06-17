#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
constexpr double minLoopSeconds = 0.02;
constexpr double defaultLoopSeconds = 0.25;
constexpr double defaultFadeSeconds = 0.012;
constexpr double minFadeSeconds = 0.002;
constexpr double maxFadeSeconds = 0.080;
constexpr double defaultAttackSeconds = 0.012;
constexpr double defaultDecaySeconds = 0.0;
constexpr float defaultSustainLevel = 1.0f;
constexpr double defaultReleaseSeconds = 0.012;
constexpr double minAdsrSeconds = 0.0;
constexpr double maxAdsrSeconds = 5.0;
constexpr double maxLoopEndpointAdjustSeconds = 0.400;
constexpr double loopEndpointSearchStepSeconds = 0.001;
constexpr int sequenceOutputSmoothingSamples = 256;

double clampDouble (double value, double low, double high)
{
    return std::min (std::max (value, low), high);
}

float dbToGain (float db)
{
    return juce::Decibels::decibelsToGain (db, -80.0f);
}

float sequenceOutputVolumeGainFromCcValue (int value)
{
    const auto clampedValue = juce::jlimit (0, 100, value);
    const auto attenuationDb = (float) (100 - clampedValue) * -0.3f;
    return dbToGain (attenuationDb);
}

float rampedSequenceOutputGain (float startGain, float endGain, int rampSamples, int sample)
{
    if (rampSamples <= 1 || sample >= rampSamples)
        return endGain;

    const auto t = (float) (sample + 1) / (float) rampSamples;
    return startGain + ((endGain - startGain) * t);
}

int envelopeSamplesForHost (double seconds, double sampleRate)
{
    return juce::jmax (0, (int) std::round (clampDouble (seconds, minAdsrSeconds, maxAdsrSeconds) * sampleRate));
}
}

FronasmaskinenAudioProcessor::FronasmaskinenAudioProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    formatManager.registerBasicFormats();
}

void FronasmaskinenAudioProcessor::prepareToPlay (double sampleRate, int)
{
    hostSampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;
    fadeSamples = juce::jmax (16, (int) std::round (defaultFadeSeconds * hostSampleRate));
    crossfadeSamples = fadeSamples;
    releaseAllVoices();
}

void FronasmaskinenAudioProcessor::releaseResources()
{
    releaseAllVoices();
}

bool FronasmaskinenAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& output = layouts.getMainOutputChannelSet();
    return output == juce::AudioChannelSet::mono() || output == juce::AudioChannelSet::stereo();
}

void FronasmaskinenAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();

    if (hostTransportIsStopped())
    {
        sequenceOutputCurrentGain = 1.0f;
        sequenceOutputTargetGain = 1.0f;
    }

    handleMidi (midiMessages);
    const auto sequenceOutputStartGain = sequenceOutputCurrentGain;
    const auto sequenceOutputEndGain = sequenceOutputTargetGain;
    const auto sequenceOutputRampSamples = juce::jmin (buffer.getNumSamples(), sequenceOutputSmoothingSamples);

    const juce::ScopedTryLock lock (dataLock);
    if (! lock.isLocked())
        return;

    renderPreview (buffer,
                   0,
                   buffer.getNumSamples(),
                   sequenceOutputStartGain,
                   sequenceOutputEndGain,
                   sequenceOutputRampSamples);

    for (auto& voice : voices)
        if (voice.active)
            renderVoice (voice,
                         buffer,
                         0,
                         buffer.getNumSamples(),
                         sequenceOutputStartGain,
                         sequenceOutputEndGain,
                         sequenceOutputRampSamples);

    sequenceOutputCurrentGain = sequenceOutputEndGain;
}

void FronasmaskinenAudioProcessor::handleMidi (const juce::MidiBuffer& midiMessages)
{
    for (const auto metadata : midiMessages)
    {
        const auto message = metadata.getMessage();

        if (message.isNoteOn())
            noteOn (message.getNoteNumber(), message.getFloatVelocity());
        else if (message.isNoteOff())
            noteOff (message.getNoteNumber());
        else if (message.isController() && message.getControllerNumber() == sequenceOutputVolumeCc)
            sequenceOutputTargetGain = sequenceOutputVolumeGainFromCcValue (message.getControllerValue());
    }
}

void FronasmaskinenAudioProcessor::noteOn (int noteNumber, float velocity)
{
    lastMidiNote.store (noteNumber);
    lastTriggeredSlot.store (-1);

    const auto slotIndex = noteNumber - baseNote;
    if (slotIndex < 0 || slotIndex >= slotCount)
        return;

    const juce::ScopedLock lock (dataLock);
    if (! slots[(size_t) slotIndex].filled || sampleBuffer.getNumSamples() == 0)
        return;

    beginPreviewSlotTransition (slotIndex);
    selectedSlot = slotIndex;
    activateLoopFromSelectedSlot();
    preview.positionSamples = preview.loopStartSeconds * sampleBufferRate;
    lastTriggeredSlot.store (slotIndex);

    if (preview.playing && ! preview.releasing)
    {
        releaseAllVoices();
        return;
    }

    startVoice (slotIndex, noteNumber, velocity);
}

void FronasmaskinenAudioProcessor::noteOff (int noteNumber)
{
    lastMidiNote.store (noteNumber);

    const auto slotIndex = noteNumber - baseNote;
    if (slotIndex < 0 || slotIndex >= slotCount)
        return;

    const juce::ScopedLock lock (dataLock);
    releaseVoicesForSlot (slotIndex);
}

void FronasmaskinenAudioProcessor::startVoice (int slotIndex, int noteNumber, float velocity)
{
    releaseAllVoices();

    auto& voice = voices[(size_t) nextVoiceIndex];
    nextVoiceIndex = (nextVoiceIndex + 1) % (int) voices.size();

    const auto bounds = effectiveSlotBoundsSamples (slots[(size_t) slotIndex]);
    voice = {};
    voice.active = true;
    voice.slotIndex = slotIndex;
    voice.noteNumber = noteNumber;
    voice.positionSamples = bounds.first;
    voice.velocityGain = 0.35f + (0.65f * juce::jlimit (0.0f, 1.0f, velocity));
}

void FronasmaskinenAudioProcessor::releaseVoicesForSlot (int slotIndex)
{
    for (auto& voice : voices)
    {
        if (voice.active && voice.slotIndex == slotIndex && ! voice.releasing)
        {
            voice.releasing = true;
            voice.releaseSample = 0;
            voice.releaseStartEnvelope = voice.envelope;
        }
    }
}

void FronasmaskinenAudioProcessor::releaseAllVoices()
{
    for (auto& voice : voices)
    {
        if (voice.active && ! voice.releasing)
        {
            voice.releasing = true;
            voice.releaseSample = 0;
            voice.releaseStartEnvelope = voice.envelope;
        }
    }
}

void FronasmaskinenAudioProcessor::renderVoice (Voice& voice,
                                                juce::AudioBuffer<float>& output,
                                                int startSample,
                                                int numSamples,
                                                float outputGainStart,
                                                float outputGainEnd,
                                                int outputGainRampSamples)
{
    if (voice.slotIndex < 0 || voice.slotIndex >= slotCount)
        return;

    const auto slot = slots[(size_t) voice.slotIndex];
    if (! slot.filled)
    {
        voice.active = false;
        return;
    }

    const auto bounds = effectiveSlotBoundsSamples (slot);
    const auto start = bounds.first;
    const auto end = bounds.second;
    const auto loopLength = end - start;
    if (loopLength < 1.0)
    {
        voice.active = false;
        return;
    }

    const auto channels = output.getNumChannels();
    const auto sourceChannels = sampleBuffer.getNumChannels();
    const auto seamFade = slotFadeSamplesForSource (slot, loopLength);
    const auto attackSamples = envelopeSamplesForHost (slot.attackSeconds, hostSampleRate);
    const auto decaySamples = envelopeSamplesForHost (slot.decaySeconds, hostSampleRate);
    const auto releaseSamples = envelopeSamplesForHost (slot.releaseSeconds, hostSampleRate);
    const auto sustain = juce::jlimit (0.0f, 1.0f, slot.sustainLevel);
    const auto gain = dbToGain (slot.gainDb) * voice.velocityGain;
    const auto sampleCount = (double) sampleBuffer.getNumSamples();
    const auto canUsePostRoll = end + (double) seamFade < sampleCount - 1.0;

    for (int sample = 0; sample < numSamples; ++sample)
    {
        if (! voice.active)
            break;

        if (! voice.releasing)
        {
            if (voice.envelopeStage == EnvelopeStage::attack)
            {
                if (attackSamples <= 0)
                {
                    voice.envelope = 1.0f;
                    voice.envelopeStage = EnvelopeStage::decay;
                    voice.decaySample = 0;
                }
                else
                {
                    voice.envelope = (float) voice.attackSample / (float) attackSamples;
                    ++voice.attackSample;

                    if (voice.attackSample >= attackSamples)
                    {
                        voice.envelopeStage = EnvelopeStage::decay;
                        voice.decaySample = 0;
                    }
                }
            }

            if (voice.envelopeStage == EnvelopeStage::decay)
            {
                if (decaySamples <= 0)
                {
                    voice.envelope = sustain;
                    voice.envelopeStage = EnvelopeStage::sustain;
                }
                else
                {
                    const auto t = juce::jlimit (0.0f, 1.0f, (float) voice.decaySample / (float) decaySamples);
                    voice.envelope = 1.0f + ((sustain - 1.0f) * t);
                    ++voice.decaySample;

                    if (voice.decaySample >= decaySamples)
                    {
                        voice.envelope = sustain;
                        voice.envelopeStage = EnvelopeStage::sustain;
                    }
                }
            }

            if (voice.envelopeStage == EnvelopeStage::sustain)
                voice.envelope = sustain;
        }

        if (voice.releasing)
        {
            if (releaseSamples <= 0)
            {
                voice.active = false;
                break;
            }

            const auto t = (float) voice.releaseSample / (float) releaseSamples;
            voice.envelope = voice.releaseStartEnvelope * juce::jlimit (0.0f, 1.0f, 1.0f - t);
            ++voice.releaseSample;

            if (voice.releaseSample >= releaseSamples)
            {
                voice.active = false;
                break;
            }
        }

        while (voice.positionSamples >= end)
        {
            voice.positionSamples -= loopLength;
            if (canUsePostRoll)
                voice.seamCrossfadeActive = true;
            else
                voice.positionSamples += (double) seamFade;
        }

        const auto loopOffset = voice.positionSamples - start;
        const auto distanceToEnd = end - voice.positionSamples;
        const auto preRollXfade = ! canUsePostRoll && distanceToEnd < seamFade
                                    ? 1.0 - (distanceToEnd / (double) seamFade)
                                    : 0.0;
        const auto preRollPosition = start + (double) seamFade - distanceToEnd;
        const auto seamProgress = seamFade > 0 ? loopOffset / (double) seamFade : 1.0;
        const auto seamXfade = voice.seamCrossfadeActive ? juce::jlimit (0.0, 1.0, seamProgress) : 1.0;
        const auto postRollPosition = end + loopOffset;
        if (voice.seamCrossfadeActive && (seamProgress >= 1.0 || postRollPosition >= sampleCount - 1.0))
            voice.seamCrossfadeActive = false;

        for (int channel = 0; channel < channels; ++channel)
        {
            const auto sourceChannel = sourceChannels == 1 ? 0 : juce::jmin (channel, sourceChannels - 1);
            auto value = readSample (sourceChannel, voice.positionSamples);

            if (voice.seamCrossfadeActive)
            {
                const auto tail = readSample (sourceChannel, postRollPosition);
                value = (float) ((tail * (1.0 - seamXfade)) + (value * seamXfade));
            }
            else if (preRollXfade > 0.0)
            {
                const auto head = readSample (sourceChannel, preRollPosition);
                value = (float) ((value * (1.0 - preRollXfade)) + (head * preRollXfade));
            }

            const auto outputGain = rampedSequenceOutputGain (outputGainStart, outputGainEnd, outputGainRampSamples, sample);
            output.addSample (channel, startSample + sample, value * gain * voice.envelope * outputGain);
        }

        voice.positionSamples += sampleBufferRate / hostSampleRate;
    }
}

void FronasmaskinenAudioProcessor::renderPreview (juce::AudioBuffer<float>& output,
                                                  int startSample,
                                                  int numSamples,
                                                  float outputGainStart,
                                                  float outputGainEnd,
                                                  int outputGainRampSamples)
{
    if (! preview.playing || sampleBuffer.getNumSamples() == 0)
        return;

    const auto channels = output.getNumChannels();
    const auto sourceChannels = sampleBuffer.getNumChannels();
    const auto sampleCount = (double) sampleBuffer.getNumSamples();
    auto loopStart = preview.loopStartSeconds * sampleBufferRate;
    auto loopEnd = preview.loopEndSeconds * sampleBufferRate;
    auto loopLength = loopEnd - loopStart;
    const auto canLoop = preview.loopActive && loopLength >= 1.0;
    auto gain = 1.0f;
    auto envelopeFadeSamples = fadeSamples;
    auto seamFade = 1;
    auto canUsePostRoll = false;
    if (canLoop && selectedSlot >= 0 && selectedSlot < slotCount && slots[(size_t) selectedSlot].filled)
    {
        gain = dbToGain (slots[(size_t) selectedSlot].gainDb);
        envelopeFadeSamples = slotFadeSamplesForHost (slots[(size_t) selectedSlot]);
        seamFade = slotFadeSamplesForSource (slots[(size_t) selectedSlot], loopLength);
        canUsePostRoll = loopEnd + (double) seamFade < sampleCount - 1.0;
    }

    for (int sample = 0; sample < numSamples; ++sample)
    {
        if (! preview.playing)
            break;

        if (preview.attackSample < envelopeFadeSamples && ! preview.releasing)
        {
            preview.envelope = (float) preview.attackSample / (float) envelopeFadeSamples;
            ++preview.attackSample;
        }
        else if (! preview.releasing)
        {
            preview.envelope = 1.0f;
        }

        if (preview.releasing)
        {
            const auto t = (float) preview.releaseSample / (float) envelopeFadeSamples;
            preview.envelope = preview.releaseStartEnvelope * juce::jlimit (0.0f, 1.0f, 1.0f - t);
            ++preview.releaseSample;

            if (preview.releaseSample >= envelopeFadeSamples)
            {
                preview.playing = false;
                preview.releasing = false;
                preview.envelope = 0.0f;
                break;
            }
        }

        if (canLoop)
        {
            while (preview.positionSamples >= loopEnd)
            {
                preview.positionSamples -= loopLength;
                if (canUsePostRoll)
                    preview.seamCrossfadeActive = true;
                else
                    preview.positionSamples += (double) seamFade;
            }
        }
        else if (preview.positionSamples >= sampleCount - 1.0)
        {
            preview.playing = false;
            preview.positionSamples = 0.0;
            preview.envelope = 0.0f;
            break;
        }

        const auto loopOffset = preview.positionSamples - loopStart;
        const auto distanceToEnd = loopEnd - preview.positionSamples;
        const auto preRollXfade = canLoop && ! canUsePostRoll && distanceToEnd < seamFade
                                    ? 1.0 - (distanceToEnd / (double) seamFade)
                                    : 0.0;
        const auto preRollPosition = loopStart + (double) seamFade - distanceToEnd;
        const auto seamProgress = seamFade > 0 ? loopOffset / (double) seamFade : 1.0;
        const auto seamXfade = preview.seamCrossfadeActive ? juce::jlimit (0.0, 1.0, seamProgress) : 1.0;
        const auto postRollPosition = loopEnd + loopOffset;
        if (preview.seamCrossfadeActive && (seamProgress >= 1.0 || postRollPosition >= sampleCount - 1.0))
            preview.seamCrossfadeActive = false;

        const auto transitionMix = preview.slotTransitionActive
            ? juce::jlimit (0.0, 1.0, (double) preview.transitionSample / (double) preview.transitionSamples)
            : 1.0;

        for (int channel = 0; channel < channels; ++channel)
        {
            const auto sourceChannel = sourceChannels == 1 ? 0 : juce::jmin (channel, sourceChannels - 1);
            auto value = readSample (sourceChannel, preview.positionSamples);

            if (preview.seamCrossfadeActive)
            {
                const auto tail = readSample (sourceChannel, postRollPosition);
                value = (float) ((tail * (1.0 - seamXfade)) + (value * seamXfade));
            }
            else if (preRollXfade > 0.0)
            {
                const auto head = readSample (sourceChannel, preRollPosition);
                value = (float) ((value * (1.0 - preRollXfade)) + (head * preRollXfade));
            }

            if (preview.slotTransitionActive)
            {
                const auto oldValue = readSample (sourceChannel, preview.transitionPositionSamples)
                                    * (float) preview.transitionGain;
                value = (float) ((oldValue * (1.0 - transitionMix)) + ((value * gain) * transitionMix));
                const auto outputGain = rampedSequenceOutputGain (outputGainStart, outputGainEnd, outputGainRampSamples, sample);
                output.addSample (channel, startSample + sample, value * preview.envelope * outputGain);
            }
            else
            {
                const auto outputGain = rampedSequenceOutputGain (outputGainStart, outputGainEnd, outputGainRampSamples, sample);
                output.addSample (channel, startSample + sample, value * preview.envelope * gain * outputGain);
            }
        }

        preview.positionSamples += sampleBufferRate / hostSampleRate;

        if (preview.slotTransitionActive)
        {
            preview.transitionPositionSamples += sampleBufferRate / hostSampleRate;
            const auto transitionLength = preview.transitionLoopEndSamples - preview.transitionLoopStartSamples;
            if (transitionLength >= 1.0)
            {
                while (preview.transitionPositionSamples >= preview.transitionLoopEndSamples)
                    preview.transitionPositionSamples -= transitionLength;
            }

            ++preview.transitionSample;
            if (preview.transitionSample >= preview.transitionSamples)
                preview.slotTransitionActive = false;
        }
    }
}

float FronasmaskinenAudioProcessor::readSample (int channel, double absoluteSample) const
{
    const auto numSamples = sampleBuffer.getNumSamples();
    if (numSamples <= 1)
        return 0.0f;

    const auto bounded = clampDouble (absoluteSample, 0.0, (double) numSamples - 1.001);
    const auto index = (int) std::floor (bounded);
    const auto fraction = (float) (bounded - (double) index);
    const auto* data = sampleBuffer.getReadPointer (channel);
    return data[index] + ((data[index + 1] - data[index]) * fraction);
}

int FronasmaskinenAudioProcessor::slotFadeSamplesForHost (const Slot& slot) const
{
    const auto fadeSecondsToUse = clampDouble (slot.fadeSeconds, minFadeSeconds, maxFadeSeconds);
    return juce::jmax (1, (int) std::round (fadeSecondsToUse * hostSampleRate));
}

int FronasmaskinenAudioProcessor::slotFadeSamplesForSource (const Slot& slot, double loopLengthSamples) const
{
    const auto fadeSecondsToUse = clampDouble (slot.fadeSeconds, minFadeSeconds, maxFadeSeconds);
    const auto fadeSamplesForSource = (int) std::round (fadeSecondsToUse * sampleBufferRate);
    return juce::jlimit (1, (int) std::floor (loopLengthSamples * 0.45), fadeSamplesForSource);
}

std::pair<double, double> FronasmaskinenAudioProcessor::effectiveSlotBoundsSamples (const Slot& slot) const
{
    const auto duration = getSampleDurationSeconds();
    const auto startSeconds = clampDouble (slot.baseStartSeconds + slot.startTrimSeconds, 0.0, duration - minLoopSeconds);
    const auto endSeconds = clampDouble (slot.baseEndSeconds + slot.endTrimSeconds, startSeconds + minLoopSeconds, duration);
    return { startSeconds * sampleBufferRate, endSeconds * sampleBufferRate };
}

bool FronasmaskinenAudioProcessor::loadAudioFile (const juce::File& file)
{
    if (! file.existsAsFile())
        return false;

    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));
    if (reader == nullptr)
        return false;

    juce::AudioBuffer<float> nextBuffer ((int) reader->numChannels, (int) reader->lengthInSamples);
    reader->read (&nextBuffer, 0, (int) reader->lengthInSamples, 0, true, true);

    const juce::ScopedLock lock (dataLock);
    preview = {};
    slots.fill ({});
    voices.fill ({});
    nextVoiceIndex = 0;
    sampleBuffer = std::move (nextBuffer);
    sampleBufferRate = reader->sampleRate;
    rebuildWaveformThumbnail();
    loadedFile = file;
    selectionStartSeconds = 0.0;
    selectionEndSeconds = std::min (defaultLoopSeconds, getSampleDurationSeconds());
    selectedSlot = -1;
    lastTriggeredSlot.store (-1);
    return true;
}

bool FronasmaskinenAudioProcessor::hasSample() const
{
    const juce::ScopedLock lock (dataLock);
    return sampleBuffer.getNumSamples() > 0;
}

juce::String FronasmaskinenAudioProcessor::getLoadedFilePath() const
{
    const juce::ScopedLock lock (dataLock);
    return loadedFile.getFullPathName();
}

int FronasmaskinenAudioProcessor::getSampleLengthSamples() const
{
    const juce::ScopedLock lock (dataLock);
    return sampleBuffer.getNumSamples();
}

double FronasmaskinenAudioProcessor::getSampleDurationSeconds() const
{
    return sampleBufferRate > 0.0 ? (double) sampleBuffer.getNumSamples() / sampleBufferRate : 0.0;
}

double FronasmaskinenAudioProcessor::getSelectionStartSeconds() const
{
    return selectionStartSeconds;
}

double FronasmaskinenAudioProcessor::getSelectionEndSeconds() const
{
    return selectionEndSeconds;
}

void FronasmaskinenAudioProcessor::setSelection (double startSeconds, double endSeconds)
{
    const juce::ScopedLock lock (dataLock);
    const auto duration = getSampleDurationSeconds();
    if (duration <= 0.0)
        return;

    selectionStartSeconds = clampDouble (startSeconds, 0.0, duration - minLoopSeconds);
    selectionEndSeconds = clampDouble (endSeconds, selectionStartSeconds + minLoopSeconds, duration);
}

void FronasmaskinenAudioProcessor::playPreview()
{
    const juce::ScopedLock lock (dataLock);
    if (sampleBuffer.getNumSamples() == 0)
        return;

    preview.playing = true;
    preview.releasing = false;
    preview.attackSample = 0;
    preview.releaseSample = 0;
    preview.envelope = 0.0f;
    preview.seamCrossfadeActive = false;

    if (preview.positionSamples >= sampleBuffer.getNumSamples() - 1)
        preview.positionSamples = 0.0;
}

void FronasmaskinenAudioProcessor::pausePreview()
{
    const juce::ScopedLock lock (dataLock);
    if (! preview.playing || preview.releasing)
        return;

    preview.releasing = true;
    preview.releaseSample = 0;
    preview.releaseStartEnvelope = preview.envelope;
}

void FronasmaskinenAudioProcessor::togglePreview()
{
    if (isPreviewPlaying())
        pausePreview();
    else
        playPreview();
}

bool FronasmaskinenAudioProcessor::isPreviewPlaying() const
{
    const juce::ScopedLock lock (dataLock);
    return preview.playing && ! preview.releasing;
}

double FronasmaskinenAudioProcessor::getPreviewPositionSeconds() const
{
    const juce::ScopedLock lock (dataLock);
    return sampleBufferRate > 0.0 ? preview.positionSamples / sampleBufferRate : 0.0;
}

void FronasmaskinenAudioProcessor::seekPreview (double positionSeconds)
{
    const juce::ScopedLock lock (dataLock);
    const auto duration = getSampleDurationSeconds();
    if (duration <= 0.0)
        return;

    preview.positionSamples = clampDouble (positionSeconds, 0.0, duration) * sampleBufferRate;
    preview.seamCrossfadeActive = false;
    preview.slotTransitionActive = false;
    if (preview.loopActive && (positionSeconds < preview.loopStartSeconds || positionSeconds >= preview.loopEndSeconds))
    {
        preview.loopActive = false;
        preview.pendingLoopStartActive = false;
    }
}

bool FronasmaskinenAudioProcessor::setLoopPointAtPreviewPosition()
{
    const juce::ScopedLock lock (dataLock);
    if (sampleBuffer.getNumSamples() == 0 || ! preview.playing)
        return false;

    const auto position = sampleBufferRate > 0.0 ? preview.positionSamples / sampleBufferRate : 0.0;
    if (! preview.pendingLoopStartActive)
    {
        preview.pendingLoopStartActive = true;
        preview.pendingLoopStartSeconds = position;
        preview.loopActive = false;
        selectedSlot = -1;
        return true;
    }

    const auto start = std::min (preview.pendingLoopStartSeconds, position);
    const auto end = std::max (preview.pendingLoopStartSeconds, position);
    if (end - start < minLoopSeconds)
        return false;

    preview.pendingLoopStartActive = false;
    if (! saveLoopToNextSlot (start, end, true))
        return false;

    activateLoopFromSelectedSlot();
    preview.positionSamples = preview.loopStartSeconds * sampleBufferRate;
    preview.attackSample = 0;
    preview.envelope = 0.0f;
    return true;
}

void FronasmaskinenAudioProcessor::releasePreviewLoop()
{
    const juce::ScopedLock lock (dataLock);
    preview.loopActive = false;
    preview.pendingLoopStartActive = false;
    preview.slotTransitionActive = false;
}

bool FronasmaskinenAudioProcessor::randomizePreviewLoopStart()
{
    const juce::ScopedLock lock (dataLock);
    if (sampleBuffer.getNumSamples() == 0 || selectedSlot < 0 || selectedSlot >= slotCount)
        return false;

    const auto slot = slots[(size_t) selectedSlot];
    if (! slot.filled)
        return false;

    const auto bounds = effectiveSlotBoundsSamples (slot);
    const auto lengthSeconds = (bounds.second - bounds.first) / sampleBufferRate;
    const auto duration = getSampleDurationSeconds();
    const auto maxStart = duration - lengthSeconds;
    if (lengthSeconds < minLoopSeconds || maxStart <= 0.0)
        return false;

    const auto start = juce::Random::getSystemRandom().nextDouble() * maxStart;
    const auto end = start + lengthSeconds;
    if (! saveLoopToNextSlot (start, end, false))
        return false;

    activateLoopFromSelectedSlot();
    preview.positionSamples = start * sampleBufferRate;
    return true;
}

bool FronasmaskinenAudioProcessor::hasPreviewLoop() const
{
    const juce::ScopedLock lock (dataLock);
    return preview.loopActive;
}

bool FronasmaskinenAudioProcessor::hasPendingLoopStart() const
{
    const juce::ScopedLock lock (dataLock);
    return preview.pendingLoopStartActive;
}

double FronasmaskinenAudioProcessor::getPendingLoopStartSeconds() const
{
    const juce::ScopedLock lock (dataLock);
    return preview.pendingLoopStartActive ? preview.pendingLoopStartSeconds : -1.0;
}

double FronasmaskinenAudioProcessor::getPreviewLoopStartSeconds() const
{
    const juce::ScopedLock lock (dataLock);
    return preview.loopActive ? preview.loopStartSeconds : -1.0;
}

double FronasmaskinenAudioProcessor::getPreviewLoopEndSeconds() const
{
    const juce::ScopedLock lock (dataLock);
    return preview.loopActive ? preview.loopEndSeconds : -1.0;
}

FronasmaskinenAudioProcessor::EditorSnapshot FronasmaskinenAudioProcessor::getEditorSnapshot() const
{
    const juce::ScopedLock lock (dataLock);

    EditorSnapshot snapshot;
    snapshot.hasSample = sampleBuffer.getNumSamples() > 0;
    snapshot.previewPlaying = preview.playing && ! preview.releasing;
    snapshot.previewLoopActive = preview.loopActive;
    snapshot.pendingLoopStartActive = preview.pendingLoopStartActive;
    snapshot.sampleDurationSeconds = sampleBufferRate > 0.0 ? (double) sampleBuffer.getNumSamples() / sampleBufferRate : 0.0;
    snapshot.previewPositionSeconds = sampleBufferRate > 0.0 ? preview.positionSamples / sampleBufferRate : 0.0;
    snapshot.pendingLoopStartSeconds = preview.pendingLoopStartActive ? preview.pendingLoopStartSeconds : -1.0;
    snapshot.previewLoopStartSeconds = preview.loopActive ? preview.loopStartSeconds : -1.0;
    snapshot.previewLoopEndSeconds = preview.loopActive ? preview.loopEndSeconds : -1.0;
    snapshot.selectedSlot = selectedSlot;
    snapshot.slots = slots;
    return snapshot;
}

void FronasmaskinenAudioProcessor::getWaveformThumbnail (std::vector<float>& peaks) const
{
    const juce::ScopedLock lock (dataLock);
    peaks = waveformThumbnail;
}

bool FronasmaskinenAudioProcessor::saveSelectionToSlot (int slotIndex)
{
    if (slotIndex < 0 || slotIndex >= slotCount)
        return false;

    const juce::ScopedLock lock (dataLock);
    if (sampleBuffer.getNumSamples() == 0)
        return false;

    auto& slot = slots[(size_t) slotIndex];
    slot.filled = true;
    slot.baseStartSeconds = selectionStartSeconds;
    slot.baseEndSeconds = selectionEndSeconds;
    slot.startTrimSeconds = 0.0;
    slot.endTrimSeconds = 0.0;
    slot.fadeSeconds = defaultFadeSeconds;
    slot.gainDb = 0.0f;
    slot.attackSeconds = defaultAttackSeconds;
    slot.decaySeconds = defaultDecaySeconds;
    slot.sustainLevel = defaultSustainLevel;
    slot.releaseSeconds = defaultReleaseSeconds;
    selectedSlot = slotIndex;
    activateLoopFromSelectedSlot();
    return true;
}

bool FronasmaskinenAudioProcessor::selectSlot (int slotIndex)
{
    if (slotIndex < 0 || slotIndex >= slotCount)
        return false;

    const juce::ScopedLock lock (dataLock);
    const auto& slot = slots[(size_t) slotIndex];
    if (! slot.filled)
        return false;

    beginPreviewSlotTransition (slotIndex);
    selectedSlot = slotIndex;
    selectionStartSeconds = slot.baseStartSeconds;
    selectionEndSeconds = slot.baseEndSeconds;
    activateLoopFromSelectedSlot();

    if (preview.playing)
        preview.positionSamples = preview.loopStartSeconds * sampleBufferRate;

    return true;
}

bool FronasmaskinenAudioProcessor::moveSlotLeft (int slotIndex)
{
    return swapFilledSlots (slotIndex, slotIndex - 1);
}

bool FronasmaskinenAudioProcessor::moveSlotRight (int slotIndex)
{
    return swapFilledSlots (slotIndex, slotIndex + 1);
}

void FronasmaskinenAudioProcessor::clearSlot (int slotIndex)
{
    if (slotIndex < 0 || slotIndex >= slotCount)
        return;

    const juce::ScopedLock lock (dataLock);
    slots[(size_t) slotIndex] = {};
    if (selectedSlot == slotIndex)
    {
        selectedSlot = -1;
        preview.loopActive = false;
        preview.pendingLoopStartActive = false;
        preview.slotTransitionActive = false;
    }
    releaseVoicesForSlot (slotIndex);
}

void FronasmaskinenAudioProcessor::setSelectedSlotTrim (double startTrimSeconds, double endTrimSeconds)
{
    const juce::ScopedLock lock (dataLock);
    if (selectedSlot < 0 || selectedSlot >= slotCount)
        return;

    auto& slot = slots[(size_t) selectedSlot];
    if (! slot.filled)
        return;

    const auto duration = getSampleDurationSeconds();
    if (duration <= minLoopSeconds)
        return;

    constexpr double trimChangeEpsilon = 0.0000001;
    const auto startChanged = std::abs (startTrimSeconds - slot.startTrimSeconds) > trimChangeEpsilon;
    const auto endChanged = std::abs (endTrimSeconds - slot.endTrimSeconds) > trimChangeEpsilon;

    if (startChanged && ! endChanged)
    {
        const auto endSeconds = clampDouble (slot.baseEndSeconds + slot.endTrimSeconds, minLoopSeconds, duration);
        const auto startSeconds = clampDouble (slot.baseStartSeconds + startTrimSeconds, 0.0, endSeconds - minLoopSeconds);
        slot.startTrimSeconds = startSeconds - slot.baseStartSeconds;
    }
    else if (endChanged && ! startChanged)
    {
        const auto startSeconds = clampDouble (slot.baseStartSeconds + slot.startTrimSeconds, 0.0, duration - minLoopSeconds);
        const auto endSeconds = clampDouble (slot.baseEndSeconds + endTrimSeconds, startSeconds + minLoopSeconds, duration);
        slot.endTrimSeconds = endSeconds - slot.baseEndSeconds;
    }
    else
    {
        const auto startSeconds = clampDouble (slot.baseStartSeconds + startTrimSeconds, 0.0, duration - minLoopSeconds);
        const auto endSeconds = clampDouble (slot.baseEndSeconds + endTrimSeconds, startSeconds + minLoopSeconds, duration);
        slot.startTrimSeconds = startSeconds - slot.baseStartSeconds;
        slot.endTrimSeconds = endSeconds - slot.baseEndSeconds;
    }

    activateLoopFromSelectedSlot();
}

void FronasmaskinenAudioProcessor::setSelectedSlotFadeSeconds (double fadeSecondsToUse)
{
    const juce::ScopedLock lock (dataLock);
    if (selectedSlot < 0 || selectedSlot >= slotCount)
        return;

    auto& slot = slots[(size_t) selectedSlot];
    if (! slot.filled)
        return;

    slot.fadeSeconds = clampDouble (fadeSecondsToUse, minFadeSeconds, maxFadeSeconds);
    activateLoopFromSelectedSlot();
}

void FronasmaskinenAudioProcessor::setSelectedSlotGainDb (float gainDb)
{
    const juce::ScopedLock lock (dataLock);
    if (selectedSlot < 0 || selectedSlot >= slotCount)
        return;

    auto& slot = slots[(size_t) selectedSlot];
    if (slot.filled)
    {
        slot.gainDb = juce::jlimit (-24.0f, 6.0f, gainDb);
        activateLoopFromSelectedSlot();
    }
}

void FronasmaskinenAudioProcessor::setSelectedSlotAdsr (double attackSeconds,
                                                        double decaySeconds,
                                                        float sustainLevel,
                                                        double releaseSeconds)
{
    const juce::ScopedLock lock (dataLock);
    if (selectedSlot < 0 || selectedSlot >= slotCount)
        return;

    auto& slot = slots[(size_t) selectedSlot];
    if (! slot.filled)
        return;

    slot.attackSeconds = clampDouble (attackSeconds, minAdsrSeconds, maxAdsrSeconds);
    slot.decaySeconds = clampDouble (decaySeconds, minAdsrSeconds, maxAdsrSeconds);
    slot.sustainLevel = juce::jlimit (0.0f, 1.0f, sustainLevel);
    slot.releaseSeconds = clampDouble (releaseSeconds, minAdsrSeconds, maxAdsrSeconds);
}

int FronasmaskinenAudioProcessor::getSelectedSlotIndex() const
{
    const juce::ScopedLock lock (dataLock);
    return selectedSlot;
}

FronasmaskinenAudioProcessor::Slot FronasmaskinenAudioProcessor::getSlot (int slotIndex) const
{
    const juce::ScopedLock lock (dataLock);
    if (slotIndex < 0 || slotIndex >= slotCount)
        return {};

    return slots[(size_t) slotIndex];
}

juce::String FronasmaskinenAudioProcessor::describeSlot (int slotIndex) const
{
    const auto slot = getSlot (slotIndex);
    if (! slot.filled)
        return "empty";

    return juce::String (slot.baseStartSeconds, 3) + "s - " + juce::String (slot.baseEndSeconds, 3) + "s";
}

void FronasmaskinenAudioProcessor::auditionSelectedSlot()
{
    const juce::ScopedLock lock (dataLock);
    if (selectedSlot < 0 || selectedSlot >= slotCount)
        return;

    if (! slots[(size_t) selectedSlot].filled || sampleBuffer.getNumSamples() == 0)
        return;

    lastTriggeredSlot.store (selectedSlot);
    startVoice (selectedSlot, baseNote + selectedSlot, 1.0f);
}

void FronasmaskinenAudioProcessor::stopAudition()
{
    const juce::ScopedLock lock (dataLock);
    releaseAllVoices();
}

int FronasmaskinenAudioProcessor::getLastMidiNote() const
{
    return lastMidiNote.load();
}

int FronasmaskinenAudioProcessor::getLastTriggeredSlot() const
{
    return lastTriggeredSlot.load();
}

int FronasmaskinenAudioProcessor::getActiveVoiceCount() const
{
    const juce::ScopedLock lock (dataLock);
    return (int) std::count_if (voices.begin(), voices.end(), [] (const Voice& voice) { return voice.active; });
}

bool FronasmaskinenAudioProcessor::hostTransportIsStopped() const
{
    const auto* playHead = getPlayHead();
    if (playHead == nullptr)
        return false;

    const auto position = playHead->getPosition();
    if (! position.hasValue())
        return false;

    return ! position->getIsPlaying();
}

std::pair<double, double> FronasmaskinenAudioProcessor::effectiveSelectedSlotBoundsSeconds() const
{
    if (selectedSlot < 0 || selectedSlot >= slotCount || ! slots[(size_t) selectedSlot].filled)
        return { -1.0, -1.0 };

    const auto bounds = effectiveSlotBoundsSamples (slots[(size_t) selectedSlot]);
    return { bounds.first / sampleBufferRate, bounds.second / sampleBufferRate };
}

void FronasmaskinenAudioProcessor::beginPreviewSlotTransition (int nextSlotIndex)
{
    if (! preview.playing || ! preview.loopActive || nextSlotIndex < 0 || nextSlotIndex >= slotCount)
        return;

    if (selectedSlot == nextSlotIndex || selectedSlot < 0 || selectedSlot >= slotCount)
        return;

    const auto& currentSlot = slots[(size_t) selectedSlot];
    const auto& nextSlot = slots[(size_t) nextSlotIndex];
    if (! currentSlot.filled || ! nextSlot.filled)
        return;

    const auto currentBounds = effectiveSlotBoundsSamples (currentSlot);
    if (currentBounds.second - currentBounds.first < 1.0)
        return;

    preview.slotTransitionActive = true;
    preview.transitionPositionSamples = preview.positionSamples;
    preview.transitionLoopStartSamples = currentBounds.first;
    preview.transitionLoopEndSamples = currentBounds.second;
    preview.transitionGain = dbToGain (currentSlot.gainDb);
    preview.transitionSample = 0;
    preview.transitionSamples = slotFadeSamplesForHost (nextSlot);
}

bool FronasmaskinenAudioProcessor::swapFilledSlots (int slotIndex, int targetSlotIndex)
{
    if (slotIndex < 0 || slotIndex >= slotCount || targetSlotIndex < 0 || targetSlotIndex >= slotCount)
        return false;

    const juce::ScopedLock lock (dataLock);
    auto& slot = slots[(size_t) slotIndex];
    auto& targetSlot = slots[(size_t) targetSlotIndex];
    if (! slot.filled || ! targetSlot.filled)
        return false;

    std::swap (slot, targetSlot);

    if (selectedSlot == slotIndex)
        selectedSlot = targetSlotIndex;
    else if (selectedSlot == targetSlotIndex)
        selectedSlot = slotIndex;

    for (auto& voice : voices)
    {
        if (voice.slotIndex == slotIndex)
            voice.slotIndex = targetSlotIndex;
        else if (voice.slotIndex == targetSlotIndex)
            voice.slotIndex = slotIndex;
    }

    if (lastTriggeredSlot.load() == slotIndex)
        lastTriggeredSlot.store (targetSlotIndex);
    else if (lastTriggeredSlot.load() == targetSlotIndex)
        lastTriggeredSlot.store (slotIndex);

    if (selectedSlot >= 0)
    {
        selectionStartSeconds = slots[(size_t) selectedSlot].baseStartSeconds;
        selectionEndSeconds = slots[(size_t) selectedSlot].baseEndSeconds;
        activateLoopFromSelectedSlot();
    }

    return true;
}

std::pair<double, double> FronasmaskinenAudioProcessor::findSmoothLoopBoundsSeconds (double startSeconds, double endSeconds) const
{
    const auto duration = getSampleDurationSeconds();
    if (duration <= minLoopSeconds || sampleBuffer.getNumSamples() <= 2)
        return { startSeconds, endSeconds };

    const auto boundedStart = clampDouble (startSeconds, 0.0, duration - minLoopSeconds);
    const auto boundedEnd = clampDouble (endSeconds, boundedStart + minLoopSeconds, duration);
    const auto originalStartSample = boundedStart * sampleBufferRate;
    const auto originalEndSample = boundedEnd * sampleBufferRate;
    const auto originalScore = loopEndpointMatchScore (originalStartSample, originalEndSample);

    auto bestStart = boundedStart;
    auto bestEnd = boundedEnd;
    auto bestScore = originalScore;

    const auto minStart = clampDouble (boundedStart - maxLoopEndpointAdjustSeconds, 0.0, duration - minLoopSeconds);
    const auto maxStart = clampDouble (boundedStart + maxLoopEndpointAdjustSeconds, 0.0, duration - minLoopSeconds);
    const auto minEnd = clampDouble (boundedEnd - maxLoopEndpointAdjustSeconds, minStart + minLoopSeconds, duration);
    const auto maxEnd = clampDouble (boundedEnd + maxLoopEndpointAdjustSeconds, minStart + minLoopSeconds, duration);
    const auto step = juce::jmax (loopEndpointSearchStepSeconds, 1.0 / sampleBufferRate);

    for (auto candidateStart = minStart; candidateStart <= maxStart; candidateStart += step)
    {
        const auto candidateMinEnd = juce::jmax (minEnd, candidateStart + minLoopSeconds);
        if (candidateMinEnd > maxEnd)
            continue;

        for (auto candidateEnd = candidateMinEnd; candidateEnd <= maxEnd; candidateEnd += step)
        {
            const auto startOffset = std::abs (candidateStart - boundedStart);
            const auto endOffset = std::abs (candidateEnd - boundedEnd);
            const auto intentPenalty = (startOffset + endOffset) * 0.015;
            const auto lengthPenalty = std::abs ((candidateEnd - candidateStart) - (boundedEnd - boundedStart)) * 0.010;
            const auto score = loopEndpointMatchScore (candidateStart * sampleBufferRate, candidateEnd * sampleBufferRate)
                             + intentPenalty
                             + lengthPenalty;

            if (score < bestScore)
            {
                bestScore = score;
                bestStart = candidateStart;
                bestEnd = candidateEnd;
            }
        }
    }

    return { bestStart, bestEnd };
}

double FronasmaskinenAudioProcessor::loopEndpointMatchScore (double startSample, double endSample) const
{
    const auto sourceChannels = sampleBuffer.getNumChannels();
    if (sourceChannels <= 0)
        return 0.0;

    auto score = 0.0;
    for (int channel = 0; channel < sourceChannels; ++channel)
    {
        const auto startValue = readSample (channel, startSample);
        const auto endValue = readSample (channel, endSample);
        const auto startSlope = readSample (channel, startSample + 1.0) - readSample (channel, startSample - 1.0);
        const auto endSlope = readSample (channel, endSample + 1.0) - readSample (channel, endSample - 1.0);

        score += std::abs ((double) startValue - (double) endValue);
        score += std::abs ((double) startSlope - (double) endSlope) * 0.5;
        score += (std::abs ((double) startValue) + std::abs ((double) endValue)) * 0.05;
    }

    return score / (double) sourceChannels;
}

bool FronasmaskinenAudioProcessor::saveLoopToNextSlot (double startSeconds, double endSeconds, bool smoothLoopBounds)
{
    const auto nextSlot = std::find_if (slots.begin(), slots.end(), [] (const Slot& slot) { return ! slot.filled; });
    if (nextSlot == slots.end())
        return false;

    const auto bounds = smoothLoopBounds ? findSmoothLoopBoundsSeconds (startSeconds, endSeconds)
                                         : std::pair<double, double> { startSeconds, endSeconds };

    auto& slot = *nextSlot;
    slot.filled = true;
    slot.baseStartSeconds = bounds.first;
    slot.baseEndSeconds = bounds.second;
    slot.startTrimSeconds = 0.0;
    slot.endTrimSeconds = 0.0;
    slot.fadeSeconds = defaultFadeSeconds;
    slot.gainDb = 0.0f;
    slot.attackSeconds = defaultAttackSeconds;
    slot.decaySeconds = defaultDecaySeconds;
    slot.sustainLevel = defaultSustainLevel;
    slot.releaseSeconds = defaultReleaseSeconds;
    selectedSlot = (int) std::distance (slots.begin(), nextSlot);
    selectionStartSeconds = bounds.first;
    selectionEndSeconds = bounds.second;
    return true;
}

void FronasmaskinenAudioProcessor::activateLoopFromSelectedSlot()
{
    const auto bounds = effectiveSelectedSlotBoundsSeconds();
    if (bounds.first < 0.0 || bounds.second <= bounds.first)
        return;

    preview.loopStartSeconds = bounds.first;
    preview.loopEndSeconds = bounds.second;
    preview.loopActive = true;
    preview.pendingLoopStartActive = false;
    preview.seamCrossfadeActive = false;
}

void FronasmaskinenAudioProcessor::rebuildWaveformThumbnail()
{
    constexpr int thumbnailSize = 2048;
    waveformThumbnail.assign (thumbnailSize, 0.0f);

    if (sampleBuffer.getNumSamples() == 0)
        return;

    const auto sampleCount = sampleBuffer.getNumSamples();
    const auto* data = sampleBuffer.getReadPointer (0);

    for (int x = 0; x < thumbnailSize; ++x)
    {
        const auto start = (int) std::floor ((double) x / (double) thumbnailSize * (double) sampleCount);
        const auto end = (int) std::floor ((double) (x + 1) / (double) thumbnailSize * (double) sampleCount);
        auto peak = 0.0f;

        for (int i = start; i < juce::jmax (start + 1, end) && i < sampleCount; ++i)
            peak = std::max (peak, std::abs (data[i]));

        waveformThumbnail[(size_t) x] = peak;
    }
}


float FronasmaskinenAudioProcessor::selectedSlotGainDb() const
{
    if (selectedSlot < 0 || selectedSlot >= slotCount)
        return 0.0f;

    return slots[(size_t) selectedSlot].gainDb;
}

void FronasmaskinenAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto root = std::make_unique<juce::XmlElement> ("FronasmaskinenState");
    root->setAttribute ("file", getLoadedFilePath());
    root->setAttribute ("selectionStart", selectionStartSeconds);
    root->setAttribute ("selectionEnd", selectionEndSeconds);
    root->setAttribute ("selectedSlot", selectedSlot);

    const juce::ScopedLock lock (dataLock);
    for (int i = 0; i < slotCount; ++i)
    {
        const auto& slot = slots[(size_t) i];
        auto* child = root->createNewChildElement ("Slot");
        child->setAttribute ("index", i);
        child->setAttribute ("filled", slot.filled);
        child->setAttribute ("baseStart", slot.baseStartSeconds);
        child->setAttribute ("baseEnd", slot.baseEndSeconds);
        child->setAttribute ("startTrim", slot.startTrimSeconds);
        child->setAttribute ("endTrim", slot.endTrimSeconds);
        child->setAttribute ("fade", slot.fadeSeconds);
        child->setAttribute ("gainDb", (double) slot.gainDb);
        child->setAttribute ("attack", slot.attackSeconds);
        child->setAttribute ("decay", slot.decaySeconds);
        child->setAttribute ("sustain", (double) slot.sustainLevel);
        child->setAttribute ("release", slot.releaseSeconds);
    }

    copyXmlToBinary (*root, destData);
}

void FronasmaskinenAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> root (getXmlFromBinary (data, sizeInBytes));
    if (root == nullptr || ! root->hasTagName ("FronasmaskinenState"))
        return;

    const auto file = juce::File (root->getStringAttribute ("file"));
    if (file.existsAsFile())
        loadAudioFile (file);

    const juce::ScopedLock lock (dataLock);
    selectionStartSeconds = root->getDoubleAttribute ("selectionStart", selectionStartSeconds);
    selectionEndSeconds = root->getDoubleAttribute ("selectionEnd", selectionEndSeconds);
    selectedSlot = root->getIntAttribute ("selectedSlot", -1);

    for (auto* child : root->getChildIterator())
    {
        if (! child->hasTagName ("Slot"))
            continue;

        const auto index = child->getIntAttribute ("index", -1);
        if (index < 0 || index >= slotCount)
            continue;

        auto& slot = slots[(size_t) index];
        slot.filled = child->getBoolAttribute ("filled", false);
        slot.baseStartSeconds = child->getDoubleAttribute ("baseStart", 0.0);
        slot.baseEndSeconds = child->getDoubleAttribute ("baseEnd", defaultLoopSeconds);
        slot.startTrimSeconds = child->getDoubleAttribute ("startTrim", 0.0);
        slot.endTrimSeconds = child->getDoubleAttribute ("endTrim", 0.0);
        slot.fadeSeconds = child->getDoubleAttribute ("fade", defaultFadeSeconds);
        slot.gainDb = (float) child->getDoubleAttribute ("gainDb", 0.0);
        slot.attackSeconds = child->getDoubleAttribute ("attack", slot.fadeSeconds);
        slot.decaySeconds = child->getDoubleAttribute ("decay", defaultDecaySeconds);
        slot.sustainLevel = (float) child->getDoubleAttribute ("sustain", defaultSustainLevel);
        slot.releaseSeconds = child->getDoubleAttribute ("release", slot.fadeSeconds);
    }

    if (selectedSlot >= 0 && selectedSlot < slotCount && slots[(size_t) selectedSlot].filled)
        activateLoopFromSelectedSlot();
    else
        selectedSlot = -1;
}

juce::AudioProcessorEditor* FronasmaskinenAudioProcessor::createEditor()
{
    return new FronasmaskinenAudioProcessorEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new FronasmaskinenAudioProcessor();
}
