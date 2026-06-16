#pragma once

#include <JuceHeader.h>
#include <functional>
#include "PluginProcessor.h"

class FronasLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    FronasLookAndFeel();

    void drawButtonBackground (juce::Graphics& g,
                               juce::Button& button,
                               const juce::Colour& backgroundColour,
                               bool shouldDrawButtonAsHighlighted,
                               bool shouldDrawButtonAsDown) override;
    void drawRotarySlider (juce::Graphics& g,
                           int x,
                           int y,
                           int width,
                           int height,
                           float sliderPosProportional,
                           float rotaryStartAngle,
                           float rotaryEndAngle,
                           juce::Slider& slider) override;
    void drawLinearSlider (juce::Graphics& g,
                           int x,
                           int y,
                           int width,
                           int height,
                           float sliderPos,
                           float minSliderPos,
                           float maxSliderPos,
                           const juce::Slider::SliderStyle style,
                           juce::Slider& slider) override;
};

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

class SlotStripComponent final : public juce::Component,
                                 private juce::Button::Listener
{
public:
    std::function<void (int)> onSlotClicked;
    std::function<void (int)> onMoveLeftClicked;
    std::function<void (int)> onMoveRightClicked;
    std::function<void (int)> onClearClicked;

    SlotStripComponent();
    ~SlotStripComponent() override;

    void resized() override;
    void refresh (const FronasmaskinenAudioProcessor::EditorSnapshot& snapshot);
    void triggerSlotClick (int slotIndex);
    void triggerMoveLeftClick (int slotIndex);
    void triggerMoveRightClick (int slotIndex);
    void triggerClearClick (int slotIndex);

private:
    std::array<juce::TextButton, FronasmaskinenAudioProcessor::slotCount> slotButtons;
    std::array<juce::TextButton, FronasmaskinenAudioProcessor::slotCount> moveLeftButtons;
    std::array<juce::TextButton, FronasmaskinenAudioProcessor::slotCount> moveRightButtons;
    std::array<juce::TextButton, FronasmaskinenAudioProcessor::slotCount> clearButtons;

    void buttonClicked (juce::Button*) override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SlotStripComponent)
};

class AdsrDisplayComponent final : public juce::Component
{
public:
    AdsrDisplayComponent() = default;

    void setAdsr (double attackSeconds, double decaySeconds, float sustainLevel, double releaseSeconds, bool enabled);
    void paint (juce::Graphics& g) override;

private:
    double attack = 0.012;
    double decay = 0.0;
    float sustain = 1.0f;
    double release = 0.012;
    bool active = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AdsrDisplayComponent)
};

class FronasmaskinenAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                                 private juce::Button::Listener,
                                                 private juce::Slider::Listener,
                                                 private juce::Timer
{
public:
    explicit FronasmaskinenAudioProcessorEditor (FronasmaskinenAudioProcessor&);
    ~FronasmaskinenAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress& key) override;

private:
    FronasmaskinenAudioProcessor& audioProcessor;

    FronasLookAndFeel lookAndFeel;
    juce::TextButton loadButton { "Load WAV/AIFF" };
    juce::TextButton playButton { "Play" };
    juce::TextButton loopPointButton { "Set loop start" };
    juce::TextButton releaseButton { "Release" };
    juce::TextButton randomButton { "Random start" };
    juce::Label titleLabel;
    WaveformComponent waveform;
    SlotStripComponent slotStrip;
    AdsrDisplayComponent adsrDisplay;
    juce::Slider startTrimSlider;
    juce::Slider endTrimSlider;
    juce::Slider fadeSlider;
    juce::Slider gainSlider;
    juce::Slider attackSlider;
    juce::Slider decaySlider;
    juce::Slider sustainSlider;
    juce::Slider releaseSlider;
    juce::Label startTrimLabel;
    juce::Label endTrimLabel;
    juce::Label fadeLabel;
    juce::Label gainLabel;
    juce::Label attackLabel;
    juce::Label decayLabel;
    juce::Label sustainLabel;
    juce::Label releaseLabel;
    std::unique_ptr<juce::FileChooser> fileChooser;
    int controlsSyncedToSelectedSlot = -2;

    void buttonClicked (juce::Button*) override;
    void sliderValueChanged (juce::Slider*) override;
    void timerCallback() override;
    void configureSlider (juce::Slider& slider, juce::Label& label, const juce::String& text);
    void configureKnob (juce::Slider& slider);
    void refreshFromProcessor();
    void refreshPlaybackState (const FronasmaskinenAudioProcessor::EditorSnapshot& snapshot);
    void refreshSlotButtons (const FronasmaskinenAudioProcessor::EditorSnapshot& snapshot);
    void syncSelectedSlotControls (const FronasmaskinenAudioProcessor::EditorSnapshot& snapshot);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FronasmaskinenAudioProcessorEditor)
};
