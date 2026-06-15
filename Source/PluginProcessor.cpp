#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
constexpr double minLoopSeconds = 0.02;
constexpr double defaultLoopSeconds = 0.25;
constexpr double defaultFadeSeconds = 0.012;

double clampDouble (double value, double low, double high)
{
    return std::min (std::max (value, low), high);
}

float dbToGain (float db)
{
    return juce::Decibels::decibelsToGain (db, -80.0f);
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

    handleMidi (midiMessages);

    const juce::ScopedTryLock lock (dataLock);
    if (! lock.isLocked())
        return;

    renderPreview (buffer, 0, buffer.getNumSamples());

    for (auto& voice : voices)
        if (voice.active)
            renderVoice (voice, buffer, 0, buffer.getNumSamples());
}

void FronasmaskinenAudioProcessor::handleMidi (const juce::MidiBuffer& midiMessages)
{
    for (const auto metadata : midiMessages)
    {
        const auto message = metadata.getMessage();

        if (message.isNoteOn())
            noteOn (message.getNoteNumber(), message.getVelocity());
        else if (message.isNoteOff())
            noteOff (message.getNoteNumber());
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

    selectedSlot = slotIndex;
    activateLoopFromSelectedSlot();
    preview.positionSamples = preview.loopStartSeconds * sampleBufferRate;
    lastTriggeredSlot.store (slotIndex);
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

void FronasmaskinenAudioProcessor::renderVoice (Voice& voice, juce::AudioBuffer<float>& output, int startSample, int numSamples)
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
    const auto seamFade = juce::jlimit (1, (int) std::floor (loopLength * 0.45), crossfadeSamples);
    const auto gain = dbToGain (slot.gainDb) * voice.velocityGain;

    for (int sample = 0; sample < numSamples; ++sample)
    {
        if (! voice.active)
            break;

        if (voice.attackSample < fadeSamples && ! voice.releasing)
        {
            voice.envelope = (float) voice.attackSample / (float) fadeSamples;
            ++voice.attackSample;
        }
        else if (! voice.releasing)
        {
            voice.envelope = 1.0f;
        }

        if (voice.releasing)
        {
            const auto t = (float) voice.releaseSample / (float) fadeSamples;
            voice.envelope = voice.releaseStartEnvelope * juce::jlimit (0.0f, 1.0f, 1.0f - t);
            ++voice.releaseSample;

            if (voice.releaseSample >= fadeSamples)
            {
                voice.active = false;
                break;
            }
        }

        while (voice.positionSamples >= end)
            voice.positionSamples -= loopLength;

        const auto distanceToEnd = end - voice.positionSamples;
        const auto xfade = distanceToEnd < seamFade ? 1.0 - (distanceToEnd / (double) seamFade) : 0.0;
        const auto wrappedPosition = start + (double) seamFade - distanceToEnd;

        for (int channel = 0; channel < channels; ++channel)
        {
            const auto sourceChannel = sourceChannels == 1 ? 0 : juce::jmin (channel, sourceChannels - 1);
            auto value = readSample (sourceChannel, voice.positionSamples);

            if (xfade > 0.0)
            {
                const auto wrapped = readSample (sourceChannel, wrappedPosition);
                value = (float) ((value * (1.0 - xfade)) + (wrapped * xfade));
            }

            output.addSample (channel, startSample + sample, value * gain * voice.envelope);
        }

        voice.positionSamples += sampleBufferRate / hostSampleRate;
    }
}

void FronasmaskinenAudioProcessor::renderPreview (juce::AudioBuffer<float>& output, int startSample, int numSamples)
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
    const auto seamFade = canLoop ? juce::jlimit (1, (int) std::floor (loopLength * 0.45), crossfadeSamples) : 1;
    auto gain = 1.0f;
    if (canLoop && selectedSlot >= 0 && selectedSlot < slotCount && slots[(size_t) selectedSlot].filled)
        gain = dbToGain (slots[(size_t) selectedSlot].gainDb);

    for (int sample = 0; sample < numSamples; ++sample)
    {
        if (! preview.playing)
            break;

        if (preview.attackSample < fadeSamples && ! preview.releasing)
        {
            preview.envelope = (float) preview.attackSample / (float) fadeSamples;
            ++preview.attackSample;
        }
        else if (! preview.releasing)
        {
            preview.envelope = 1.0f;
        }

        if (preview.releasing)
        {
            const auto t = (float) preview.releaseSample / (float) fadeSamples;
            preview.envelope = preview.releaseStartEnvelope * juce::jlimit (0.0f, 1.0f, 1.0f - t);
            ++preview.releaseSample;

            if (preview.releaseSample >= fadeSamples)
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
                preview.positionSamples -= loopLength;
        }
        else if (preview.positionSamples >= sampleCount - 1.0)
        {
            preview.playing = false;
            preview.positionSamples = 0.0;
            preview.envelope = 0.0f;
            break;
        }

        auto xfade = 0.0;
        auto wrappedPosition = loopStart;
        if (canLoop)
        {
            const auto distanceToEnd = loopEnd - preview.positionSamples;
            xfade = distanceToEnd < seamFade ? 1.0 - (distanceToEnd / (double) seamFade) : 0.0;
            wrappedPosition = loopStart + (double) seamFade - distanceToEnd;
        }

        for (int channel = 0; channel < channels; ++channel)
        {
            const auto sourceChannel = sourceChannels == 1 ? 0 : juce::jmin (channel, sourceChannels - 1);
            auto value = readSample (sourceChannel, preview.positionSamples);

            if (xfade > 0.0)
            {
                const auto wrapped = readSample (sourceChannel, wrappedPosition);
                value = (float) ((value * (1.0 - xfade)) + (wrapped * xfade));
            }

            output.addSample (channel, startSample + sample, value * preview.envelope * gain);
        }

        preview.positionSamples += sampleBufferRate / hostSampleRate;
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
    releaseAllVoices();
    preview = {};
    sampleBuffer = std::move (nextBuffer);
    sampleBufferRate = reader->sampleRate;
    rebuildWaveformThumbnail();
    loadedFile = file;
    selectionStartSeconds = 0.0;
    selectionEndSeconds = std::min (defaultLoopSeconds, getSampleDurationSeconds());
    selectedSlot = -1;
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
    if (! saveLoopToNextSlot (start, end))
        return false;

    activateLoopFromSelectedSlot();
    preview.positionSamples = start * sampleBufferRate;
    return true;
}

void FronasmaskinenAudioProcessor::releasePreviewLoop()
{
    const juce::ScopedLock lock (dataLock);
    preview.loopActive = false;
    preview.pendingLoopStartActive = false;
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
    if (! saveLoopToNextSlot (start, end))
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
    slot.gainDb = 0.0f;
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
        selectedSlot = -1;
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

    slot.startTrimSeconds = startTrimSeconds;
    slot.endTrimSeconds = endTrimSeconds;
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

std::pair<double, double> FronasmaskinenAudioProcessor::effectiveSelectedSlotBoundsSeconds() const
{
    if (selectedSlot < 0 || selectedSlot >= slotCount || ! slots[(size_t) selectedSlot].filled)
        return { -1.0, -1.0 };

    const auto bounds = effectiveSlotBoundsSamples (slots[(size_t) selectedSlot]);
    return { bounds.first / sampleBufferRate, bounds.second / sampleBufferRate };
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

bool FronasmaskinenAudioProcessor::saveLoopToNextSlot (double startSeconds, double endSeconds)
{
    const auto nextSlot = std::find_if (slots.begin(), slots.end(), [] (const Slot& slot) { return ! slot.filled; });
    if (nextSlot == slots.end())
        return false;

    auto& slot = *nextSlot;
    slot.filled = true;
    slot.baseStartSeconds = startSeconds;
    slot.baseEndSeconds = endSeconds;
    slot.startTrimSeconds = 0.0;
    slot.endTrimSeconds = 0.0;
    slot.gainDb = 0.0f;
    selectedSlot = (int) std::distance (slots.begin(), nextSlot);
    selectionStartSeconds = startSeconds;
    selectionEndSeconds = endSeconds;
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
        child->setAttribute ("gainDb", (double) slot.gainDb);
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
        slot.gainDb = (float) child->getDoubleAttribute ("gainDb", 0.0);
    }
}

juce::AudioProcessorEditor* FronasmaskinenAudioProcessor::createEditor()
{
    return new FronasmaskinenAudioProcessorEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new FronasmaskinenAudioProcessor();
}
