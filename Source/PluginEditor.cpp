#include "PluginEditor.h"

namespace
{
bool isSupportedAudioFile (const juce::File& file)
{
    const auto extension = file.getFileExtension().toLowerCase();
    return file.existsAsFile() && (extension == ".wav" || extension == ".aif" || extension == ".aiff");
}

juce::String describeSlot (const FronasmaskinenAudioProcessor::Slot& slot)
{
    if (! slot.filled)
        return "empty";

    return juce::String (slot.baseStartSeconds, 3) + "s - " + juce::String (slot.baseEndSeconds, 3) + "s";
}

juce::Path buildAdsrPath (juce::Rectangle<float> bounds, double attack, double decay, float sustain, double release)
{
    const auto total = juce::jmax (0.20, attack + decay + release + 0.20);
    const auto sustainHold = juce::jmax (0.16, total * 0.28);
    const auto drawTotal = attack + decay + sustainHold + release;
    const auto bottom = bounds.getBottom();
    const auto top = bounds.getY();
    const auto levelY = [&bounds, bottom] (float level)
    {
        return bottom - (juce::jlimit (0.0f, 1.0f, level) * bounds.getHeight());
    };
    const auto xFor = [&bounds, drawTotal] (double seconds)
    {
        return bounds.getX() + (float) (seconds / drawTotal) * bounds.getWidth();
    };

    const auto attackEnd = attack;
    const auto decayEnd = attack + decay;
    const auto sustainEnd = decayEnd + sustainHold;
    const auto releaseEnd = sustainEnd + release;

    juce::Path path;
    path.startNewSubPath (bounds.getX(), bottom);
    path.lineTo (xFor (attackEnd), top);
    path.lineTo (xFor (decayEnd), levelY (sustain));
    path.lineTo (xFor (sustainEnd), levelY (sustain));
    path.lineTo (xFor (releaseEnd), bottom);
    return path;
}
}

FronasLookAndFeel::FronasLookAndFeel()
{
    setColour (juce::Slider::thumbColourId, juce::Colour::fromRGB (73, 172, 209));
    setColour (juce::Slider::trackColourId, juce::Colour::fromRGB (33, 59, 66));
    setColour (juce::Slider::rotarySliderFillColourId, juce::Colour::fromRGB (80, 178, 157));
    setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour::fromRGB (42, 52, 57));
    setColour (juce::TextButton::buttonColourId, juce::Colour::fromRGB (36, 47, 54));
    setColour (juce::TextButton::buttonOnColourId, juce::Colour::fromRGB (54, 116, 104));
}

void FronasLookAndFeel::drawButtonBackground (juce::Graphics& g,
                                              juce::Button& button,
                                              const juce::Colour& backgroundColour,
                                              bool shouldDrawButtonAsHighlighted,
                                              bool shouldDrawButtonAsDown)
{
    auto bounds = button.getLocalBounds().toFloat().reduced (0.5f);
    auto colour = backgroundColour;

    if (! button.isEnabled())
        colour = juce::Colour::fromRGB (31, 37, 41);
    else if (shouldDrawButtonAsDown)
        colour = colour.brighter (0.18f);
    else if (shouldDrawButtonAsHighlighted)
        colour = colour.brighter (0.08f);

    g.setColour (colour);
    g.fillRoundedRectangle (bounds, 6.0f);

    g.setColour (button.isEnabled() ? juce::Colour::fromRGB (137, 158, 164)
                                    : juce::Colour::fromRGB (76, 86, 90));
    g.drawRoundedRectangle (bounds, 6.0f, 1.0f);
}

