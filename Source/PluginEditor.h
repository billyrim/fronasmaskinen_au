#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

class WaveformComponent final : public juce::Component,
                                public juce::FileDragAndDropTarget
{
public:
    explicit WaveformComponent (FronasmaskinenAudioProcessor& processorToUse);

    void paint (juce::Graphics& g) override;
    void mouseDown (const juce::MouseEvent& event) override;
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

private:
    FronasmaskinenAudioProcessor& processor;

    void drawMarker (juce::Graphics& g, double seconds, juce::Colour colour, float thickness);
};

class FronasmaskinenAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                                 private juce::Button::Listener,
                                                 private juce::Slider::Listener,
                                                 private juce::Timer
{
public:
    explicit FronasmaskinenAudioProcessorEditor (FronasmaskinenAudioProcessor&);
    ~FronasmaskinenAudioProcessorEditor() override = default;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress& key) override;

private:
    FronasmaskinenAudioProcessor& audioProcessor;

    juce::TextButton loadButton { "Load WAV/AIFF" };
    juce::TextButton playButton { "Play" };
    juce::TextButton loopPointButton { "Set loop start" };
    juce::TextButton releaseButton { "Release" };
    juce::TextButton randomButton { "Random start" };
    juce::Label titleLabel;
    WaveformComponent waveform;
    juce::Slider startSlider;
    juce::Slider endSlider;
    juce::Slider startTrimSlider;
    juce::Slider endTrimSlider;
    juce::Slider gainSlider;
    juce::Label startLabel;
    juce::Label endLabel;
    juce::Label startTrimLabel;
    juce::Label endTrimLabel;
    juce::Label gainLabel;
    std::array<juce::TextButton, FronasmaskinenAudioProcessor::slotCount> slotButtons;
    std::array<juce::TextButton, FronasmaskinenAudioProcessor::slotCount> moveLeftButtons;
    std::array<juce::TextButton, FronasmaskinenAudioProcessor::slotCount> moveRightButtons;
    std::array<juce::TextButton, FronasmaskinenAudioProcessor::slotCount> clearButtons;
    std::unique_ptr<juce::FileChooser> fileChooser;

    void buttonClicked (juce::Button*) override;
    void sliderValueChanged (juce::Slider*) override;
    void timerCallback() override;
    void configureSlider (juce::Slider& slider, juce::Label& label, const juce::String& text);
    void refreshFromProcessor();
    void updateRangeFromSample();
    void syncSelectedSlotControls();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FronasmaskinenAudioProcessorEditor)
};
