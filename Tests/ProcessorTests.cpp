#include <JuceHeader.h>
#include "../Source/PluginProcessor.h"

namespace
{
constexpr double epsilon = 0.0001;

struct TestFailure
{
    juce::String message;
};

void expect (bool condition, const juce::String& message)
{
    if (! condition)
        throw TestFailure { message };
}

void expectNear (double actual, double expected, double tolerance, const juce::String& message)
{
    if (std::abs (actual - expected) > tolerance)
        throw TestFailure { message + " expected " + juce::String (expected, 6) + " got " + juce::String (actual, 6) };
}

juce::File createTestSample()
{
    const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
        .getNonexistentChildFile ("fronasmaskinen-test-sample", ".wav");

    constexpr double sampleRate = 44100.0;
    constexpr int numSamples = 44100;
    juce::AudioBuffer<float> buffer (1, numSamples);

    for (int i = 0; i < numSamples; ++i)
    {
        const auto phase = (float) i / (float) numSamples;
        buffer.setSample (0, i, std::sin (phase * juce::MathConstants<float>::twoPi * 12.0f) * 0.8f);
    }

    juce::WavAudioFormat format;
    std::unique_ptr<juce::FileOutputStream> stream (file.createOutputStream());
    expect (stream != nullptr, "Could not create WAV fixture stream");

    std::unique_ptr<juce::AudioFormatWriter> writer (format.createWriterFor (stream.get(), sampleRate, 1, 24, {}, 0));
    expect (writer != nullptr, "Could not create WAV fixture writer");
    stream.release();

    expect (writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples()), "Could not write WAV fixture");
    return file;
}

void processMidiNote (FronasmaskinenAudioProcessor& processor, int noteNumber, bool noteOn)
{
    juce::AudioBuffer<float> buffer (2, 256);
    juce::MidiBuffer midi;
    midi.addEvent (noteOn ? juce::MidiMessage::noteOn (1, noteNumber, (juce::uint8) 100)
                          : juce::MidiMessage::noteOff (1, noteNumber),
                   0);
    processor.processBlock (buffer, midi);
}

void seekAndSetLoopPoint (FronasmaskinenAudioProcessor& processor, double seconds)
{
    processor.seekPreview (seconds);
    expect (processor.setLoopPointAtPreviewPosition(), "Could not set loop point at " + juce::String (seconds, 3));
}

void testSampleLoadingAndWaveform()
{
    auto processor = FronasmaskinenAudioProcessor();
    const auto sample = createTestSample();

    expect (processor.loadAudioFile (sample), "Load sample button path should load a WAV file");
    expect (processor.hasSample(), "Processor should report a loaded sample");
    expect (processor.getLoadedFilePath() == sample.getFullPathName(), "Loaded file path should be stored");
    expect (processor.getSampleLengthSamples() == 44100, "Sample length should match fixture");
    expectNear (processor.getSampleDurationSeconds(), 1.0, epsilon, "Sample duration should match fixture");

    std::vector<float> peaks;
    processor.getWaveformThumbnail (peaks);
    expect (peaks.size() == 2048, "Waveform thumbnail should be built after loading");
    expect (std::any_of (peaks.begin(), peaks.end(), [] (float value) { return value > 0.1f; }),
            "Waveform thumbnail should contain non-silent peaks");

    sample.deleteFile();
}

void testPreviewSeekPlayAndLoopSlotCreation()
{
    auto processor = FronasmaskinenAudioProcessor();
    const auto sample = createTestSample();
    expect (processor.loadAudioFile (sample), "Sample should load");

    processor.seekPreview (0.42);
    expectNear (processor.getPreviewPositionSeconds(), 0.42, epsilon, "Waveform click should seek preview position");

    processor.playPreview();
    expect (processor.isPreviewPlaying(), "Play should start preview playback");

    seekAndSetLoopPoint (processor, 0.10);
    expect (processor.hasPendingLoopStart(), "First loop click should arm pending loop start");
    expectNear (processor.getPendingLoopStartSeconds(), 0.10, epsilon, "Pending loop start should use playhead position");
    expect (processor.getSelectedSlotIndex() == -1, "Pending loop start should leave no active slot");

    seekAndSetLoopPoint (processor, 0.18);
    expect (! processor.hasPendingLoopStart(), "Second loop click should clear pending loop start");
    expect (processor.hasPreviewLoop(), "Second loop click should activate preview loop");
    expect (processor.getSelectedSlotIndex() == 0, "Created loop should select the next free slot");
    expectNear (processor.getPreviewLoopStartSeconds(), 0.10, epsilon, "Preview loop start should match slot start");
    expectNear (processor.getPreviewLoopEndSeconds(), 0.18, epsilon, "Preview loop end should match slot end");

    const auto slot = processor.getSlot (0);
    expect (slot.filled, "Created loop should fill S1");
    expectNear (slot.baseStartSeconds, 0.10, epsilon, "S1 should store base start");
    expectNear (slot.baseEndSeconds, 0.18, epsilon, "S1 should store base end");

    sample.deleteFile();
}