void FronasLookAndFeel::drawRotarySlider (juce::Graphics& g,
                                          int x,
                                          int y,
                                          int width,
                                          int height,
                                          float sliderPosProportional,
                                          float rotaryStartAngle,
                                          float rotaryEndAngle,
                                          juce::Slider& slider)
{
    const auto bounds = juce::Rectangle<float> ((float) x, (float) y, (float) width, (float) height).reduced (7.0f);
    const auto radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto centre = bounds.getCentre();
    const auto angle = rotaryStartAngle + sliderPosProportional * (rotaryEndAngle - rotaryStartAngle);
    const auto arcRadius = radius - 6.0f;
    const auto lineWidth = juce::jmax (2.0f, radius * 0.12f);

    juce::Path backgroundArc;
    backgroundArc.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, rotaryStartAngle, rotaryEndAngle, true);
    g.setColour (slider.findColour (juce::Slider::rotarySliderOutlineColourId));
    g.strokePath (backgroundArc, juce::PathStrokeType (lineWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    juce::Path valueArc;
    valueArc.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, rotaryStartAngle, angle, true);
    g.setColour (slider.findColour (juce::Slider::rotarySliderFillColourId));
    g.strokePath (valueArc, juce::PathStrokeType (lineWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    g.setColour (juce::Colour::fromRGB (21, 27, 30));
    g.fillEllipse (centre.x - radius * 0.62f, centre.y - radius * 0.62f, radius * 1.24f, radius * 1.24f);

    g.setColour (juce::Colour::fromRGB (83, 99, 105));
    g.drawEllipse (centre.x - radius * 0.62f, centre.y - radius * 0.62f, radius * 1.24f, radius * 1.24f, 1.0f);

    juce::Path pointer;
    pointer.addRoundedRectangle (-2.0f, -radius * 0.52f, 4.0f, radius * 0.34f, 2.0f);
    pointer.applyTransform (juce::AffineTransform::rotation (angle).translated (centre.x, centre.y));
    g.setColour (juce::Colour::fromRGB (232, 228, 210));
    g.fillPath (pointer);
}

void FronasLookAndFeel::drawLinearSlider (juce::Graphics& g,
                                          int x,
                                          int y,
                                          int width,
                                          int height,
                                          float sliderPos,
                                          float minSliderPos,
                                          float maxSliderPos,
                                          const juce::Slider::SliderStyle style,
                                          juce::Slider& slider)
{
    if (style != juce::Slider::LinearHorizontal)
    {
        LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);
        return;
    }

    juce::ignoreUnused (minSliderPos, maxSliderPos);
    auto bounds = juce::Rectangle<float> ((float) x, (float) y, (float) width, (float) height);
    auto track = bounds.withHeight (6.0f).withCentre (bounds.getCentre());

    g.setColour (juce::Colour::fromRGB (20, 30, 34));
    g.fillRoundedRectangle (track, 3.0f);

    g.setColour (slider.findColour (juce::Slider::trackColourId));
    g.fillRoundedRectangle (track.withRight (sliderPos), 3.0f);

    g.setColour (slider.findColour (juce::Slider::thumbColourId));
    g.fillEllipse (sliderPos - 6.0f, bounds.getCentreY() - 6.0f, 12.0f, 12.0f);
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

SlotStripComponent::SlotStripComponent()
{
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
}

SlotStripComponent::~SlotStripComponent()
{
    for (int i = 0; i < FronasmaskinenAudioProcessor::slotCount; ++i)
    {
        slotButtons[(size_t) i].removeListener (this);
        moveLeftButtons[(size_t) i].removeListener (this);
        moveRightButtons[(size_t) i].removeListener (this);
        clearButtons[(size_t) i].removeListener (this);
    }
}

void SlotStripComponent::resized()
{
    auto area = getLocalBounds();
    const auto slotWidth = area.getWidth() / FronasmaskinenAudioProcessor::slotCount;

    for (int i = 0; i < FronasmaskinenAudioProcessor::slotCount; ++i)
    {
        auto cell = area.removeFromLeft (slotWidth).reduced (4);
        auto moveRow = cell.removeFromTop (20);
        moveLeftButtons[(size_t) i].setBounds (moveRow.removeFromLeft (24));
        moveRightButtons[(size_t) i].setBounds (moveRow.removeFromLeft (24).reduced (2, 0));
        slotButtons[(size_t) i].setBounds (cell.removeFromTop (42));
        clearButtons[(size_t) i].setBounds (cell.removeFromTop (22).reduced (10, 0));
    }
}

void SlotStripComponent::refresh (const FronasmaskinenAudioProcessor::EditorSnapshot& snapshot)
{
    for (int i = 0; i < FronasmaskinenAudioProcessor::slotCount; ++i)
    {
        const auto& slot = snapshot.slots[(size_t) i];
        const auto text = "S" + juce::String (i + 1) + "\n" + describeSlot (slot);
        slotButtons[(size_t) i].setButtonText (text);
        slotButtons[(size_t) i].setColour (juce::TextButton::buttonColourId,
                                           snapshot.selectedSlot == i ? juce::Colour::fromRGB (54, 116, 104)
                                                                      : juce::Colour::fromRGB (36, 47, 54));
        moveLeftButtons[(size_t) i].setEnabled (slot.filled && i > 0 && snapshot.slots[(size_t) (i - 1)].filled);
        moveRightButtons[(size_t) i].setEnabled (slot.filled && i < FronasmaskinenAudioProcessor::slotCount - 1
                                                 && snapshot.slots[(size_t) (i + 1)].filled);
        clearButtons[(size_t) i].setEnabled (slot.filled);
    }
}

void SlotStripComponent::triggerSlotClick (int slotIndex)
{
    if (onSlotClicked)
        onSlotClicked (slotIndex);
}

void SlotStripComponent::triggerMoveLeftClick (int slotIndex)
{
    if (onMoveLeftClicked)
        onMoveLeftClicked (slotIndex);
}

void SlotStripComponent::triggerMoveRightClick (int slotIndex)
{
    if (onMoveRightClicked)
        onMoveRightClicked (slotIndex);
}

void SlotStripComponent::triggerClearClick (int slotIndex)
{
    if (onClearClicked)
        onClearClicked (slotIndex);
}

void SlotStripComponent::buttonClicked (juce::Button* button)
{
    for (int i = 0; i < FronasmaskinenAudioProcessor::slotCount; ++i)
    {
        if (button == &slotButtons[(size_t) i])
        {
            triggerSlotClick (i);
            return;
        }

        if (button == &moveLeftButtons[(size_t) i])
        {
            triggerMoveLeftClick (i);
            return;
        }

        if (button == &moveRightButtons[(size_t) i])
        {
            triggerMoveRightClick (i);
            return;
        }

        if (button == &clearButtons[(size_t) i])
        {
            triggerClearClick (i);
            return;
        }
    }
}

void AdsrDisplayComponent::setAdsr (double attackSeconds, double decaySeconds, float sustainLevel, double releaseSeconds, bool enabled)
{
    attack = attackSeconds;
    decay = decaySeconds;
    sustain = sustainLevel;
    release = releaseSeconds;
    active = enabled;
    repaint();
}

void AdsrDisplayComponent::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    g.setColour (juce::Colour::fromRGB (18, 25, 28));
    g.fillRoundedRectangle (bounds, 6.0f);
    g.setColour (juce::Colour::fromRGB (63, 78, 84));
    g.drawRoundedRectangle (bounds.reduced (0.5f), 6.0f, 1.0f);

    auto graph = bounds.reduced (14.0f, 12.0f);
    g.setColour (juce::Colour::fromRGBA (98, 116, 120, 90));
    for (int i = 1; i < 4; ++i)
    {
        const auto y = graph.getY() + graph.getHeight() * (float) i / 4.0f;
        g.drawHorizontalLine ((int) y, graph.getX(), graph.getRight());
    }

    auto path = buildAdsrPath (graph, attack, decay, sustain, release);
    g.setColour (active ? juce::Colour::fromRGB (98, 202, 177)
                        : juce::Colour::fromRGB (86, 100, 104));
    g.strokePath (path, juce::PathStrokeType (2.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    g.setColour (active ? juce::Colour::fromRGBA (98, 202, 177, 36)
                        : juce::Colour::fromRGBA (86, 100, 104, 22));
    path.lineTo (graph.getRight(), graph.getBottom());
    path.lineTo (graph.getX(), graph.getBottom());
    path.closeSubPath();
    g.fillPath (path);
}

FronasmaskinenAudioProcessorEditor::FronasmaskinenAudioProcessorEditor (FronasmaskinenAudioProcessor& p)
    : AudioProcessorEditor (&p), audioProcessor (p), waveform (p)
{
    setLookAndFeel (&lookAndFeel);
    setSize (820, 760);
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
    addAndMakeVisible (slotStrip);
    addAndMakeVisible (adsrDisplay);

    for (auto* button : { &playButton, &loopPointButton, &releaseButton, &randomButton })
        button->addListener (this);

    slotStrip.onSlotClicked = [this] (int slotIndex)
    {
        if (! audioProcessor.selectSlot (slotIndex))
            audioProcessor.saveSelectionToSlot (slotIndex);

        refreshFromProcessor();
    };
    slotStrip.onMoveLeftClicked = [this] (int slotIndex)
    {
        audioProcessor.moveSlotLeft (slotIndex);
        refreshFromProcessor();
    };
    slotStrip.onMoveRightClicked = [this] (int slotIndex)
    {
        audioProcessor.moveSlotRight (slotIndex);
        refreshFromProcessor();
    };
    slotStrip.onClearClicked = [this] (int slotIndex)
    {
        audioProcessor.clearSlot (slotIndex);
        refreshFromProcessor();
    };

    configureSlider (startTrimSlider, startTrimLabel, "Start trim");
    configureSlider (endTrimSlider, endTrimLabel, "End trim");
    configureSlider (fadeSlider, fadeLabel, "Fade");
    configureSlider (gainSlider, gainLabel, "Slot gain");
    configureSlider (attackSlider, attackLabel, "Attack");
    configureSlider (decaySlider, decayLabel, "Decay");
    configureSlider (sustainSlider, sustainLabel, "Sustain");
    configureSlider (releaseSlider, releaseLabel, "Release");

    startTrimSlider.setRange (-2.000, 2.000, 0.001);
    endTrimSlider.setRange (-2.000, 2.000, 0.001);
    fadeSlider.setRange (0.002, 0.080, 0.001);
    gainSlider.setRange (-24.0, 6.0, 0.1);
    attackSlider.setRange (0.000, 5.000, 0.001);
    decaySlider.setRange (0.000, 5.000, 0.001);
    sustainSlider.setRange (0.000, 1.000, 0.01);
    releaseSlider.setRange (0.000, 5.000, 0.001);
    configureKnob (fadeSlider);
    configureKnob (gainSlider);
    startTrimSlider.setTextValueSuffix (" s");
    endTrimSlider.setTextValueSuffix (" s");
    fadeSlider.setTextValueSuffix (" s");
    gainSlider.setTextValueSuffix (" dB");
    attackSlider.setTextValueSuffix (" s");
    decaySlider.setTextValueSuffix (" s");
    releaseSlider.setTextValueSuffix (" s");

    refreshFromProcessor();
    startTimerHz (12);
}

FronasmaskinenAudioProcessorEditor::~FronasmaskinenAudioProcessorEditor()
{
    setLookAndFeel (nullptr);
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
    slotStrip.setBounds (area.removeFromTop (90));

    auto transport = area.removeFromTop (42).reduced (0, 4);
    playButton.setBounds (transport.removeFromLeft (96).reduced (0, 2));
    loopPointButton.setBounds (transport.removeFromLeft (140).reduced (4, 2));
    releaseButton.setBounds (transport.removeFromLeft (110).reduced (4, 2));
    randomButton.setBounds (transport.removeFromLeft (140).reduced (4, 2));
    loadButton.toFront (false);

    area.removeFromTop (6);

    auto controlsArea = area.removeFromTop (350);
    auto trimArea = controlsArea.removeFromLeft (330);
    auto knobArea = controlsArea.removeFromLeft (190).reduced (10, 0);
    auto adsrArea = controlsArea.reduced (10, 0);

    auto layoutSlider = [] (juce::Rectangle<int>& sliderArea, juce::Label& label, juce::Slider& slider)
    {
        auto row = sliderArea.removeFromTop (42).reduced (0, 4);
        label.setBounds (row.removeFromLeft (90));
        slider.setBounds (row);
    };

    layoutSlider (trimArea, startTrimLabel, startTrimSlider);
    layoutSlider (trimArea, endTrimLabel, endTrimSlider);

    auto fadeKnob = knobArea.removeFromTop (160);
    fadeLabel.setBounds (fadeKnob.removeFromTop (24));
    fadeSlider.setBounds (fadeKnob.reduced (12, 0));

    auto gainKnob = knobArea.removeFromTop (160);
    gainLabel.setBounds (gainKnob.removeFromTop (24));
    gainSlider.setBounds (gainKnob.reduced (12, 0));

    adsrDisplay.setBounds (adsrArea.removeFromTop (120).reduced (0, 4));
    adsrArea.removeFromTop (8);
    layoutSlider (adsrArea, attackLabel, attackSlider);
    layoutSlider (adsrArea, decayLabel, decaySlider);
    layoutSlider (adsrArea, sustainLabel, sustainSlider);
    layoutSlider (adsrArea, releaseLabel, releaseSlider);
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
        return;
    }

    if (slider == &attackSlider || slider == &decaySlider || slider == &sustainSlider || slider == &releaseSlider)
    {
        audioProcessor.setSelectedSlotAdsr (attackSlider.getValue(),
                                            decaySlider.getValue(),
                                            (float) sustainSlider.getValue(),
                                            releaseSlider.getValue());
        refreshFromProcessor();
    }
}

void FronasmaskinenAudioProcessorEditor::timerCallback()
{
    const auto snapshot = audioProcessor.getEditorSnapshot();
    waveform.repaint();
    refreshPlaybackState (snapshot);

    if (snapshot.selectedSlot != controlsSyncedToSelectedSlot)
    {
        syncSelectedSlotControls (snapshot);
        refreshSlotButtons (snapshot);
        controlsSyncedToSelectedSlot = snapshot.selectedSlot;
    }
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

void FronasmaskinenAudioProcessorEditor::configureKnob (juce::Slider& slider)
{
    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setRotaryParameters (juce::MathConstants<float>::pi * 1.18f,
                                juce::MathConstants<float>::pi * 2.82f,
                                true);
    slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 88, 22);
}

void FronasmaskinenAudioProcessorEditor::refreshFromProcessor()
{
    const auto snapshot = audioProcessor.getEditorSnapshot();

    refreshPlaybackState (snapshot);
    syncSelectedSlotControls (snapshot);
    refreshSlotButtons (snapshot);
    controlsSyncedToSelectedSlot = snapshot.selectedSlot;
}

void FronasmaskinenAudioProcessorEditor::refreshPlaybackState (const FronasmaskinenAudioProcessor::EditorSnapshot& snapshot)
{
    playButton.setButtonText (snapshot.previewPlaying ? "Pause" : "Play");
    loopPointButton.setButtonText (snapshot.pendingLoopStartActive ? "Set loop end" : "Set loop start");
    loadButton.setEnabled (true);
    playButton.setEnabled (snapshot.hasSample);
    loopPointButton.setEnabled (snapshot.hasSample && snapshot.previewPlaying && ! snapshot.previewLoopActive);
    releaseButton.setEnabled (snapshot.previewLoopActive);
    randomButton.setEnabled (snapshot.previewLoopActive);
}

void FronasmaskinenAudioProcessorEditor::refreshSlotButtons (const FronasmaskinenAudioProcessor::EditorSnapshot& snapshot)
{
    slotStrip.refresh (snapshot);
}

void FronasmaskinenAudioProcessorEditor::syncSelectedSlotControls (const FronasmaskinenAudioProcessor::EditorSnapshot& snapshot)
{
    const auto selected = snapshot.selectedSlot;
    const auto hasSelected = selected >= 0;

    startTrimSlider.setEnabled (hasSelected);
    endTrimSlider.setEnabled (hasSelected);
    fadeSlider.setEnabled (hasSelected);
    gainSlider.setEnabled (hasSelected);
    attackSlider.setEnabled (hasSelected);
    decaySlider.setEnabled (hasSelected);
    sustainSlider.setEnabled (hasSelected);
    releaseSlider.setEnabled (hasSelected);

    if (! hasSelected)
    {
        startTrimSlider.setValue (0.0, juce::dontSendNotification);
        endTrimSlider.setValue (0.0, juce::dontSendNotification);
        fadeSlider.setValue (0.012, juce::dontSendNotification);
        gainSlider.setValue (0.0, juce::dontSendNotification);
        attackSlider.setValue (0.012, juce::dontSendNotification);
        decaySlider.setValue (0.0, juce::dontSendNotification);
        sustainSlider.setValue (1.0, juce::dontSendNotification);
        releaseSlider.setValue (0.012, juce::dontSendNotification);
        adsrDisplay.setAdsr (0.012, 0.0, 1.0f, 0.012, false);
        return;
    }

    const auto slot = snapshot.slots[(size_t) selected];
    startTrimSlider.setValue (slot.startTrimSeconds, juce::dontSendNotification);
    endTrimSlider.setValue (slot.endTrimSeconds, juce::dontSendNotification);
    fadeSlider.setValue (slot.fadeSeconds, juce::dontSendNotification);
    gainSlider.setValue (slot.gainDb, juce::dontSendNotification);
    attackSlider.setValue (slot.attackSeconds, juce::dontSendNotification);
    decaySlider.setValue (slot.decaySeconds, juce::dontSendNotification);
    sustainSlider.setValue (slot.sustainLevel, juce::dontSendNotification);
    releaseSlider.setValue (slot.releaseSeconds, juce::dontSendNotification);
    adsrDisplay.setAdsr (slot.attackSeconds, slot.decaySeconds, slot.sustainLevel, slot.releaseSeconds, true);
}
