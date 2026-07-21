#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "WaveformDisplay.h"
#include "SubdivisionProbabilityGrid.h"

//==============================================================================
class SlicerAudioProcessorEditor : public juce::AudioProcessorEditor,
                                    private juce::Button::Listener,
                                    private juce::Timer
{
public:
    explicit SlicerAudioProcessorEditor (SlicerAudioProcessor&);
    ~SlicerAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void buttonClicked (juce::Button* button) override;
    void timerCallback() override;
    void chooseAndLoadFile();
    void updateAfterSampleOrSliceChange();
    void updateTriggerModeVisibility();
    void updatePitchModeVisibility();
    void refreshAllControls();

    SlicerAudioProcessor& processor;

    juce::Viewport viewport;
    juce::Component contentComponent;

    juce::TextButton loadButton { "Load Sample..." };
    juce::TextButton resetEditsButton { "Reset edits" };
    juce::TextButton undoButton { "Undo" };
    juce::TextButton redoButton { "Redo" };
    juce::Label statusLabel;

    juce::Label loopLengthLabel;
    juce::Slider loopLengthSlider;
    juce::Label calculatedBpmLabel;

    juce::Label pitchModeLabel;
    juce::ComboBox pitchModeSelector;

    juce::Label grainSizeLabel;
    juce::Slider grainSizeSlider;
    juce::Label windowShapeLabel;
    juce::ComboBox windowShapeSelector;
    juce::Label pitchShiftLabel;
    juce::Slider pitchShiftSlider;

    juce::Label sensitivityLabel;
    juce::Slider sensitivitySlider;

    juce::Label fadeInLabel;
    juce::Slider fadeInSlider;
    juce::Label fadeOutLabel;
    juce::Slider fadeOutSlider;

    juce::Label triggerModeLabel;
    juce::ComboBox triggerModeSelector;

    juce::Label clockReferenceLabel;
    juce::ComboBox clockReferenceSelector;

    juce::Label subdivisionTableLabel;
    SubdivisionProbabilityGrid subdivisionGrid;

    WaveformDisplay waveformDisplay;

    std::unique_ptr<juce::FileChooser> fileChooser;

    static constexpr int headerHeight = 30;
    static constexpr int maxWindowHeight = 960;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SlicerAudioProcessorEditor)
};
