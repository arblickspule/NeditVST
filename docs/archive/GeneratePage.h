#pragma once

#include "../layout/Layout.h"
#include "../components/UiPanel.h"
#include "../../SlicerModel.h"

class SlicerEngine;
class WaveformDisplay;

namespace ui
{

//==============================================================================
/** The Generate page: every Beats-mode section (Sample / Tempo / Detection /
    Fade In/Out / Pitch Mode / Playback Style / Timing) composed on the layout
    DSL as a vertical stack of TitledPanels, replacing the old editor's
    per-section setBounds() math. Reads model state through the shared 10fps
    poll (syncFromModel()) and writes back through each control's own
    callback -- the same data flow as PluginEditor, none of its ~93 setBounds
    calls.

    Sizing policy (see docs/ui-layout-decision.md section 7):
      - Each TitledPanel is a ui::Box, so its height follows its content
        (getPreferredMainSize()), and the page column grows to fit all of it.
      - Leaf controls are fixed-size via ui::sized(); sliders/labels that
        should stretch use ui::fill() and keep their constructor height.
      - Sections whose rows change with a selection/toggle rebuild their own
        cells (setCells()) and then re-layout the page, so the page height
        follows the new content. Controls that must keep state across such a
        rebuild (the section SegmentedButtonRows, the sliders) live in the
        never-rebuilt parts of their section; only the conditional rows are
        swapped in and out. */
class GeneratePage : public Box, public UiPanel
{
public:
    GeneratePage (SlicerModel& modelToUse, SlicerEngine& engineToUse, WaveformDisplay& waveformToUse);

    void syncFromModel() override;

    // Refresh the file-name/slice-count status, the "~X BPM" readout and the
    // waveform cache -- the caller (or the page's own load/redetect handlers)
    // runs this after loading a sample or any slice-affecting change.
    void refreshAfterSampleOrSliceChange();

private:
    void buildSections();
    void buildSampleSection();
    void buildTempoSection();
    void buildDetectionSection();
    void buildFadeSection();
    void buildPitchModeSection();
    void buildPlaybackStyleSection();
    void buildTimingSection();

    TitledPanel* addSection (std::vector<Cell>& cells, juce::String title);

    // Conditional-row rebuilds -- each targets one small dedicated Box so
    // nothing else in the section (slider state, segment selection) is torn
    // down, then re-layouts the page to follow the new content height.
    void rebuildManualBpmRow();
    void rebuildQuantizeGridRow();
    void rebuildPitchModeExtra();
    void rebuildPlaybackStyleContent();
    void rebuildTimingExtra();

    // Timing section content builders -- the swap-box cells are rebuilt
    // from these whenever the trigger-mode selection changes.
    std::vector<Cell> makeTimingClockCells();
    std::vector<Cell> makeTimingSliceLengthCells();

    void loadSampleWithChooser();
    void setLoopLengthAttention (bool attention);

    // Leaf factories (fixed sizes per the DSL sizing rule).
    static std::unique_ptr<juce::Label> makeLabel (const juce::String& text);
    static std::unique_ptr<juce::Slider> makeSlider (juce::Slider::SliderStyle style,
                                                     double min, double max, double interval,
                                                     int textBoxWidth, int width, int height);
    static std::unique_ptr<juce::ComboBox> makeComboBox (int width, int height);
    static std::unique_ptr<juce::ToggleButton> makeToggle (const juce::String& text);

    SlicerModel& model;
    SlicerEngine& engine;
    WaveformDisplay& waveform;

    // Stable section panels -- owned by this page's cells, never recreated,
    // so these raw pointers are valid for the page's whole lifetime.
    TitledPanel* sampleSection = nullptr;
    TitledPanel* tempoSection = nullptr;
    TitledPanel* detectionSection = nullptr;
    TitledPanel* fadeSection = nullptr;
    TitledPanel* pitchModeSection = nullptr;
    TitledPanel* playbackStyleSection = nullptr;
    TitledPanel* timingSection = nullptr;

    // Swap-content sub-boxes inside the sections above (empty = collapsed).
    Box* manualBpmRow = nullptr;
    Box* quantizeGridRow = nullptr;
    Box* pitchModeExtra = nullptr;
    Box* timingExtra = nullptr;

    // Controls the page polls or refreshes. Raw pointers: owned by the tree,
    // captured during build, never dangling (only the conditional rows above
    // are recreated, and their children are re-captured on each rebuild).
    juce::Label* statusLabel = nullptr;
    juce::Label* calculatedBpmLabel = nullptr;
    juce::Label* loopLengthLabel = nullptr;
    juce::TextButton* auditionButton = nullptr;
    juce::TextButton* undoButton = nullptr;
    juce::TextButton* redoButton = nullptr;

    int selectedPlaybackStyle = 0; // pure UI navigation state (like the old editor)
    SlicerModel::TriggerMode lastGenerateTriggerMode = SlicerModel::TriggerMode::sliceLength;

    std::unique_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GeneratePage)
};

} // namespace ui
