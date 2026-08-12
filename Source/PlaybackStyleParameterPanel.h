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

    The style selector's current selection is purely local UI state for the
    default (Slice Length/Clock) construction, kept deliberately separate
    from SlicerAudioProcessor::getSelectedDrawingStyle() (Sequenced mode's
    "what style paints next" concept) -- reusing that value here would mean
    switching styles in this panel also changed what Sequenced mode's own
    grid draws with, and vice versa.

    Reused by Performance mode (Pass 1) via the optional constructor
    parameters below: a getValue/setValue pair lets a caller point this panel
    at ANY per-index float storage instead of the global default value --
    Performance mode passes lambdas bound to
    SlicerAudioProcessor::getPerformanceWorkingParameterValue()/
    setPerformanceWorkingParameterValue() instead, so editing there can never
    read or write the same global values Slice Length/Clock use (they must
    stay fully independent). initialStyleId/onStyleChanged make the style
    selector's own selection persist as real state too, when a caller needs
    that (Performance mode does; Slice Length/Clock's own instance, passing
    neither, keeps today's "purely local UI state" behaviour unchanged).

    Subdivide (index 5) applies to every style in SequencerGrid's own menu
    but is excluded here -- it's a per-step retrigger rate tied to the step
    sequencer's own step timing, with no meaning in Slice Length/Clock modes.
    Volume (index 19) is excluded for the same reason: it's a per-step gain
    ramp keyed to the Sequencer's own Whole Window step timing (see
    PluginProcessor.h's own doc comment on isSequencerCellParameterSwept()),
    has no global dial (getSequencerCellParameterGlobalValue() hardcodes its
    fallback the same way it does for Subdivide), and isn't meaningful outside
    Sequenced mode -- a possible later addition once the Sequencer version is
    proven, not required now. */
class PlaybackStyleParameterPanel : public juce::Component
{
public:
    // getValue/setValue default to the global default value methods when
    // left null (the original Slice Length/Clock behaviour, unchanged).
    // initialStyleId seeds the style selector (JUCE's 1-based item IDs,
    // matching styleSelector.setSelectedId()'s own convention -- id 1 ==
    // style index 0). onStyleChanged, when set, is called with the newly
    // selected style index every time the selector changes -- see class doc
    // comment for why the default (null) instance leaves this purely local.
    using GetParameterValue = std::function<float (int paramIndex)>;
    using SetParameterValue = std::function<void (int paramIndex, float value)>;

    explicit PlaybackStyleParameterPanel (SlicerAudioProcessor& processorToUse,
                                           GetParameterValue getValueIn = nullptr,
                                           SetParameterValue setValueIn = nullptr,
                                           int initialStyleId = 1,
                                           std::function<void (int style)> onStyleChangedIn = nullptr);

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

    // Resyncs the style selector to styleIndex without firing onStyleChanged
    // -- needed by a caller whose underlying value can change out from under
    // this panel (Performance mode's working state can change via a MIDI
    // recall, on the audio thread, same class of "poll and resync" issue
    // SlicerAudioProcessorEditor::timerCallback() already handles for
    // stepResolutionSelector/patternLengthSelector). A no-op if styleIndex
    // is already selected, so it's cheap to call unconditionally every tick.
    void setSelectedStyle (int styleIndex);

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
    // minus Subdivide (index 5) and Volume (index 19); a swept parameter
    // (Sample Rate Reduction/Bit Depth/Delay Time/Mix/Feedback) expands to
    // its own Mode row (discrete, paramIndex+1) followed by its Value row
    // (continuous, paramIndex) -- Mode first, matching the right-click
    // menu's own "pick mode, then configure amount" order.
    static std::vector<PanelRow> buildRowsForStyle (int style);

    juce::Rectangle<int> getRowBounds (int rowIndex) const;
    void showDiscreteOptionsMenu (const PanelRow& row, juce::Rectangle<int> rowBounds);
    void updateContinuousValueFromMouseX (int paramIndex, int mouseX, const juce::Rectangle<int>& rowBounds);

    SlicerAudioProcessor& processor;
    juce::ComboBox styleSelector;
    GetParameterValue getValue;
    SetParameterValue setValue;
    std::function<void (int)> onStyleChanged;

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
