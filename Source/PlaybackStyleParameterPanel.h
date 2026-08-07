#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

//==============================================================================
/** Slice Length/Clock mode global-default parameter panel. A style selector
    (juce::ComboBox, same "Forward"/"Ping-Pong"/... order as
    PlaybackStylePalette/PlaybackStyleGrid) sits above a stack of rows showing
    whichever parameters SlicerAudioProcessor::getApplicableSequencerCellParameters()
    says the selected style actually uses (Subdivide excluded -- see its own
    doc comment below).

    Every row reuses the exact same visual/interaction language as
    SequencerGrid's right-click parameter editing (see its own class doc
    comment): continuous parameters get a hand-painted drag bar (black
    background, cyan fill proportional to value, white border, centred
    "Name: value" text -- SequencerGrid::paint()'s slider-overlay code,
    stretched to a persistent full-width row instead of a floating 120px
    overlay); discrete parameters (including each swept parameter's own
    Static/Sweep In/Sweep Out Mode) get a same-styled row that opens a
    juce::PopupMenu of named options on click, the exact same
    "addItem per option name" loop SequencerGrid::showParameterMenuForCell()
    already uses for its discrete submenus, parented to getTopLevelComponent()
    for the same reason (Rate's 20-item list would otherwise get squashed
    into columns by a narrow parent).

    Unlike SequencerGrid, this panel reads/writes the GLOBAL default value for
    each parameter (SlicerAudioProcessor::getSequencerCellParameterGlobalValue()/
    setSequencerCellParameterGlobalValue()), not a per-step override -- the
    same storage a Sequencer step without its own override already falls back
    to, so editing here changes both Slice Length/Clock mode playback AND
    whatever Sequenced-mode steps have no override of their own.

    The style selector's current selection is purely local UI state, kept
    deliberately separate from SlicerAudioProcessor::getSelectedDrawingStyle()
    (Sequenced mode's "what style paints next" concept) -- reusing that value
    here would mean switching styles in this panel also changed what
    Sequenced mode's own grid draws with, and vice versa.

    Subdivide (index 5) applies to every style in SequencerGrid's own menu
    but is excluded here -- it's a per-step retrigger rate tied to the step
    sequencer's own step timing, with no meaning in Slice Length/Clock modes. */
class PlaybackStyleParameterPanel : public juce::Component
{
public:
    explicit PlaybackStyleParameterPanel (SlicerAudioProcessor& processorToUse);

    void paint (juce::Graphics&) override;
    void resized() override;

    void mouseDown (const juce::MouseEvent& event) override;
    void mouseDrag (const juce::MouseEvent& event) override;
    void mouseUp (const juce::MouseEvent& event) override;

    // Fixed height sized for the worst-case style (most rows) -- see class
    // doc comment. The editor reserves exactly this much space regardless
    // of which style is currently selected, so choosing a different style
    // never reflows anything laid out below this panel.
    static int getPreferredHeight();

private:
    // One visible row: either a discrete option picker (Filter Type, Curve
    // Shape, a swept parameter's own Mode, Forward/Backward Curve, Rate) or
    // a continuous drag-bar (Resonance, Grain Size, Grain Speed, or a swept
    // parameter's own Value). paramIndex matches
    // SlicerAudioProcessor::getSequencerCellParameterName()'s own indexing.
    struct PanelRow
    {
        int paramIndex;
        bool discrete;
    };

    // Builds the row list for one style: getApplicableSequencerCellParameters()
    // minus Subdivide (index 5); a swept parameter (Sample Rate Reduction/
    // Bit Depth/Delay Time/Mix/Feedback) expands to its own Mode row
    // (discrete, paramIndex+1) followed by its Value row (continuous,
    // paramIndex) -- Mode first, matching the right-click menu's own "pick
    // mode, then configure amount" order.
    static std::vector<PanelRow> buildRowsForStyle (int style);

    juce::Rectangle<int> getRowBounds (int rowIndex) const;
    void showDiscreteOptionsMenu (const PanelRow& row, juce::Rectangle<int> rowBounds);
    void updateContinuousValueFromMouseX (int paramIndex, int mouseX, const juce::Rectangle<int>& rowBounds);

    SlicerAudioProcessor& processor;
    juce::ComboBox styleSelector;

    static constexpr int styleSelectorHeight = 24;
    static constexpr int rowGap = 6;
    static constexpr int rowHeight = 24;

    // Continuous-row drag state -- -1 when no row is being dragged, same
    // "click starts it, drag continues it, release ends it" shape
    // SequencerGrid's own slider overlay uses.
    int draggingParamIndex = -1;
    juce::Rectangle<int> draggingRowBounds;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PlaybackStyleParameterPanel)
};