void testSlotTrimAndGainPersistence()
{
    auto processor = FronasmaskinenAudioProcessor();
    const auto sample = createTestSample();
    expect (processor.loadAudioFile (sample), "Sample should load");

    processor.setSelection (0.10, 0.20);
    expect (processor.saveSelectionToSlot (0), "Should save selection to S1");
    processor.setSelection (0.30, 0.42);
    expect (processor.saveSelectionToSlot (1), "Should save selection to S2");

    expect (processor.selectSlot (0), "Should select S1");
    processor.setSelectedSlotTrim (0.011, -0.014);
    processor.setSelectedSlotGainDb (-7.5f);

    expect (processor.selectSlot (1), "Should select S2");
    processor.setSelectedSlotTrim (0.020, -0.010);
    processor.setSelectedSlotGainDb (3.0f);

    expect (processor.selectSlot (0), "Should return to S1");
    auto slot = processor.getSlot (0);
    expectNear (slot.startTrimSeconds, 0.011, epsilon, "S1 should persist start trim");
    expectNear (slot.endTrimSeconds, -0.014, epsilon, "S1 should persist end trim");
    expectNear (slot.gainDb, -7.5, epsilon, "S1 should persist gain");
    expectNear (processor.getPreviewLoopStartSeconds(), 0.111, epsilon, "Trim should update active loop start");
    expectNear (processor.getPreviewLoopEndSeconds(), 0.186, epsilon, "Trim should update active loop end");

    expect (processor.selectSlot (1), "Should return to S2");
    slot = processor.getSlot (1);
    expectNear (slot.startTrimSeconds, 0.020, epsilon, "S2 should persist start trim independently");
    expectNear (slot.endTrimSeconds, -0.010, epsilon, "S2 should persist end trim independently");
    expectNear (slot.gainDb, 3.0, epsilon, "S2 should persist gain independently");

    processor.setSelectedSlotGainDb (24.0f);
    expectNear (processor.getSlot (1).gainDb, 6.0, epsilon, "Slot gain should clamp to max");
    processor.setSelectedSlotGainDb (-99.0f);
    expectNear (processor.getSlot (1).gainDb, -24.0, epsilon, "Slot gain should clamp to min");

    sample.deleteFile();
}

void testReleaseAndRandomStart()
{
    auto processor = FronasmaskinenAudioProcessor();
    const auto sample = createTestSample();
    expect (processor.loadAudioFile (sample), "Sample should load");
    processor.playPreview();

    seekAndSetLoopPoint (processor, 0.20);
    seekAndSetLoopPoint (processor, 0.32);
    expect (processor.hasPreviewLoop(), "Loop should be active before release");
    expect (processor.isPreviewPlaying(), "Preview should be playing before release");

    processor.releasePreviewLoop();
    expect (! processor.hasPreviewLoop(), "Release should remove loop markers from preview");
    expect (! processor.hasPendingLoopStart(), "Release should clear pending loop state");
    expect (processor.isPreviewPlaying(), "Release should keep preview playback running");

    seekAndSetLoopPoint (processor, 0.40);
    seekAndSetLoopPoint (processor, 0.48);
    expect (processor.getSelectedSlotIndex() == 1, "New loop after release should fill the next slot");

    const auto previous = processor.getSlot (1);
    const auto previousLength = previous.baseEndSeconds - previous.baseStartSeconds;
    expect (processor.randomizePreviewLoopStart(), "Random start should create a new slot from the active loop");
    expect (processor.getSelectedSlotIndex() == 2, "Random start should select the new slot");
    const auto randomized = processor.getSlot (2);
    expect (randomized.filled, "Random start should fill the next free slot");
    expectNear (randomized.baseEndSeconds - randomized.baseStartSeconds, previousLength, epsilon,
                "Random start should preserve loop length");
    expect (processor.hasPreviewLoop(), "Random start should activate the new loop");

    sample.deleteFile();
}

