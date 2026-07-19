#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

//==============================================================================
/** Minimal step-1 editor: a button to load a sample and a label showing
    what's loaded. Waveform display and slice markers come in step 2, once
    we've ported the transient detector and actually have slices to draw. */
class SlicerAudioProcessorEditor : public juce::AudioProcessorEditor,
                                    private juce::Button::Listener
{
public:
    explicit SlicerAudioProcessorEditor (SlicerAudioProcessor&);
    ~SlicerAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void buttonClicked (juce::Button* button) override;
    void chooseAndLoadFile();

    SlicerAudioProcessor& processor;

    juce::TextButton loadButton { "Load Sample..." };
    juce::Label statusLabel;

    std::unique_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SlicerAudioProcessorEditor)
};
