#include "PluginEditor.h"

namespace
{
bool isSupportedAudioFile (const juce::File& file)
{
    const auto extension = file.getFileExtension().toLowerCase();
    return file.existsAsFile() && (extension == ".wav" || extension == ".aif" || extension == ".aiff");
}
}

WaveformComponent::WaveformComponent (FronasmaskinenAudioProcessor& processorToUse)
    : processor (processorToUse)
{
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

void WaveformComponent::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    g.fillAll (juce::Colour::fromRGB (13, 18, 17));

    g.setColour (juce::Colour::fromRGBA (38, 51, 48, 180));
    for (int i = 1; i < 8; ++i)
    {
        const auto y = bounds.getHeight() * (float) i / 8.0f;
        g.drawHorizontalLine ((int) y, 0.0f, bounds.getWidth());
    }

    const auto duration = processor.getSampleDurationSeconds();
    if (duration <= 0.0)
    {
        g.setColour (juce::Colour::fromRGB (145, 158, 157));
        g.drawFittedText ("No waveform loaded", getLocalBounds(), juce::Justification::centred, 1);
        return;
    }

    std::vector<float> peaks;
    processor.getWaveformThumbnail (peaks);
    if (peaks.empty())
        return;

    const auto width = juce::jmax (1, getWidth());
    const auto middle = bounds.getCentreY();
    const auto halfHeight = bounds.getHeight() * 0.42f;

    g.setColour (juce::Colour::fromRGB (98, 137, 123));
    for (int x = 0; x < width; ++x)
    {
        const auto index = juce::jlimit (0, (int) peaks.size() - 1, (int) std::floor ((double) x / (double) width * (double) peaks.size()));
        const auto peak = peaks[(size_t) index];
        g.drawVerticalLine (x, middle - peak * halfHeight, middle + peak * halfHeight);
    }

    const auto loopStart = processor.getPreviewLoopStartSeconds();
    const auto loopEnd = processor.getPreviewLoopEndSeconds();
    if (loopStart >= 0.0 && loopEnd > loopStart)
    {
        const auto x1 = (float) (loopStart / duration) * bounds.getWidth();
        const auto x2 = (float) (loopEnd / duration) * bounds.getWidth();
        g.setColour (juce::Colour::fromRGBA (200, 148, 70, 44));
        g.fillRect (juce::Rectangle<float> (x1, 0.0f, x2 - x1, bounds.getHeight()));
        drawMarker (g, loopStart, juce::Colour::fromRGB (200, 148, 70), 3.0f);
        drawMarker (g, loopEnd, juce::Colour::fromRGB (180, 111, 77), 3.0f);
    }

    const auto pending = processor.getPendingLoopStartSeconds();
    if (pending >= 0.0)
        drawMarker (g, pending, juce::Colour::fromRGB (200, 148, 70), 2.0f);

    drawMarker (g, processor.getPreviewPositionSeconds(), juce::Colour::fromRGB (212, 176, 106), 2.0f);
}

void WaveformComponent::mouseDown (const juce::MouseEvent& event)
{
    const auto duration = processor.getSampleDurationSeconds();
    if (duration <= 0.0 || getWidth() <= 0)
        return;

    const auto ratio = juce::jlimit (0.0, 1.0, (double) event.x / (double) getWidth());
    processor.seekPreview (ratio * duration);
    repaint();
}

bool WaveformComponent::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& path : files)
        if (isSupportedAudioFile (juce::File (path)))
            return true;

    return false;
}

void WaveformComponent::filesDropped (const juce::StringArray& files, int, int)
{
    for (const auto& path : files)
    {
        const auto file = juce::File (path);
        if (isSupportedAudioFile (file) && processor.loadAudioFile (file))
        {
            repaint();
            return;
        }
    }
}

void WaveformComponent::drawMarker (juce::Graphics& g, double seconds, juce::Colour colour, float thickness)
{
    const auto duration = processor.getSampleDurationSeconds();
    if (duration <= 0.0)
        return;

    const auto x = (float) (seconds / duration) * (float) getWidth();
    g.setColour (colour);
    g.drawLine (x, 0.0f, x, (float) getHeight(), thickness);
}