void testSelectingSlotDuringPreviewMovesPlayheadIntoSlot()
{
    auto processor = FronasmaskinenAudioProcessor();
    const auto sample = createTestSample();
    expect (processor.loadAudioFile (sample), "Sample should load");
    processor.playPreview();

    seekAndSetLoopPoint (processor, 0.10);
    seekAndSetLoopPoint (processor, 0.18);
    processor.releasePreviewLoop();

    seekAndSetLoopPoint (processor, 0.30);
    seekAndSetLoopPoint (processor, 0.38);
    processor.releasePreviewLoop();

    seekAndSetLoopPoint (processor, 0.60);
    seekAndSetLoopPoint (processor, 0.68);
    expect (processor.getSelectedSlotIndex() == 2, "S3 should be active before selecting S1");
    expectNear (processor.getPreviewLoopStartSeconds(), 0.60, epsilon, "S3 loop should be active");

    expect (processor.selectSlot (0), "Clicking S1 while preview plays should select it");
    expect (processor.getSelectedSlotIndex() == 0, "S1 should become active");
    expectNear (processor.getPreviewLoopStartSeconds(), 0.10, epsilon, "S1 loop start should become active");
    expectNear (processor.getPreviewLoopEndSeconds(), 0.18, epsilon, "S1 loop end should become active");
    expectNear (processor.getPreviewPositionSeconds(), 0.10, epsilon, "Preview playhead should move to S1 start");

    sample.deleteFile();
}

void testSlotMouseAndMidiTriggers()
{
    auto processor = FronasmaskinenAudioProcessor();
    const auto sample = createTestSample();
    expect (processor.loadAudioFile (sample), "Sample should load");
    processor.prepareToPlay (44100.0, 256);

    for (int i = 0; i < FronasmaskinenAudioProcessor::slotCount; ++i)
    {
        const auto start = 0.05 + (double) i * 0.04;
        processor.setSelection (start, start + 0.03);
        expect (processor.saveSelectionToSlot (i), "Should save loop to slot " + juce::String (i + 1));
    }

    expect (processor.selectSlot (4), "Clicking S5 should select it");
    expect (processor.getSelectedSlotIndex() == 4, "S5 should become active after mouse selection");
    expect (processor.hasPreviewLoop(), "Selecting a slot should activate its loop");

    processor.auditionSelectedSlot();
    expect (processor.getLastTriggeredSlot() == 4, "Mouse audition should trigger selected slot");
    expect (processor.getActiveVoiceCount() > 0, "Mouse audition should start a voice");
    processor.stopAudition();

    for (int i = 0; i < FronasmaskinenAudioProcessor::slotCount; ++i)
    {
        const auto note = FronasmaskinenAudioProcessor::baseNote + i;
        processMidiNote (processor, note, true);
        expect (processor.getLastMidiNote() == note, "MIDI note should be recorded");
        expect (processor.getLastTriggeredSlot() == i, "MIDI C1-A1 should trigger matching slot");
        expect (processor.getSelectedSlotIndex() == i, "MIDI-triggered slot should become active");
        expect (processor.getActiveVoiceCount() > 0, "MIDI note-on should start a voice");
        processMidiNote (processor, note, false);
    }

    processMidiNote (processor, FronasmaskinenAudioProcessor::baseNote + FronasmaskinenAudioProcessor::slotCount, true);
    expect (processor.getLastTriggeredSlot() == -1, "MIDI note outside C1-A1 should not trigger a slot");

    sample.deleteFile();
}

void runTest (const char* name, void (*test)())
{
    test();
    std::cout << "[pass] " << name << std::endl;
}
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInitialiser;

    try
    {
        runTest ("sample loading and waveform thumbnail", testSampleLoadingAndWaveform);
        runTest ("preview seek, play, and loop slot creation", testPreviewSeekPlayAndLoopSlotCreation);
        runTest ("slot trim and gain persistence", testSlotTrimAndGainPersistence);
        runTest ("release and random start", testReleaseAndRandomStart);
        runTest ("slot selection moves playhead during preview", testSelectingSlotDuringPreviewMovesPlayheadIntoSlot);
        runTest ("slot mouse and MIDI triggers", testSlotMouseAndMidiTriggers);
    }
    catch (const TestFailure& failure)
    {
        std::cerr << "[fail] " << failure.message << std::endl;
        return 1;
    }
    catch (const std::exception& error)
    {
        std::cerr << "[fail] Unexpected exception: " << error.what() << std::endl;
        return 1;
    }

    return 0;
}
