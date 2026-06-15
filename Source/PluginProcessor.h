#pragma once

#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <vector>

#ifndef JucePlugin_Name
#define JucePlugin_Name "Fronasmaskinen"
#endif

class FronasmaskinenAudioProcessor final : public juce::AudioProcessor
{
public:
    static constexpr int slotCount = 10;
    static constexpr int baseNote = 36; // Logic labels MIDI note 36 as C1.
    static constexpr int sequenceOutputVolumeCc = 20;

    struct Slot
    {
        bool filled = false;
        double baseStartSeconds = 0.0;
        double baseEndSeconds = 0.25;
        double startTrimSeconds = 0.0;
        double endTrimSeconds = 0.0;
        double fadeSeconds = 0.012;
        float gainDb = 0.0f;
    };

    FronasmaskinenAudioProcessor();
    ~FronasmaskinenAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.05; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    bool loadAudioFile (const juce::File& file);
    bool hasSample() const;
    juce::String getLoadedFilePath() const;
    int getSampleLengthSamples() const;
    double getSampleDurationSeconds() const;
    double getSelectionStartSeconds() const;
    double getSelectionEndSeconds() const;
    void setSelection (double startSeconds, double endSeconds);

    void playPreview();
    void pausePreview();
    void togglePreview();
    bool isPreviewPlaying() const;
    double getPreviewPositionSeconds() const;
    void seekPreview (double positionSeconds);
    bool setLoopPointAtPreviewPosition();
    void releasePreviewLoop();
    bool randomizePreviewLoopStart();
    bool hasPreviewLoop() const;
    bool hasPendingLoopStart() const;
    double getPendingLoopStartSeconds() const;
    double getPreviewLoopStartSeconds() const;
    double getPreviewLoopEndSeconds() const;
    void getWaveformThumbnail (std::vector<float>& peaks) const;

    bool saveSelectionToSlot (int slotIndex);
    bool selectSlot (int slotIndex);
    bool moveSlotLeft (int slotIndex);
    bool moveSlotRight (int slotIndex);
    void clearSlot (int slotIndex);
    void setSelectedSlotTrim (double startTrimSeconds, double endTrimSeconds);
    void setSelectedSlotFadeSeconds (double fadeSeconds);
    void setSelectedSlotGainDb (float gainDb);
    int getSelectedSlotIndex() const;
    Slot getSlot (int slotIndex) const;
    juce::String describeSlot (int slotIndex) const;
    void auditionSelectedSlot();
    void stopAudition();
    int getLastMidiNote() const;
    int getLastTriggeredSlot() const;
    int getActiveVoiceCount() const;

private:
    struct Voice
    {
        bool active = false;
        bool releasing = false;
        int slotIndex = -1;
        int noteNumber = -1;
        double positionSamples = 0.0;
        float velocityGain = 1.0f;
        float envelope = 0.0f;
        float releaseStartEnvelope = 0.0f;
        int attackSample = 0;
        int releaseSample = 0;
        bool seamCrossfadeActive = false;
    };

    struct PreviewState
    {
        bool playing = false;
        bool loopActive = false;
        bool pendingLoopStartActive = false;
        double pendingLoopStartSeconds = 0.0;
        double loopStartSeconds = 0.0;
        double loopEndSeconds = 0.0;
        double positionSamples = 0.0;
        float envelope = 0.0f;
        bool releasing = false;
        float releaseStartEnvelope = 0.0f;
        int attackSample = 0;
        int releaseSample = 0;
        bool seamCrossfadeActive = false;
        bool slotTransitionActive = false;
        double transitionPositionSamples = 0.0;
        double transitionLoopStartSamples = 0.0;
        double transitionLoopEndSamples = 0.0;
        double transitionGain = 1.0;
        int transitionSample = 0;
        int transitionSamples = 1;
    };

    juce::AudioFormatManager formatManager;
    mutable juce::CriticalSection dataLock;
    juce::AudioBuffer<float> sampleBuffer;
    std::vector<float> waveformThumbnail;
    double sampleBufferRate = 44100.0;
    juce::File loadedFile;
    std::array<Slot, slotCount> slots;
    std::array<Voice, 4> voices;
    PreviewState preview;
    int selectedSlot = -1;
    double selectionStartSeconds = 0.0;
    double selectionEndSeconds = 0.25;
    double hostSampleRate = 44100.0;
    int fadeSamples = 441;
    int crossfadeSamples = 441;
    int nextVoiceIndex = 0;
    std::atomic<int> lastMidiNote { -1 };
    std::atomic<int> lastTriggeredSlot { -1 };
    float sequenceOutputCurrentGain = 1.0f;
    float sequenceOutputTargetGain = 1.0f;

    void handleMidi (const juce::MidiBuffer& midiMessages);
    void noteOn (int noteNumber, float velocity);
    void noteOff (int noteNumber);
    void startVoice (int slotIndex, int noteNumber, float velocity);
    void releaseVoicesForSlot (int slotIndex);
    void releaseAllVoices();
    void renderVoice (Voice& voice,
                      juce::AudioBuffer<float>& output,
                      int startSample,
                      int numSamples,
                      float outputGainStart,
                      float outputGainEnd,
                      int outputGainRampSamples);
    void renderPreview (juce::AudioBuffer<float>& output,
                        int startSample,
                        int numSamples,
                        float outputGainStart,
                        float outputGainEnd,
                        int outputGainRampSamples);
    float readSample (int channel, double absoluteSample) const;
    int slotFadeSamplesForHost (const Slot& slot) const;
    int slotFadeSamplesForSource (const Slot& slot, double loopLengthSamples) const;
    std::pair<double, double> effectiveSlotBoundsSamples (const Slot& slot) const;
    std::pair<double, double> effectiveSelectedSlotBoundsSeconds() const;
    bool hostTransportIsStopped() const;
    void beginPreviewSlotTransition (int nextSlotIndex);
    bool swapFilledSlots (int slotIndex, int targetSlotIndex);
    std::pair<double, double> findSmoothLoopBoundsSeconds (double startSeconds, double endSeconds) const;
    double loopEndpointMatchScore (double startSample, double endSample) const;
    bool saveLoopToNextSlot (double startSeconds, double endSeconds, bool smoothLoopBounds);
    void activateLoopFromSelectedSlot();
    void rebuildWaveformThumbnail();
    float selectedSlotGainDb() const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FronasmaskinenAudioProcessor)
};
