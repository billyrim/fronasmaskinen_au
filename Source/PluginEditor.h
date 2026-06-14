#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

class WaveformComponent final : public juce::Component
{
public:
    explicit WaveformComponent (FronasmaskinenAudioProcessor& processorToUse);

    void paint (juce::Graphics& g) override;
    void mouseDown (const juce::MouseEvent& event) override;

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
    juce::Label fileLabel;
    juce::Label selectedLabel;
    juce::Label durationLabel;
    juce::Label midiLabel;
    juce::TextButton auditionButton { "Audition selected" };
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
    std::array<juce::TextButton, FronasmaskinenAudioProcessor::slotCount> clearButtons;
    std::unique_ptr<juce::FileChooser> fileChooser;

    void buttonClicked (juce::Button*) override;
    void buttonStateChanged (juce::Button*) override;
    void sliderValueChanged (juce::Slider*) override;
    void timerCallback() override;
    void configureSlider (juce::Slider& slider, juce::Label& label, const juce::String& text);
    void refreshFromProcessor();
    void updateRangeFromSample();
    void syncSelectedSlotControls();
    static juce::String secondsText (double seconds);
    static juce::String midiNoteName (int noteNumber);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FronasmaskinenAudioProcessorEditor)
};
