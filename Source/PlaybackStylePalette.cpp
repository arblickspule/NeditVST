#include "PlaybackStylePalette.h"

PlaybackStylePalette::PlaybackStylePalette (SlicerAudioProcessor& processorToUse)
    : processor (processorToUse)
{
    setSize (preferredWidth, SlicerAudioProcessor::numPlaybackStyleOptions * rowHeight);
}

juce::Colour PlaybackStylePalette::getStyleColour (int styleIndex)
{
    switch (styleIndex)
    {
        case 0:  return juce::Colours::orange;      // Forward
        case 1:  return juce::Colours::purple;      // Ping-Pong
        case 2:  return juce::Colours::dodgerblue;  // Tape Stop
        case 3:  return juce::Colours::teal;        // Stretch
        case 4:  return juce::Colours::red;         // Filter Down
        case 5:  return juce::Colours::gold;        // Filter Up
        default: return juce::Colours::grey;
    }
}

int PlaybackStylePalette::getRowIndexAtY (int y) const
{
    return juce::jlimit (0, SlicerAudioProcessor::numPlaybackStyleOptions - 1, y / rowHeight);
}

void PlaybackStylePalette::paint (juce::Graphics& g)
{
    const int selected = processor.getSelectedDrawingStyle();

    for (int i = 0; i < SlicerAudioProcessor::numPlaybackStyleOptions; ++i)
    {
        const juce::Rectangle<int> row (0, i * rowHeight, getWidth(), rowHeight);
        const auto swatch = row.reduced (3);

        g.setColour (getStyleColour (i));
        g.fillRect (swatch);

        const bool isSelected = (i == selected);
        g.setColour (isSelected ? juce::Colours::white : juce::Colours::black.withAlpha (0.4f));
        g.drawRect (swatch, isSelected ? 2 : 1);

        g.setColour (juce::Colours::white);
        g.setFont (12.0f);
        g.drawFittedText (SlicerAudioProcessor::getPlaybackStyleName (i), swatch.reduced (4, 0),
                           juce::Justification::centred, 1);
    }
}

void PlaybackStylePalette::mouseDown (const juce::MouseEvent& event)
{
    processor.setSelectedDrawingStyle (getRowIndexAtY (event.y));
    repaint();
}
