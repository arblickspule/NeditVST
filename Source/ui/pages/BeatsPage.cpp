// The Beats page (docs/ui-layout-decision.md migration step 2): every
// Beats-mode section composed on the layout DSL as a vertical stack of
// TitledPanels, with the waveform display pinned at the bottom. This is the
// archived GeneratePage (docs/archive/GeneratePage.{h,cpp}) ported onto the
// full-flex Layout.h (juce::FlexBox base) -- the old ui::Row/ui::Column swap
// boxes are now ui::Flexes, and the page owns its WaveformDisplay instead of
// receiving one from the editor. Model/engine are only ever touched through
// their public members.

#include "BeatsPage.h"
#include "../../model/SlicerEngine.h"
#include "../../ui/widgets/WaveformDisplay.h"
#include "../../ui/widgets/PlaybackStyleGrid.h"
#include "../../ui/widgets/PlaybackStyleParameterPanel.h"
#include "../../ui/widgets/PlaybackStylePalette.h"
#include "../../ui/widgets/SubdivisionProbabilityGrid.h"
#include "../../ui/widgets/SegmentedButtonRow.h"
#include "../NeditPalette.h"

namespace ui
{

//==============================================================================
BeatsPage::BeatsPage (SlicerModel& modelToUse, SlicerEngine& engineToUse)
    : Flex (FlexConfig{ .direction = Direction::column,
                        .gap = 14.0f,                        // section-to-section gap
                        .padding = juce::BorderSize<int> (0, 0, 8, 0) }),
      model (modelToUse),
      engine (engineToUse),
      waveform (new WaveformDisplay (model))
{
    // The Beats page's Timing section offers Slice Length / Clock only (the
    // old editor's Sequence/Perform sub-tabs don't exist in the two-tab
    // shell). If a previous session persisted sequenced/performance as the
    // active trigger mode, bring the engine back into the sliceLength/clock
    // domain this page actually controls -- otherwise the segment row would
    // claim "Slice Length" while the engine ran a mode this UI can't reach.
    const auto mode = model.getTriggerMode();
    lastGenerateTriggerMode = mode == SlicerModel::TriggerMode::clock
                              ? SlicerModel::TriggerMode::clock
                              : SlicerModel::TriggerMode::sliceLength;
    if (mode != SlicerModel::TriggerMode::sliceLength
        && mode != SlicerModel::TriggerMode::clock)
        engine.setTriggerMode (SlicerModel::TriggerMode::sliceLength);

    waveform->onSampleChanged = [this] { refreshAfterSampleOrSliceChange(); };
    waveform->onTrimChanged = [this]
    {
        setLoopLengthAttention (true);
        refreshAfterSampleOrSliceChange();
    };

    buildSections();
    refreshAfterSampleOrSliceChange();
}

//==============================================================================
void BeatsPage::resized()
{
    Flex::resized();

    // The viewport scrolls this page rather than stretching it, so the page
    // must always fit its content vertically -- the base Flex floor only ever
    // GROWS (a parent fill-cell may want the box bigger); here the parent is
    // the viewport and never stretches, so shrink to content too. Guarded so
    // the setSize -> resized() round-trip terminates (second call is a no-op).
    const int want = juce::roundToInt (getPreferredMainSize());
    if (getHeight() != want)
        setSize (getWidth(), want);
}

//==============================================================================
void BeatsPage::buildSections()
{
    std::vector<Cell> cells;
    sampleSection        = addSection (cells, "Sample");
    tempoSection         = addSection (cells, "Tempo");
    detectionSection     = addSection (cells, "Detection");
    fadeSection          = addSection (cells, "Fade In/Out");
    pitchModeSection     = addSection (cells, "Pitch Mode");
    playbackStyleSection = addSection (cells, "Playback Style");
    timingSection        = addSection (cells, "Timing");

    // Waveform -- pinned at the bottom of the page (its Beats-scoped home),
    // with the old editor's Zoom-to-Trims/Reset-Zoom buttons above it.
    auto zoomToTrims = std::make_unique<juce::TextButton> ("Zoom to Trims");
    zoomToTrims->setSize (zoomToTrims->getBestWidthForHeight (30) + 24, 30);
    zoomToTrims->onClick = [this] { waveform->zoomToTrims(); };

    auto resetZoom = std::make_unique<juce::TextButton> ("Reset Zoom");
    resetZoom->setSize (resetZoom->getBestWidthForHeight (30) + 24, 30);
    resetZoom->onClick = [this] { waveform->resetZoom(); };

    waveform->setSize (1, waveformHeight);

    cells.push_back (ui::spacer (4));
    cells.push_back (ui::row (ui::cell (std::move (zoomToTrims)), ui::cell (std::move (resetZoom))));
    cells.push_back (ui::spacer (10));
    cells.push_back (ui::cell (std::unique_ptr<juce::Component> (waveform)));

    setCells (std::move (cells));

    buildSampleSection();
    buildTempoSection();
    buildDetectionSection();
    buildFadeSection();
    buildPitchModeSection();
    buildPlaybackStyleSection();
    buildTimingSection();

    resized();
}

TitledPanel* BeatsPage::addSection (std::vector<Cell>& cells, juce::String title)
{
    auto panel = std::make_unique<TitledPanel> (std::move (title));
    auto* raw = panel.get();
    cells.push_back (ui::cell (std::move (panel)));
    return raw;
}

//==============================================================================
// Sample -- Load button + file/slice status line.
//==============================================================================
void BeatsPage::buildSampleSection()
{
    auto loadButton = std::make_unique<juce::TextButton> ("Load Sample...");
    loadButton->setSize (loadButton->getBestWidthForHeight (36) + 24, 36);
    loadButton->onClick = [this] { loadSampleWithChooser(); };

    auto status = makeLabel ("");
    status->setJustificationType (juce::Justification::centred);
    status->setSize (200, 30);
    statusLabel = status.get();

    std::vector<Cell> cells;
    cells.push_back (ui::row (ui::cell (std::move (loadButton))));
    cells.push_back (ui::spacer (10));
    cells.push_back (ui::cell (std::move (status)));
    sampleSection->setCells (std::move (cells));
}

//==============================================================================
// Tempo -- Audition, Loop length + calculated BPM, Manual BPM override.
//==============================================================================
void BeatsPage::buildTempoSection()
{
    auto undo = std::make_unique<juce::TextButton> ("Undo");
    undo->setSize (undo->getBestWidthForHeight (30) + 24, 30);
    undo->onClick = [this] { model.undoLastEdit(); };
    undoButton = undo.get();

    auto redo = std::make_unique<juce::TextButton> ("Redo");
    redo->setSize (redo->getBestWidthForHeight (30) + 24, 30);
    redo->onClick = [this] { model.redoLastEdit(); };
    redoButton = redo.get();

    auto resetEdits = std::make_unique<juce::TextButton> ("Reset Edits");
    resetEdits->setSize (resetEdits->getBestWidthForHeight (30) + 24, 30);
    resetEdits->onClick = [this] { model.resetAllManualEdits(); };

    auto audition = std::make_unique<juce::TextButton> ("Audition");
    audition->setSize (audition->getBestWidthForHeight (30) + 24, 30);
    audition->onClick = [this] { engine.setAuditionActive (! model.getAuditionActive()); };
    auditionButton = audition.get();

    auto loopLabel = makeLabel ("Loop length (bars)");
    loopLabel->setSize (110, 20);
    loopLengthLabel = loopLabel.get();

    auto loopSlider = makeSlider (juce::Slider::IncDecButtons, 1.0, 8.0, 1.0, 50, 100, 30);
    loopSlider->setNumDecimalPlacesToDisplay (0);
    loopSlider->setValue (model.getLoopLengthBars(), juce::dontSendNotification);
    loopSlider->onValueChange = [this, s = loopSlider.get()]
    {
        model.setLoopLengthBars ((int) s->getValue());

        const double bpm = model.getCalculatedOriginalBpm();
        calculatedBpmLabel->setText (bpm > 0.0 ? ("~" + juce::String (bpm, 1) + " BPM") : "",
                                     juce::dontSendNotification);

        // Any interaction counts as acknowledgment of the trim-change
        // staleness hint (Step 33).
        setLoopLengthAttention (false);
    };
    loopSlider->onDragEnd = [this] { setLoopLengthAttention (false); };

    auto bpmLabel = makeLabel ("");
    bpmLabel->setSize (200, 20);
    calculatedBpmLabel = bpmLabel.get();

    auto overrideToggle = makeToggle ("Manual BPM override");
    overrideToggle->setToggleState (model.getManualBpmOverrideEnabled(), juce::dontSendNotification);
    auto* overrideToggleRaw = overrideToggle.get();
    overrideToggleRaw->onClick = [this, overrideToggleRaw]
    {
        model.setManualBpmOverrideEnabled (overrideToggleRaw->getToggleState());
        rebuildManualBpmRow();
        refreshAfterSampleOrSliceChange(); // "~X BPM" label refreshes immediately
    };

    auto bpmRow = std::make_unique<Flex> (FlexConfig{ .direction = Direction::row });
    manualBpmRow = bpmRow.get();

    std::vector<Cell> cells;
    cells.push_back (ui::row (ui::cell (std::move (undo)),
                              ui::cell (std::move (redo)),
                              ui::cell (std::move (resetEdits))));
    cells.push_back (ui::spacer (10));
    cells.push_back (ui::row (ui::cell (std::move (audition))));
    cells.push_back (ui::spacer (10));
    cells.push_back (ui::row (ui::cell (std::move (loopLabel)),
                              ui::cell (std::move (loopSlider)),
                              ui::spacer (10),
                              ui::cell (std::move (bpmLabel))));
    cells.push_back (ui::spacer (14));
    cells.push_back (ui::cell (std::move (overrideToggle)));
    cells.push_back (ui::spacer (6));
    cells.push_back (ui::cell (std::move (bpmRow)));
    tempoSection->setCells (std::move (cells));

    rebuildManualBpmRow();
}

void BeatsPage::rebuildManualBpmRow()
{
    if (model.getManualBpmOverrideEnabled())
    {
        auto label = makeLabel ("BPM");
        label->setSize (60, 20);

        auto slider = makeSlider (juce::Slider::LinearHorizontal, 20.0, 300.0, 0.1, 60, 1, 30);
        slider->setValue (model.getManualBpmOverrideValue(), juce::dontSendNotification);
        slider->onValueChange = [this, s = slider.get()]
        {
            model.setManualBpmOverrideValue (s->getValue());
            refreshAfterSampleOrSliceChange(); // "~X BPM" label refreshes live while dragging
        };

        std::vector<Cell> cells;
        cells.push_back (ui::cell (std::move (label)));
        cells.push_back (ui::fill (std::move (slider)));
        manualBpmRow->setCells (std::move (cells));
    }
    else
    {
        manualBpmRow->setCells (std::vector<Cell>{});
    }

    resized();
}

//==============================================================================
// Detection -- transient sensitivity, quantize transients + grid.
//==============================================================================
void BeatsPage::buildDetectionSection()
{
    auto sensLabel = makeLabel ("Transient sensitivity");
    sensLabel->setSize (110, 20);

    auto sensSlider = makeSlider (juce::Slider::IncDecButtons, 0.0, 1.0, 0.01, 50, 100, 30);
    sensSlider->setNumDecimalPlacesToDisplay (2);
    sensSlider->setValue (model.getSensitivity(), juce::dontSendNotification);
    sensSlider->onValueChange = [this, s = sensSlider.get()]
    {
        // While actually dragging, just preview where slices would land --
        // no commit, no sound, no lost probability edits; the real commit
        // happens on release (onDragEnd) or for non-drag changes.
        if (s->isMouseButtonDown())
        {
            waveform->showPreviewSlices (model.previewSlicesAtSensitivity ((float) s->getValue()));
            return;
        }

        model.setSensitivityAndRedetect ((float) s->getValue());
        refreshAfterSampleOrSliceChange();
    };
    sensSlider->onDragEnd = [this, s = sensSlider.get()]
    {
        model.setSensitivityAndRedetect ((float) s->getValue());
        refreshAfterSampleOrSliceChange();
    };

    auto quantizeToggle = makeToggle ("Quantize transients");
    quantizeToggle->setToggleState (model.getQuantizeTransientsEnabled(), juce::dontSendNotification);
    auto* quantizeToggleRaw = quantizeToggle.get();
    quantizeToggleRaw->onClick = [this, quantizeToggleRaw]
    {
        model.setQuantizeTransientsEnabled (quantizeToggleRaw->getToggleState());
        rebuildQuantizeGridRow();
        refreshAfterSampleOrSliceChange();
    };

    auto gridRow = std::make_unique<Flex> (FlexConfig{ .direction = Direction::row });
    quantizeGridRow = gridRow.get();

    std::vector<Cell> cells;
    cells.push_back (ui::row (ui::cell (std::move (sensLabel)), ui::cell (std::move (sensSlider))));
    cells.push_back (ui::spacer (14));
    cells.push_back (ui::cell (std::move (quantizeToggle)));
    cells.push_back (ui::spacer (6));
    cells.push_back (ui::cell (std::move (gridRow)));
    detectionSection->setCells (std::move (cells));

    rebuildQuantizeGridRow();
}

void BeatsPage::rebuildQuantizeGridRow()
{
    if (model.getQuantizeTransientsEnabled())
    {
        auto label = makeLabel ("Grid");
        label->setSize (60, 20);

        auto combo = makeComboBox (150, 24);
        for (int i = 0; i < SlicerModel::numNoteValueOptions; ++i)
            combo->addItem (SlicerModel::getNoteValueName (i), i + 1); // JUCE item IDs are 1-based
        combo->setSelectedId (model.getQuantizeGridIndex() + 1, juce::dontSendNotification);
        combo->onChange = [this, c = combo.get()]
        {
            model.setQuantizeGridIndex (c->getSelectedId() - 1);
            refreshAfterSampleOrSliceChange();
        };

        std::vector<Cell> cells;
        cells.push_back (ui::cell (std::move (label)));
        cells.push_back (ui::cell (std::move (combo)));
        quantizeGridRow->setCells (std::move (cells));
    }
    else
    {
        quantizeGridRow->setCells (std::vector<Cell>{});
    }

    resized();
}

//==============================================================================
// Fade In/Out -- two row sliders.
//==============================================================================
void BeatsPage::buildFadeSection()
{
    auto inLabel = makeLabel ("Fade in (ms)");
    inLabel->setSize (80, 20);

    auto inSlider = makeSlider (juce::Slider::LinearHorizontal, 0.0, 100.0, 0.5, 50, 1, 30);
    inSlider->setValue (model.getFadeInMs(), juce::dontSendNotification);
    inSlider->onValueChange = [this, s = inSlider.get()] { model.setFadeInMs ((float) s->getValue()); };

    auto outLabel = makeLabel ("Fade out (ms)");
    outLabel->setSize (80, 20);

    auto outSlider = makeSlider (juce::Slider::LinearHorizontal, 0.0, 100.0, 0.5, 50, 1, 30);
    outSlider->setValue (model.getFadeOutMs(), juce::dontSendNotification);
    outSlider->onValueChange = [this, s = outSlider.get()] { model.setFadeOutMs ((float) s->getValue()); };

    std::vector<Cell> cells;
    cells.push_back (ui::row (ui::cell (std::move (inLabel)), ui::fill (std::move (inSlider))));
    cells.push_back (ui::spacer (10));
    cells.push_back (ui::row (ui::cell (std::move (outLabel)), ui::fill (std::move (outSlider))));
    fadeSection->setCells (std::move (cells));
}

//==============================================================================
// Pitch Mode -- Repitch / Time-Stretch segment row + mode-specific extras.
//==============================================================================
void BeatsPage::buildPitchModeSection()
{
    auto label = makeLabel ("Pitch mode");
    label->setSize (200, 20);

    auto segments = std::make_unique<SegmentedButtonRow>();
    segments->setSize (400, SegmentedButtonRow::preferredHeight);
    segments->setOptions ({ { "Repitch", std::nullopt }, { "Time-Stretch", std::nullopt } });
    segments->setSelectedIndex (model.getPitchMode() == SlicerModel::PitchMode::timeStretch ? 1 : 0,
                                juce::dontSendNotification);
    segments->onSelectionChanged = [this] (int index)
    {
        engine.setPitchMode (index == 1 ? SlicerModel::PitchMode::timeStretch
                                        : SlicerModel::PitchMode::repitch);
        rebuildPitchModeExtra();
    };

    auto extra = std::make_unique<Flex> (FlexConfig{ .direction = Direction::column });
    pitchModeExtra = extra.get();

    std::vector<Cell> cells;
    cells.push_back (ui::cell (std::move (label)));
    cells.push_back (ui::cell (std::move (segments)));
    cells.push_back (ui::spacer (10));
    cells.push_back (ui::cell (std::move (extra)));
    pitchModeSection->setCells (std::move (cells));

    rebuildPitchModeExtra();
}

void BeatsPage::rebuildPitchModeExtra()
{
    std::vector<Cell> cells;

    if (model.getPitchMode() == SlicerModel::PitchMode::timeStretch)
    {
        auto grainLabel = makeLabel ("Grain size (ms)");
        grainLabel->setSize (110, 20);

        auto grainSlider = makeSlider (juce::Slider::LinearHorizontal, 20.0, 150.0, 1.0, 50, 1, 30);
        grainSlider->setValue (model.getGrainSizeMs(), juce::dontSendNotification);
        grainSlider->onValueChange = [this, s = grainSlider.get()] { model.setGrainSizeMs ((float) s->getValue()); };

        auto windowLabel = makeLabel ("Window shape");
        windowLabel->setSize (110, 20);

        auto windowCombo = makeComboBox (150, 24);
        windowCombo->addItem ("Hann", 1);
        windowCombo->addItem ("Triangular", 2);
        windowCombo->setSelectedId (model.getGrainWindowShape() == SlicerModel::GrainWindowShape::triangular ? 2 : 1,
                                    juce::dontSendNotification);
        windowCombo->onChange = [this, c = windowCombo.get()]
        {
            model.setGrainWindowShape (c->getSelectedId() == 2 ? SlicerModel::GrainWindowShape::triangular
                                                               : SlicerModel::GrainWindowShape::hann);
        };

        auto beatToggle = makeToggle ("Beat-quantize slice length");
        beatToggle->setToggleState (model.getBeatQuantizeSliceLengthEnabled(), juce::dontSendNotification);
        beatToggle->onClick = [this, t = beatToggle.get()] { model.setBeatQuantizeSliceLengthEnabled (t->getToggleState()); };

        auto pitchLabel = makeLabel ("Pitch shift (semitones)");
        pitchLabel->setSize (110, 20);

        auto pitchSlider = makeSlider (juce::Slider::LinearHorizontal, -24.0, 24.0, 1.0, 50, 1, 30);
        pitchSlider->setValue (model.getPitchShiftSemitones(), juce::dontSendNotification);
        pitchSlider->onValueChange = [this, s = pitchSlider.get()] { model.setPitchShiftSemitones ((float) s->getValue()); };

        cells.push_back (ui::row (ui::cell (std::move (grainLabel)), ui::fill (std::move (grainSlider))));
        cells.push_back (ui::spacer (10));
        cells.push_back (ui::row (ui::cell (std::move (windowLabel)), ui::cell (std::move (windowCombo))));
        cells.push_back (ui::spacer (10));
        cells.push_back (ui::cell (std::move (beatToggle)));
        cells.push_back (ui::spacer (10));
        cells.push_back (ui::row (ui::cell (std::move (pitchLabel)), ui::fill (std::move (pitchSlider))));
    }
    else
    {
        auto beatToggleRepitch = makeToggle ("Beat-quantize slice length");
        beatToggleRepitch->setToggleState (model.getBeatQuantizeSliceLengthEnabledRepitch(), juce::dontSendNotification);
        beatToggleRepitch->onClick = [this, t = beatToggleRepitch.get()]
        {
            model.setBeatQuantizeSliceLengthEnabledRepitch (t->getToggleState());
        };

        cells.push_back (ui::cell (std::move (beatToggleRepitch)));
    }

    pitchModeExtra->setCells (std::move (cells));
    resized();
}

//==============================================================================
// Playback Style -- segment row, probability grid, per-style parameter panel.
//==============================================================================
void BeatsPage::buildPlaybackStyleSection()
{
    playbackStyleSection->setCells (std::vector<Cell>{});
    rebuildPlaybackStyleContent();
}

void BeatsPage::rebuildPlaybackStyleContent()
{
    auto segments = std::make_unique<SegmentedButtonRow>();
    segments->setSize (400, SegmentedButtonRow::preferredHeight);
    {
        std::vector<SegmentedButtonRow::Option> styleOptions;
        for (int i = 0; i < SlicerModel::numPlaybackStyleOptions; ++i)
            styleOptions.push_back ({ SlicerModel::getPlaybackStyleName (i), PlaybackStylePalette::getStyleColour (i) });
        segments->setOptions (std::move (styleOptions));
    }
    segments->setSelectedIndex (selectedPlaybackStyle, juce::dontSendNotification);
    segments->onSelectionChanged = [this] (int index)
    {
        selectedPlaybackStyle = index;
        rebuildPlaybackStyleContent(); // parameter panel height is per-style
    };

    auto styleLabel = makeLabel ("Playback style");
    styleLabel->setSize (200, 20);

    auto grid = std::make_unique<PlaybackStyleGrid> (model);
    grid->setSize (1, PlaybackStyleGrid::getPreferredHeight());

    auto paramsLabel = makeLabel ("Style parameters");
    paramsLabel->setSize (200, 20);

    auto params = std::make_unique<PlaybackStyleParameterPanel> (model);
    params->setStyleSelectorVisible (false); // the segment row above is the selector
    params->setSize (1, PlaybackStyleParameterPanel::getPreferredHeightForStyle (selectedPlaybackStyle));

    std::vector<Cell> cells;
    cells.push_back (ui::cell (std::move (segments)));
    cells.push_back (ui::spacer (10));
    cells.push_back (ui::cell (std::move (styleLabel)));
    cells.push_back (ui::cell (std::move (grid)));
    cells.push_back (ui::spacer (14));
    cells.push_back (ui::cell (std::move (paramsLabel)));
    cells.push_back (ui::cell (std::move (params)));
    playbackStyleSection->setCells (std::move (cells));

    resized();
}

//==============================================================================
// Timing -- Slice Length / Clock segment row + mode-specific rows.
//==============================================================================
void BeatsPage::buildTimingSection()
{
    auto label = makeLabel ("Timing");
    label->setSize (200, 20);

    auto segments = std::make_unique<SegmentedButtonRow>();
    segments->setSize (400, SegmentedButtonRow::preferredHeight);
    segments->setOptions ({ { "Slice Length", std::nullopt }, { "Clock", std::nullopt } });
    segments->setSelectedIndex (model.getTriggerMode() == SlicerModel::TriggerMode::clock ? 1 : 0,
                                juce::dontSendNotification);
    segments->onSelectionChanged = [this] (int index)
    {
        const auto mode = index == 1 ? SlicerModel::TriggerMode::clock
                                     : SlicerModel::TriggerMode::sliceLength;
        lastGenerateTriggerMode = mode;
        if (model.getTriggerMode() != mode)
            engine.setTriggerMode (mode); // guarded: setTriggerMode() resets init state even on a no-op
        rebuildTimingExtra();
    };

    auto extra = std::make_unique<Flex> (FlexConfig{ .direction = Direction::column });
    timingExtra = extra.get();

    std::vector<Cell> cells;
    cells.push_back (ui::cell (std::move (label)));
    cells.push_back (ui::cell (std::move (segments)));
    cells.push_back (ui::spacer (10));
    cells.push_back (ui::cell (std::move (extra)));
    timingSection->setCells (std::move (cells));

    rebuildTimingExtra();
}

void BeatsPage::rebuildTimingExtra()
{
    if (lastGenerateTriggerMode == SlicerModel::TriggerMode::clock)
        timingExtra->setCells (makeTimingClockCells());
    else
        timingExtra->setCells (makeTimingSliceLengthCells());
    resized();
}

std::vector<Cell> BeatsPage::makeTimingClockCells()
{
    std::vector<Cell> cells;

    auto clockRefLabel = makeLabel ("Clock reference");
    clockRefLabel->setSize (140, 20);

    auto clockRefCombo = makeComboBox (150, 24);
    for (int i = 0; i < SlicerModel::numNoteValueOptions; ++i)
        clockRefCombo->addItem (SlicerModel::getNoteValueName (i), i + 1);
    clockRefCombo->setSelectedId (model.getClockReferenceIndex() + 1, juce::dontSendNotification);
    clockRefCombo->onChange = [this, c = clockRefCombo.get()] { model.setClockReferenceIndex (c->getSelectedId() - 1); };

    cells.push_back (ui::row (ui::cell (std::move (clockRefLabel)), ui::cell (std::move (clockRefCombo))));
    cells.push_back (ui::spacer (10));

    auto tapeStopLabel = makeLabel ("Tape Stop scope");
    tapeStopLabel->setSize (140, 20);

    auto tapeStopCombo = makeComboBox (150, 24);
    for (int i = 0; i < SlicerModel::numTapeStopScopeOptions; ++i)
        tapeStopCombo->addItem (SlicerModel::getTapeStopScopeName (i), i + 1);
    tapeStopCombo->setSelectedId (model.getTapeStopScope() == SlicerModel::TapeStopScope::perTick ? 2 : 1,
                                  juce::dontSendNotification);
    tapeStopCombo->onChange = [this, c = tapeStopCombo.get()]
    {
        model.setTapeStopScope (c->getSelectedId() == 2 ? SlicerModel::TapeStopScope::perTick
                                                        : SlicerModel::TapeStopScope::wholeWindow);
    };

    cells.push_back (ui::row (ui::cell (std::move (tapeStopLabel)), ui::cell (std::move (tapeStopCombo))));
    cells.push_back (ui::spacer (10));

    auto filterSweepLabel = makeLabel ("Filter Sweep scope");
    filterSweepLabel->setSize (140, 20);

    auto filterSweepCombo = makeComboBox (150, 24);
    for (int i = 0; i < SlicerModel::numFilterSweepScopeOptions; ++i)
        filterSweepCombo->addItem (SlicerModel::getFilterSweepScopeName (i), i + 1);
    filterSweepCombo->setSelectedId (model.getFilterSweepScope() == SlicerModel::FilterSweepScope::perTick ? 2 : 1,
                                     juce::dontSendNotification);
    filterSweepCombo->onChange = [this, c = filterSweepCombo.get()]
    {
        model.setFilterSweepScope (c->getSelectedId() == 2 ? SlicerModel::FilterSweepScope::perTick
                                                           : SlicerModel::FilterSweepScope::wholeWindow);
    };

    cells.push_back (ui::row (ui::cell (std::move (filterSweepLabel)), ui::cell (std::move (filterSweepCombo))));
    cells.push_back (ui::spacer (10));

    auto subdivLabel = makeLabel ("Subdivision probability");
    subdivLabel->setSize (200, 20);

    auto subdivGrid = std::make_unique<SubdivisionProbabilityGrid> (model);
    subdivGrid->setSize (1, SubdivisionProbabilityGrid::getPreferredHeight());

    cells.push_back (ui::cell (std::move (subdivLabel)));
    cells.push_back (ui::spacer (6));
    cells.push_back (ui::cell (std::move (subdivGrid)));

    return cells;
}

std::vector<Cell> BeatsPage::makeTimingSliceLengthCells()
{
    std::vector<Cell> cells;

    auto resetEveryLabel = makeLabel ("Reset every");
    resetEveryLabel->setSize (140, 20);

    auto resetEveryCombo = makeComboBox (150, 24);
    for (int i = 0; i < SlicerModel::numResetBarsOptions; ++i)
        resetEveryCombo->addItem (SlicerModel::getResetBarsName (i), i + 1);
    resetEveryCombo->setSelectedId (model.getResetBarsIndex() + 1, juce::dontSendNotification);
    resetEveryCombo->onChange = [this, c = resetEveryCombo.get()] { model.setResetBarsIndex (c->getSelectedId() - 1); };

    cells.push_back (ui::row (ui::cell (std::move (resetEveryLabel)), ui::cell (std::move (resetEveryCombo))));

    return cells;
}

//==============================================================================
void BeatsPage::syncFromModel()
{
    undoButton->setEnabled (model.canUndoEdit());
    redoButton->setEnabled (model.canRedoEdit());

    // Polled rather than driven only by the button's own click, since the
    // engine can also stop an audition on its own (host transport started).
    const bool auditioning = model.getAuditionActive();
    auditionButton->setButtonText (auditioning ? "Stop Audition" : "Audition");
    auditionButton->setColour (juce::TextButton::buttonColourId,
                               auditioning ? juce::Colours::orange.withAlpha (0.6f)
                                           : getLookAndFeel().findColour (juce::TextButton::buttonColourId));
}

void BeatsPage::refreshAfterSampleOrSliceChange()
{
    const int numSlices = model.getNumSlices();
    const juce::String text = model.hasSample()
        ? model.getLoadedFileName() + "  —  " + juce::String (numSlices) + " slice" + (numSlices == 1 ? "" : "s")
        : "No sample loaded";
    statusLabel->setText (text, juce::dontSendNotification);

    const double bpm = model.getCalculatedOriginalBpm();
    calculatedBpmLabel->setText (bpm > 0.0 ? ("~" + juce::String (bpm, 1) + " BPM") : "",
                                 juce::dontSendNotification);

    waveform->refresh();
}

void BeatsPage::setLoopLengthAttention (bool attention)
{
    loopLengthLabel->setColour (juce::Label::textColourId,
                                attention ? juce::Colours::orange : NeditPalette::textOnTungsten);
}

void BeatsPage::loadSampleWithChooser()
{
    fileChooser = std::make_unique<juce::FileChooser> (
        "Select an audio sample to load...",
        juce::File(),
        "*.wav;*.aif;*.aiff;*.flac");

    const auto chooserFlags = juce::FileBrowserComponent::openMode
                            | juce::FileBrowserComponent::canSelectFiles;

    fileChooser->launchAsync (chooserFlags, [this] (const juce::FileChooser& fc)
    {
        const auto file = fc.getResult();

        if (file.existsAsFile())
        {
            model.loadSample (file);
            refreshAfterSampleOrSliceChange();
        }
    });
}

//==============================================================================
// Leaf factories -- fixed sizes per the DSL's "leaf sets own size" rule.
//==============================================================================
std::unique_ptr<juce::Label> BeatsPage::makeLabel (const juce::String& text)
{
    auto label = std::make_unique<juce::Label> ("", text);
    label->setSize (200, 20);
    label->setJustificationType (juce::Justification::centredLeft);
    return label;
}

std::unique_ptr<juce::Slider> BeatsPage::makeSlider (juce::Slider::SliderStyle style,
                                                     double min, double max, double interval,
                                                     int textBoxWidth, int width, int height)
{
    auto slider = std::make_unique<juce::Slider>();
    slider->setSliderStyle (style);
    slider->setScrollWheelEnabled (false);
    slider->setRange (min, max, interval);
    slider->setTextBoxStyle (style == juce::Slider::IncDecButtons ? juce::Slider::TextBoxLeft
                                                                  : juce::Slider::TextBoxRight,
                             false, textBoxWidth, 20);
    slider->setSize (width, height);
    return slider;
}

std::unique_ptr<juce::ComboBox> BeatsPage::makeComboBox (int width, int height)
{
    auto combo = std::make_unique<juce::ComboBox>();
    combo->setSize (width, height);
    return combo;
}

std::unique_ptr<juce::ToggleButton> BeatsPage::makeToggle (const juce::String& text)
{
    auto toggle = std::make_unique<juce::ToggleButton> (text);
    toggle->setSize (200, 24);
    return toggle;
}

} // namespace ui