FronasmaskinenAudioProcessorEditor::FronasmaskinenAudioProcessorEditor (FronasmaskinenAudioProcessor& p)
    : AudioProcessorEditor (&p), audioProcessor (p), waveform (p)
{
    setSize (760, 580);
    setWantsKeyboardFocus (true);

    titleLabel.setText ("Fronasmaskinen AU v0.1", juce::dontSendNotification);
    titleLabel.setFont (juce::FontOptions (24.0f, juce::Font::bold));
    addAndMakeVisible (titleLabel);

    addAndMakeVisible (loadButton);
    loadButton.setEnabled (true);
    loadButton.addListener (this);
    addAndMakeVisible (playButton);
    addAndMakeVisible (loopPointButton);
    addAndMakeVisible (releaseButton);
    addAndMakeVisible (randomButton);
    addAndMakeVisible (waveform);

    for (auto* button : { &playButton, &loopPointButton, &releaseButton, &randomButton })
        button->addListener (this);

    configureSlider (startTrimSlider, startTrimLabel, "Start trim");
    configureSlider (endTrimSlider, endTrimLabel, "End trim");
    configureSlider (fadeSlider, fadeLabel, "Fade");
    configureSlider (gainSlider, gainLabel, "Slot gain");

    startTrimSlider.setRange (-2.000, 2.000, 0.001);
    endTrimSlider.setRange (-2.000, 2.000, 0.001);
    fadeSlider.setRange (0.002, 0.080, 0.001);
    gainSlider.setRange (-24.0, 6.0, 0.1);
    startTrimSlider.setTextValueSuffix (" s");
    endTrimSlider.setTextValueSuffix (" s");
    fadeSlider.setTextValueSuffix (" s");
    gainSlider.setTextValueSuffix (" dB");

    for (int i = 0; i < FronasmaskinenAudioProcessor::slotCount; ++i)
    {
        slotButtons[(size_t) i].setButtonText ("S" + juce::String (i + 1));
        slotButtons[(size_t) i].addListener (this);
        addAndMakeVisible (slotButtons[(size_t) i]);

        moveLeftButtons[(size_t) i].setButtonText ("<");
        moveLeftButtons[(size_t) i].addListener (this);
        addAndMakeVisible (moveLeftButtons[(size_t) i]);

        moveRightButtons[(size_t) i].setButtonText (">");
        moveRightButtons[(size_t) i].addListener (this);
        addAndMakeVisible (moveRightButtons[(size_t) i]);

        clearButtons[(size_t) i].setButtonText ("x");
        clearButtons[(size_t) i].addListener (this);
        addAndMakeVisible (clearButtons[(size_t) i]);
    }

    refreshFromProcessor();
    startTimerHz (12);
}

void FronasmaskinenAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour::fromRGB (20, 22, 24));
    g.setColour (juce::Colour::fromRGB (38, 42, 46));
    g.fillRect (getLocalBounds().withTrimmedTop (72).reduced (18, 0));
}

void FronasmaskinenAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced (24);
    auto header = area.removeFromTop (48);
    titleLabel.setBounds (header.removeFromLeft (320));
    loadButton.setBounds (header.removeFromRight (140).reduced (0, 6));

    waveform.setBounds (area.removeFromTop (150).reduced (0, 4));

    area.removeFromTop (8);
    const auto slotWidth = area.getWidth() / FronasmaskinenAudioProcessor::slotCount;
    auto slotRow = area.removeFromTop (86);

    for (int i = 0; i < FronasmaskinenAudioProcessor::slotCount; ++i)
    {
        auto cell = slotRow.removeFromLeft (slotWidth).reduced (4);
        auto moveRow = cell.removeFromTop (20);
        moveLeftButtons[(size_t) i].setBounds (moveRow.removeFromLeft (24));
        moveRightButtons[(size_t) i].setBounds (moveRow.removeFromLeft (24).reduced (2, 0));
        slotButtons[(size_t) i].setBounds (cell.removeFromTop (40));
        clearButtons[(size_t) i].setBounds (cell.removeFromTop (22).reduced (10, 0));
    }

    auto transport = area.removeFromTop (42).reduced (0, 4);
    playButton.setBounds (transport.removeFromLeft (96).reduced (0, 2));
    loopPointButton.setBounds (transport.removeFromLeft (140).reduced (4, 2));
    releaseButton.setBounds (transport.removeFromLeft (110).reduced (4, 2));
    randomButton.setBounds (transport.removeFromLeft (140).reduced (4, 2));
    loadButton.toFront (false);

    area.removeFromTop (6);

    auto sliderArea = area.removeFromTop (172);
    auto layoutSlider = [&sliderArea] (juce::Label& label, juce::Slider& slider)
    {
        auto row = sliderArea.removeFromTop (42).reduced (0, 4);
        label.setBounds (row.removeFromLeft (90));
        slider.setBounds (row);
    };

    layoutSlider (startTrimLabel, startTrimSlider);
    layoutSlider (endTrimLabel, endTrimSlider);
    layoutSlider (fadeLabel, fadeSlider);
    layoutSlider (gainLabel, gainSlider);
}

void FronasmaskinenAudioProcessorEditor::buttonClicked (juce::Button* button)
{
    if (button == &loadButton)
    {
        fileChooser = std::make_unique<juce::FileChooser> ("Load a WAV or AIFF file", juce::File(), "*.wav;*.aif;*.aiff");
        fileChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                  [this] (const juce::FileChooser& chooser)
                                  {
                                      const auto file = chooser.getResult();
                                      if (file.existsAsFile())
                                      {
                                          audioProcessor.loadAudioFile (file);
                                          refreshFromProcessor();
                                      }
                                  });
        return;
    }

    if (button == &playButton)
    {
        audioProcessor.togglePreview();
        refreshFromProcessor();
        return;
    }

    if (button == &loopPointButton)
    {
        audioProcessor.setLoopPointAtPreviewPosition();
        refreshFromProcessor();
        return;
    }

    if (button == &releaseButton)
    {
        audioProcessor.releasePreviewLoop();
        refreshFromProcessor();
        return;
    }

    if (button == &randomButton)
    {
        audioProcessor.randomizePreviewLoopStart();
        refreshFromProcessor();
        return;
    }

    for (int i = 0; i < FronasmaskinenAudioProcessor::slotCount; ++i)
    {
        if (button == &slotButtons[(size_t) i])
        {
            if (! audioProcessor.selectSlot (i))
                audioProcessor.saveSelectionToSlot (i);

            refreshFromProcessor();
            return;
        }

        if (button == &moveLeftButtons[(size_t) i])
        {
            audioProcessor.moveSlotLeft (i);
            refreshFromProcessor();
            return;
        }

        if (button == &moveRightButtons[(size_t) i])
        {
            audioProcessor.moveSlotRight (i);
            refreshFromProcessor();
            return;
        }

        if (button == &clearButtons[(size_t) i])
        {
            audioProcessor.clearSlot (i);
            refreshFromProcessor();
            return;
        }
    }
}

bool FronasmaskinenAudioProcessorEditor::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::spaceKey)
    {
        audioProcessor.togglePreview();
        refreshFromProcessor();
        return true;
    }

    return false;
}

void FronasmaskinenAudioProcessorEditor::sliderValueChanged (juce::Slider* slider)
{
    if (slider == &startTrimSlider || slider == &endTrimSlider)
    {
        audioProcessor.setSelectedSlotTrim (startTrimSlider.getValue(), endTrimSlider.getValue());
        refreshFromProcessor();
        return;
    }

    if (slider == &fadeSlider)
    {
        audioProcessor.setSelectedSlotFadeSeconds (fadeSlider.getValue());
        refreshFromProcessor();
        return;
    }

    if (slider == &gainSlider)
    {
        audioProcessor.setSelectedSlotGainDb ((float) gainSlider.getValue());
        refreshFromProcessor();
    }
}

