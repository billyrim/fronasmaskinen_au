#include <JuceHeader.h>
#include "../Source/PluginProcessor.h"
#include "../Source/PluginEditor.h"

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

float renderMidiNoteRms (FronasmaskinenAudioProcessor& processor, int noteNumber)
{
    juce::AudioBuffer<float> buffer (2, 2048);
    juce::MidiBuffer midi;
    midi.addEvent (juce::MidiMessage::noteOn (1, noteNumber, (juce::uint8) 100), 0);
    processor.processBlock (buffer, midi);
    return buffer.getRMSLevel (0, 0, buffer.getNumSamples());
}

float renderPreviewRms (FronasmaskinenAudioProcessor& processor)
{
    juce::AudioBuffer<float> buffer (2, 2048);
    juce::MidiBuffer midi;
    processor.processBlock (buffer, midi);
    return buffer.getRMSLevel (0, 0, buffer.getNumSamples());
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

void testMidiTriggerUpdatesPausedPreviewLoop()
{
    auto processor = FronasmaskinenAudioProcessor();
    const auto sample = createTestSample();
    expect (processor.loadAudioFile (sample), "Sample should load");
    processor.prepareToPlay (44100.0, 256);

    processor.setSelection (0.10, 0.18);
    expect (processor.saveSelectionToSlot (0), "Should save S1");
    processor.setSelection (0.42, 0.50);
    expect (processor.saveSelectionToSlot (1), "Should save S2");

    expect (! processor.isPreviewPlaying(), "Preview should be paused before MIDI trigger");
    expectNear (processor.getPreviewLoopStartSeconds(), 0.42, epsilon, "S2 should be the active loop before MIDI");

    processMidiNote (processor, FronasmaskinenAudioProcessor::baseNote, true);
    expect (processor.getSelectedSlotIndex() == 0, "MIDI-triggered S1 should become selected while preview is paused");
    expectNear (processor.getPreviewLoopStartSeconds(), 0.10, epsilon, "MIDI-triggered S1 should update preview loop start");
    expectNear (processor.getPreviewLoopEndSeconds(), 0.18, epsilon, "MIDI-triggered S1 should update preview loop end");
    expectNear (processor.getPreviewPositionSeconds(), 0.10, epsilon, "MIDI-triggered S1 should move preview playhead to loop start");

    sample.deleteFile();
}

void testSlotGainAffectsMidiVoiceLevel()
{
    auto processor = FronasmaskinenAudioProcessor();
    const auto sample = createTestSample();
    expect (processor.loadAudioFile (sample), "Sample should load");
    processor.prepareToPlay (44100.0, 2048);

    processor.setSelection (0.05, 0.35);
    expect (processor.saveSelectionToSlot (0), "Should save S1");

    processor.setSelectedSlotGainDb (0.0f);
    const auto unityRms = renderMidiNoteRms (processor, FronasmaskinenAudioProcessor::baseNote);
    expect (unityRms > 0.001f, "Unity-gain MIDI voice should render audible output");

    processor.setSelectedSlotGainDb (-12.0f);
    const auto quietRms = renderMidiNoteRms (processor, FronasmaskinenAudioProcessor::baseNote);
    expect (quietRms < unityRms * 0.40f, "Negative slot gain should reduce MIDI voice level");

    processor.setSelectedSlotGainDb (6.0f);
    const auto loudRms = renderMidiNoteRms (processor, FronasmaskinenAudioProcessor::baseNote);
    expect (loudRms > unityRms * 1.70f, "Positive slot gain should increase MIDI voice level");

    sample.deleteFile();
}

void testSlotGainAffectsPreviewLoopLevel()
{
    auto processor = FronasmaskinenAudioProcessor();
    const auto sample = createTestSample();
    expect (processor.loadAudioFile (sample), "Sample should load");
    processor.prepareToPlay (44100.0, 2048);

    processor.setSelection (0.05, 0.35);
    expect (processor.saveSelectionToSlot (0), "Should save S1");

    processor.setSelectedSlotGainDb (0.0f);
    processor.playPreview();
    const auto unityRms = renderPreviewRms (processor);
    expect (unityRms > 0.001f, "Unity-gain preview loop should render audible output");

    expect (processor.selectSlot (0), "Should reset preview to S1");
    processor.setSelectedSlotGainDb (-12.0f);
    processor.playPreview();
    const auto quietRms = renderPreviewRms (processor);
    expect (quietRms < unityRms * 0.40f, "Negative slot gain should reduce preview loop level");

    expect (processor.selectSlot (0), "Should reset preview to S1 again");
    processor.setSelectedSlotGainDb (6.0f);
    processor.playPreview();
    const auto loudRms = renderPreviewRms (processor);
    expect (loudRms > unityRms * 1.70f, "Positive slot gain should increase preview loop level");

    sample.deleteFile();
}

void testWaveformFileDragAndDropLoadsSample()
{
    auto processor = FronasmaskinenAudioProcessor();
    auto waveform = WaveformComponent (processor);
    const auto sample = createTestSample();

    juce::StringArray files;
    files.add (sample.getFullPathName());
    expect (waveform.isInterestedInFileDrag (files), "Waveform should accept WAV files dragged from Finder");

    waveform.filesDropped (files, 10, 10);
    expect (processor.hasSample(), "Dropping a WAV on the waveform should load it");
    expect (processor.getLoadedFilePath() == sample.getFullPathName(), "Dropped file path should be stored");

    sample.deleteFile();
}

void testSlotMoveSwapsFilledSlots()
{
    auto processor = FronasmaskinenAudioProcessor();
    const auto sample = createTestSample();
    expect (processor.loadAudioFile (sample), "Sample should load");

    processor.setSelection (0.10, 0.18);
    expect (processor.saveSelectionToSlot (0), "Should save S1");
    processor.setSelection (0.30, 0.38);
    expect (processor.saveSelectionToSlot (1), "Should save S2");
    processor.setSelection (0.50, 0.58);
    expect (processor.saveSelectionToSlot (2), "Should save S3");

    expect (processor.selectSlot (0), "Should select S1 before moving it");
    expect (processor.moveSlotRight (0), "S1 should move right when S2 is filled");
    expectNear (processor.getSlot (0).baseStartSeconds, 0.30, epsilon, "S2 should move into slot 1");
    expectNear (processor.getSlot (1).baseStartSeconds, 0.10, epsilon, "S1 should move into slot 2");
    expectNear (processor.getSlot (2).baseStartSeconds, 0.50, epsilon, "S3 should stay in slot 3");
    expect (processor.getSelectedSlotIndex() == 1, "Selection should follow moved S1 to slot 2");
    expectNear (processor.getPreviewLoopStartSeconds(), 0.10, epsilon, "Active loop should follow moved S1");

    expect (processor.moveSlotRight (1), "Moved S1 should move right again when S3 is filled");
    expectNear (processor.getSlot (0).baseStartSeconds, 0.30, epsilon, "S2 should remain first");
    expectNear (processor.getSlot (1).baseStartSeconds, 0.50, epsilon, "S3 should move into slot 2");
    expectNear (processor.getSlot (2).baseStartSeconds, 0.10, epsilon, "S1 should move into slot 3");
    expect (processor.getSelectedSlotIndex() == 2, "Selection should follow moved S1 to slot 3");

    expect (processor.moveSlotLeft (2), "S1 should move left when S3 is filled");
    expectNear (processor.getSlot (1).baseStartSeconds, 0.10, epsilon, "S1 should move back into slot 2");
    expectNear (processor.getSlot (2).baseStartSeconds, 0.50, epsilon, "S3 should move back into slot 3");
    expect (processor.getSelectedSlotIndex() == 1, "Selection should follow moved S1 back to slot 2");

    sample.deleteFile();
}

void testSlotMoveRejectsEdgesAndEmptyTargets()
{
    auto processor = FronasmaskinenAudioProcessor();
    const auto sample = createTestSample();
    expect (processor.loadAudioFile (sample), "Sample should load");

    processor.setSelection (0.10, 0.18);
    expect (processor.saveSelectionToSlot (0), "Should save S1");
    processor.setSelection (0.30, 0.38);
    expect (processor.saveSelectionToSlot (1), "Should save S2");
    processor.setSelection (0.50, 0.58);
    expect (processor.saveSelectionToSlot (2), "Should save S3");

    expect (! processor.moveSlotLeft (0), "S1 should not move left");
    expect (! processor.moveSlotRight (2), "Last filled slot before an empty slot should not move right");
    expect (! processor.moveSlotRight (3), "Empty S4 should not move right");
    expect (! processor.moveSlotLeft (3), "Empty S4 should not move left");

    expectNear (processor.getSlot (0).baseStartSeconds, 0.10, epsilon, "S1 should remain first");
    expectNear (processor.getSlot (1).baseStartSeconds, 0.30, epsilon, "S2 should remain second");
    expectNear (processor.getSlot (2).baseStartSeconds, 0.50, epsilon, "S3 should remain third");
    expect (! processor.getSlot (3).filled, "S4 should stay empty");

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
        runTest ("MIDI trigger updates paused preview loop", testMidiTriggerUpdatesPausedPreviewLoop);
        runTest ("slot gain affects MIDI voice level", testSlotGainAffectsMidiVoiceLevel);
        runTest ("slot gain affects preview loop level", testSlotGainAffectsPreviewLoopLevel);
        runTest ("waveform file drag and drop loads sample", testWaveformFileDragAndDropLoadsSample);
        runTest ("slot move swaps filled slots", testSlotMoveSwapsFilledSlots);
        runTest ("slot move rejects edges and empty targets", testSlotMoveRejectsEdgesAndEmptyTargets);
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
