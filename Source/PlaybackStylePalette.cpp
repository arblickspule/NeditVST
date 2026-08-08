#include "PlaybackStylePalette.h"

PlaybackStylePalette::PlaybackStylePalette (SlicerAudioProcessor& processorToUse)
    : processor (processorToUse)
{
    setSize (preferredWidth, getPreferredHeight());
}

int PlaybackStylePalette::getPreferredHeight()
{
    return SlicerAudioProcessor::numPlaybackStyleOptions * rowHeight;
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
        case 6:  return juce::Colours::limegreen;   // Bitcrush
        case 7:  return juce::Colours::hotpink;     // Scratch
        case 8:  return juce::Colours::cyan;        // Flanger
        default: return juce::Colours::grey;
    }
}

int PlaybackStylePalette::getRowIndexAtY (int y) const
{
    return juce::jlimit (0, SlicerAudioProcessor::numPlaybackStyleOptions - 1, y / rowHeight);
}

juce::Rectangle<int> PlaybackStylePalette::getCheckboxBounds (int row) const
{
    const int y = row * rowHeight + (rowHeight - checkboxSize) / 2;
    return { getWidth() - checkboxSize - 3, y, checkboxSize, checkboxSize };
}

void PlaybackStylePalette::paint (juce::Graphics& g)
{
    const int selected = processor.getSelectedDrawingStyle();

    for (int i = 0; i < SlicerAudioProcessor::numPlaybackStyleOptions; ++i)
    {
        const juce::Rectangle<int> row (0, i * rowHeight, getWidth(), rowHeight);
        const auto checkboxBounds = getCheckboxBounds (i);
        const auto swatch = row.reduced (3).withTrimmedRight (checkboxBounds.getWidth() + 6);

        g.setColour (getStyleColour (i));
        g.fillRect (swatch);

        const bool isSelected = (i == selected);
        g.setColour (isSelected ? juce::Colours::white : juce::Colours::black.withAlpha (0.4f));
        g.drawRect (swatch, isSelected ? 2 : 1);

        g.setColour (juce::Colours::white);
        g.setFont (12.0f);
        g.drawFittedText (SlicerAudioProcessor::getPlaybackStyleName (i), swatch.reduced (4, 0),
                           juce::Justification::centred, 1);

        // "Randomize parameters" checkbox (see this class's own doc
        // comment) -- independent of the swatch's selected/unselected
        // border above.
        const bool randomizeParams = processor.getRandomizeParametersForStyle (i);

        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.fillRect (checkboxBounds);
        g.setColour (juce::Colours::white.withAlpha (0.8f));
        g.drawRect (checkboxBounds, 1);

        if (randomizeParams)
        {
            g.setColour (juce::Colours::white);
            g.fillRect (checkboxBounds.reduced (3));
        }
    }
}

void PlaybackStylePalette::mouseDown (const juce::MouseEvent& event)
{
    const int row = getRowIndexAtY (event.y);

    if (getCheckboxBounds (row).contains (event.x, event.y))
        processor.setRandomizeParametersForStyle (row, ! processor.getRandomizeParametersForStyle (row));
    else
        processor.setSelectedDrawingStyle (row);

    repaint();
}