void FronasmaskinenAudioProcessorEditor::timerCallback()
{
    waveform.repaint();
    refreshFromProcessor();
}

void FronasmaskinenAudioProcessorEditor::configureSlider (juce::Slider& slider, juce::Label& label, const juce::String& text)
{
    label.setText (text, juce::dontSendNotification);
    label.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (label);

    slider.setSliderStyle (juce::Slider::LinearHorizontal);
    slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 90, 24);
    slider.addListener (this);
    addAndMakeVisible (slider);
}

void FronasmaskinenAudioProcessorEditor::refreshFromProcessor()
{
    playButton.setButtonText (audioProcessor.isPreviewPlaying() ? "Pause" : "Play");
    loopPointButton.setButtonText (audioProcessor.hasPendingLoopStart() ? "Set loop end" : "Set loop start");
    loadButton.setEnabled (true);
    playButton.setEnabled (audioProcessor.hasSample());
    loopPointButton.setEnabled (audioProcessor.hasSample() && audioProcessor.isPreviewPlaying() && ! audioProcessor.hasPreviewLoop());
    releaseButton.setEnabled (audioProcessor.hasPreviewLoop());
    randomButton.setEnabled (audioProcessor.hasPreviewLoop());

    syncSelectedSlotControls();

    const auto selected = audioProcessor.getSelectedSlotIndex();

    for (int i = 0; i < FronasmaskinenAudioProcessor::slotCount; ++i)
    {
        const auto slot = audioProcessor.getSlot (i);
        const auto text = "S" + juce::String (i + 1) + "\n" + audioProcessor.describeSlot (i);
        slotButtons[(size_t) i].setButtonText (text);
        slotButtons[(size_t) i].setColour (juce::TextButton::buttonColourId,
                                           selected == i ? juce::Colour::fromRGB (54, 116, 104)
                                                         : juce::Colour::fromRGB (48, 52, 56));
        moveLeftButtons[(size_t) i].setEnabled (slot.filled && i > 0 && audioProcessor.getSlot (i - 1).filled);
        moveRightButtons[(size_t) i].setEnabled (slot.filled && i < FronasmaskinenAudioProcessor::slotCount - 1
                                                 && audioProcessor.getSlot (i + 1).filled);
        clearButtons[(size_t) i].setEnabled (slot.filled);
    }
}

void FronasmaskinenAudioProcessorEditor::updateRangeFromSample()
{
    const auto duration = juce::jmax (0.25, audioProcessor.getSampleDurationSeconds());
    startSlider.setRange (0.0, duration, 0.001);
    endSlider.setRange (0.0, duration, 0.001);
    startSlider.setTextValueSuffix (" s");
    endSlider.setTextValueSuffix (" s");
}

void FronasmaskinenAudioProcessorEditor::syncSelectedSlotControls()
{
    const auto selected = audioProcessor.getSelectedSlotIndex();
    const auto hasSelected = selected >= 0;

    startTrimSlider.setEnabled (hasSelected);
    endTrimSlider.setEnabled (hasSelected);
    fadeSlider.setEnabled (hasSelected);
    gainSlider.setEnabled (hasSelected);

    if (! hasSelected)
    {
        startTrimSlider.setValue (0.0, juce::dontSendNotification);
        endTrimSlider.setValue (0.0, juce::dontSendNotification);
        fadeSlider.setValue (0.012, juce::dontSendNotification);
        gainSlider.setValue (0.0, juce::dontSendNotification);
        return;
    }

    const auto slot = audioProcessor.getSlot (selected);
    startTrimSlider.setValue (slot.startTrimSeconds, juce::dontSendNotification);
    endTrimSlider.setValue (slot.endTrimSeconds, juce::dontSendNotification);
    fadeSlider.setValue (slot.fadeSeconds, juce::dontSendNotification);
    gainSlider.setValue (slot.gainDb, juce::dontSendNotification);
}
