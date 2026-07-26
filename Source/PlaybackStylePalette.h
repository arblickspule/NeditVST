#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

//==============================================================================
/** Step-41: the Sequencer's Style Palette -- six colour swatches, one per
    PlaybackStyle, stacked vertically next to the sequencer grid. Clicking
    a swatch sets it as the processor's currently selected drawing style
    (persistent until changed again -- see SlicerAudioProcessor::
    getSelectedDrawingStyle()/setSelectedDrawingStyle()), which is what
    SequencerGrid's mouse handling paints new cells with. The currently
    selected swatch is drawn with a highlighted border so it's always
    clear which style the next click/drag on the grid will use.

    getStyleColour() is the single canonical colour-per-style mapping --
    SequencerGrid reuses it directly for the piano-roll bars so a bar's
    colour always matches its swatch here, rather than duplicating the
    palette in two places. */
class PlaybackStylePalette : public juce::Component
{
public:
    explicit PlaybackStylePalette (SlicerAudioProcessor& processorToUse);

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent& event) override;

    // The canonical style -> colour mapping, shared with SequencerGrid.
    // Placeholder colours picked to be easily distinguishable at a
    // glance; change freely.
    static juce::Colour getStyleColour (int styleIndex);

    // Fixed width for this component -- the editor reserves this much
    // horizontal space beside the sequencer grid's own viewport.
    static constexpr int preferredWidth = 130;

private:
    int getRowIndexAtY (int y) const;

    SlicerAudioProcessor& processor;

    static constexpr int rowHeight = 28;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PlaybackStylePalette)
};
