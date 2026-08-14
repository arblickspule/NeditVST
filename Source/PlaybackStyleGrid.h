#pragma once

#include <JuceHeader.h>
#include "SlicerModel.h"

//==============================================================================
/** Playback style probability (Forward / Ping-Pong / Tape Stop / Stretch /
    Filter Down / Filter Up / Bitcrush / Scratch / Flanger), drawn as a
    horizontal multislider — same custom-painted pattern as
    SubdivisionProbabilityGrid (one row per option, label on the left, a
    draggable horizontal bar filling the rest of the row), but that class
    turned out to be wired directly to the subdivision table rather than
    built generically, so this is a small equivalent for the playback
    style table instead of a forced reuse.

    Drag anywhere in a row sets that style's probability from the
    horizontal position (left edge = 0.0, right edge = 1.0). Lives in
    Layer 3's always-visible Playback Style block, so it's present across
    all trigger modes; in Sequenced mode each cell's own style comes from
    the sequencer grid's right-click menu, while this table stays the
    global fallback. */
class PlaybackStyleGrid : public juce::Component
{
public:
    explicit PlaybackStyleGrid (SlicerModel& modelToUse);

    void paint (juce::Graphics&) override;

    void mouseDown (const juce::MouseEvent& event) override;
    void mouseDrag (const juce::MouseEvent& event) override;

    // Total height needed to show every row at once.
    static int getPreferredHeight();

private:
    void setProbabilityFromMouse (const juce::MouseEvent& event);
    int getRowIndexAtY (int y) const;

    SlicerModel& model;

    static constexpr int rowHeight = 18;
    static constexpr int labelWidth = 70;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PlaybackStyleGrid)
};
