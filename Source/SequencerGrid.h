#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "PlaybackStylePalette.h"

//==============================================================================
/** Step-37: mouse-drawable step-sequencer grid for Sequenced Trigger Mode
    (v1, monophonic). One row per available slice
    (SlicerAudioProcessor::getSequencerNumRows(), capped at 32), one
    column per step (SlicerAudioProcessor::getSequencerNumSteps()).

    Click-drag across cells in a row toggles them on/off, standard
    step-sequencer UX -- the row is locked for the whole drag gesture (set
    by wherever the mouse went down), and the WHOLE drag keeps doing
    whichever of activate/deactivate the first cell did, so dragging
    across already-lit cells erases them and dragging across dark ones
    lights them, rather than flickering as the cursor crosses cell
    boundaries. Structural monophony (only one active cell per column,
    grid-wide) is enforced by the processor's setSequencerCell(), not this
    component -- it just reflects whatever state comes back.

    An active cell draws as a bar spanning however many subsequent
    step-columns that row's slice's natural length covers (same
    beats-from-natural-length math Beat-Quantize already uses), cut short
    at whichever comes first: that natural length, or the next active
    cell anywhere in the grid (any row) -- monophony means THAT is what
    will actually cut the note off at playback, so the piano roll always
    shows exactly what will be heard, not just an approximation. Each bar
    is drawn in its own cell's PlaybackStyle colour (Step 41, see
    PlaybackStylePalette::getStyleColour()) -- clicking/dragging an empty
    cell writes the Style Palette's currently selected style; a cell
    already showing a DIFFERENT style gets painted over with the selected
    one; a cell already showing the SAME style toggles off, preserving
    the "click again to remove" behaviour for the common case.

    Row 0 (the first slice) renders at the BOTTOM of the grid, standard
    piano-roll convention -- this is purely a rendering/hit-testing choice
    (getRowIndexAtY() inverts screen row -> data row); the underlying
    row-index-to-slice-index mapping is untouched.

    Width (Step 38): stretches to whatever target width the editor gives
    it via setTargetWidth() -- meant to be called with WaveformDisplay's
    own width, so the grid always spans exactly as wide as the waveform
    below it rather than shrinking to a cramped fixed-pixel-per-column
    default. Column width is therefore computed (targetWidth / numColumns),
    not a fixed constant -- more columns means thinner columns, same as
    any DAW piano roll. Height still self-sizes to numRows * rowHeight and
    is meant to live inside a juce::Viewport for vertical scrolling when
    there are more rows than fit.

    Self-sizing: polls the processor's current row/column counts on a
    timer (same 30fps live-update pattern WaveformDisplay already uses)
    and resizes itself whenever they (or the target width) change. The
    same timer also drives the live playhead column highlight.

    Step parameter editing (Step 45/46, Flow A): right-click (or
    Cmd/Ctrl-click) an active step to pop up a menu of whichever
    parameters that step's own PlaybackStyle actually uses (see
    SlicerAudioProcessor::getApplicableSequencerCellParameters()) -- e.g.
    Filter Down/Up offers Resonance + Filter Type, Ping-Pong/Tape Stop
    offers Curve Shape, Stretch offers Grain Size + Grain Speed, Forward
    offers nothing (no menu at all). Continuous parameters (Resonance,
    Grain Size, Grain Speed) work exactly as Step 45 established: selecting
    one replaces the menu with a horizontal slider overlay drawn over that
    step; a left-click-drag on the slider adjusts the value live, writing
    into that cell's parameter-override map, and releasing the mouse
    closes the slider automatically. Discrete parameters (Filter Type,
    Curve Shape -- a small named list rather than a range, see
    SlicerAudioProcessor::isSequencerCellParameterDiscrete()) instead
    present as a submenu of their own option names; selecting one writes
    straight into the override map with no slider overlay at all, since a
    handful of named choices doesn't need one. A small triangle marker in
    the corner of any cell with at least one override (of any parameter)
    stays visible afterward. While the slider overlay is open, it captures
    the WHOLE next left-click gesture (whether that lands on the slider or
    elsewhere, which just cancels/dismisses it) -- normal cell
    click/drag-to-toggle is otherwise completely unaffected, since
    right-click is a distinct gesture juce::MouseEvent::mods.isPopupMenu()
    identifies before any of the ordinary toggle logic runs. */
class SequencerGrid : public juce::Component,
                       private juce::Timer
{
public:
    explicit SequencerGrid (SlicerAudioProcessor& processorToUse);

    void paint (juce::Graphics&) override;

    void mouseDown (const juce::MouseEvent& event) override;
    void mouseDrag (const juce::MouseEvent& event) override;
    void mouseUp (const juce::MouseEvent& event) override;

    // Sets the total width this component should span -- called by the
    // editor with WaveformDisplay's own width (Step 38), so the two
    // components always visually line up.
    void setTargetWidth (int width);

private:
    void timerCallback() override; // polls dimensions (resizes if changed) and drives the playhead + repaint
    void updateSizeIfNeeded();

    int getRowIndexAtY (int y) const;
    int getColumnIndexAtX (int x) const;
    int getColumnWidth() const; // targetWidth / numColumns, clamped to a sane minimum

    // How many subsequent columns (from startColumn, inclusive) a note in
    // `row` should visually span -- see the class doc comment above.
    int computeBarLengthInSteps (int row, int startColumn, int numRows, int numColumns) const;

    // Step parameter editing (Step 45/46).
    void showParameterMenuForCell (int row, int column);
    juce::Rectangle<int> getParameterSliderBounds (int row, int column, int numRows, int numColumns) const;
    void updateEditingValueFromMouseX (int mouseX, const juce::Rectangle<int>& sliderBounds);

    // Index (into SlicerAudioProcessor::getSequencerCellParameterName())
    // of whichever parameter editingParameterName currently names, or -1
    // if it doesn't match any (shouldn't happen in practice) -- looked up
    // by name rather than storing the index directly since the override
    // map itself is keyed by name (Step 46).
    int findEditingParameterIndex() const;

    SlicerAudioProcessor& processor;

    static constexpr int rowHeight = 16;
    static constexpr int minColumnWidth = 2; // floor so an extreme column count never collapses to 0px-wide cells

    // Target total width (Step 38) -- matches WaveformDisplay's width, set
    // via setTargetWidth(). Starts at a sane placeholder until the editor's
    // first layout call.
    int targetWidth = 400;

    // Drag state -- dragRow is locked for the whole gesture (-1 when not
    // dragging); dragTargetStyle (Step 41) is decided once on mouseDown --
    // -1 (clear) if the first cell already showed the currently selected
    // style, otherwise the selected style index -- and reused for every
    // subsequent cell the drag passes over, same "whole drag does the same
    // thing" UX as before, just style-aware now instead of a plain toggle.
    int dragRow = -1;
    int dragTargetStyle = -1;

    // Step parameter editing state (Step 45) -- editingRow is -1 when no
    // slider overlay is showing. Set once a parameter is chosen from the
    // right-click menu; cleared (closing the overlay) on mouse release or
    // a click that misses the slider's own bounds.
    int editingRow = -1;
    int editingColumn = -1;
    juce::String editingParameterName;
    bool sliderDragging = false;

    // Self-sizing (see class doc comment) -- only calls setSize() when
    // the dimensions actually change, not every tick.
    int lastKnownNumRows = 0;
    int lastKnownNumColumns = 0;
    int lastKnownTargetWidth = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SequencerGrid)
};
