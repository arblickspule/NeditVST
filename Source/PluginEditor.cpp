#include "PluginProcessor.h"
#include "PluginEditor.h"

void SlicerAudioProcessorEditor::ComingSoonPanel::paint (juce::Graphics& g)
{
    g.setColour (NeditPalette::tungsten);
    g.fillRect (getLocalBounds());

    g.setColour (NeditPalette::textOnTungsten.withAlpha (0.6f));
    g.setFont (16.0f);
    g.drawFittedText (message, getLocalBounds(), juce::Justification::centred, 1);
}

SlicerAudioProcessorEditor::SlicerAudioProcessorEditor (SlicerAudioProcessor& p)
    : AudioProcessorEditor (&p), processor (p),
      playbackStyleGrid (p), playbackStyleParameterPanel (p), subdivisionGrid (p), playbackStylePalette (p),
      sequencerBankSource (p), patternBankPanel (sequencerBankSource),
      performanceStyleParameterPanel (p,
          [&p] (int i) { return p.getPerformanceWorkingParameterValue (i); },
          [&p] (int i, float v) { p.setPerformanceWorkingParameterValue (i, v); },
          p.getPerformanceWorkingStyle() + 1,
          [&p] (int style) { p.setPerformanceWorkingStyle (style); }),
      performanceKeyboardSource (p), performanceKeyboardPanel (performanceKeyboardSource),
      sequencerGrid (p), waveformDisplay (p)
{
    addAndMakeVisible (controlsViewport);
    controlsViewport.setViewedComponent (&controlsContent, false); // we own it, don't let the viewport delete it
    controlsViewport.setScrollBarsShown (true, false); // vertical only, shown when needed
    controlsContent.setLookAndFeel (&neditLookAndFeel); // Tungsten/Salmon palette for every native-widget control below (Pass 1)

    // Pass 1 tab structure -- added first so every page/section beneath it
    // paints on top (SectionPanel/ComingSoonPanel are pure backdrops, added
    // here purely for correct paint z-order, not for layout).
    controlsContent.addAndMakeVisible (topLevelModeTabs);
    topLevelModeTabs.setOptions ({ { "Beats", std::nullopt }, { "Textures", std::nullopt } });

    controlsContent.addAndMakeVisible (subModeTabs);
    subModeTabs.setOptions ({ { "Generate", std::nullopt }, { "Sequence", std::nullopt },
                               { "Control", std::nullopt }, { "Perform", std::nullopt } });

    // Seed both tab rows from the processor's own current trigger mode so
    // the initial syncTriggerModeToActiveTab() call below is a no-op --
    // e.g. if the processor is already in Performance mode (persisted
    // state), land on Beats>Perform at startup rather than silently
    // resetting it back to Slice Length.
    {
        const auto mode = processor.getTriggerMode();
        const int subModeIndex = mode == SlicerAudioProcessor::TriggerMode::sequenced ? 1
                                : mode == SlicerAudioProcessor::TriggerMode::performance ? 3
                                                                                          : 0; // Generate
        subModeTabs.setSelectedIndex (subModeIndex, juce::dontSendNotification);

        if (mode == SlicerAudioProcessor::TriggerMode::clock)
            lastGenerateTriggerMode = SlicerAudioProcessor::TriggerMode::clock;
    }

    topLevelModeTabs.onSelectionChanged = [this] (int) { updateActiveTabVisibility(); syncTriggerModeToActiveTab(); resized(); };
    subModeTabs.onSelectionChanged = [this] (int) { updateActiveTabVisibility(); syncTriggerModeToActiveTab(); resized(); };

    controlsContent.addAndMakeVisible (sampleSectionPanel);
    controlsContent.addAndMakeVisible (trimTempoSectionPanel);
    controlsContent.addAndMakeVisible (detectionSectionPanel);
    controlsContent.addAndMakeVisible (engineSectionPanel);
    controlsContent.addAndMakeVisible (playbackStyleSectionPanel);
    controlsContent.addAndMakeVisible (controlPlaceholder);
    controlsContent.addAndMakeVisible (texturesPlaceholder);

    controlsContent.addAndMakeVisible (loadButton);
    loadButton.addListener (this);

    controlsContent.addAndMakeVisible (resetEditsButton);
    resetEditsButton.addListener (this);

    controlsContent.addAndMakeVisible (undoButton);
    undoButton.addListener (this);
    undoButton.setEnabled (false);

    controlsContent.addAndMakeVisible (redoButton);
    redoButton.addListener (this);
    redoButton.setEnabled (false);

    controlsContent.addAndMakeVisible (auditionButton);
    auditionButton.addListener (this);

    startTimerHz (10); // keeps Undo/Redo enabled-state (and the Audition button's label/colour) in sync with the processor

    controlsContent.addAndMakeVisible (statusLabel);
    statusLabel.setJustificationType (juce::Justification::centred);
    statusLabel.setText (processor.hasSample() ? processor.getLoadedFileName()
                                                : "No sample loaded",
                          juce::dontSendNotification);

    controlsContent.addAndMakeVisible (loopLengthLabel);
    loopLengthLabel.setText ("Loop length (bars)", juce::dontSendNotification);
    loopLengthLabel.setJustificationType (juce::Justification::centredLeft);

    controlsContent.addAndMakeVisible (loopLengthKnob);
    loopLengthKnob.setRange (1.0, 8.0, 1.0);
    loopLengthKnob.setNumDecimalPlacesToDisplay (0);
    loopLengthKnob.setLabel ("bars");
    loopLengthKnob.setValue (processor.getLoopLengthBars(), juce::dontSendNotification);
    loopLengthKnob.onValueChange = [this]
    {
        processor.setLoopLengthBars ((int) loopLengthKnob.getValue());

        const double bpm = processor.getCalculatedOriginalBpm();
        calculatedBpmLabel.setText (bpm > 0.0 ? ("~" + juce::String (bpm, 1) + " BPM") : "",
                                     juce::dontSendNotification);

        // Any interaction with this control counts as acknowledgment
        // (Step 33) -- even re-entering the same value -- so the
        // staleness highlight clears the moment the user looks at it,
        // not only when the value actually changes.
        loopLengthNeedsAttention = false;
        repaint();
    };
    loopLengthKnob.onDragEnd = [this]
    {
        loopLengthNeedsAttention = false;
        repaint();
    };

    controlsContent.addAndMakeVisible (calculatedBpmLabel);
    calculatedBpmLabel.setJustificationType (juce::Justification::centredLeft);

    controlsContent.addAndMakeVisible (manualBpmOverrideToggle);
    manualBpmOverrideToggle.setToggleState (processor.getManualBpmOverrideEnabled(), juce::dontSendNotification);
    manualBpmOverrideToggle.onClick = [this]
    {
        processor.setManualBpmOverrideEnabled (manualBpmOverrideToggle.getToggleState());
        updateManualBpmOverrideVisibility();
        updateAfterSampleOrSliceChange(); // refreshes the "~X BPM" label immediately
    };

    controlsContent.addAndMakeVisible (manualBpmOverrideLabel);
    manualBpmOverrideLabel.setText ("BPM", juce::dontSendNotification);
    manualBpmOverrideLabel.setJustificationType (juce::Justification::centredLeft);

    controlsContent.addAndMakeVisible (manualBpmOverrideSlider);
    manualBpmOverrideSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    manualBpmOverrideSlider.setScrollWheelEnabled (false); // scrolling the controlsViewport over this shouldn't also nudge the value
    manualBpmOverrideSlider.setRange (20.0, 300.0, 0.1);
    manualBpmOverrideSlider.setValue (processor.getManualBpmOverrideValue(), juce::dontSendNotification);
    manualBpmOverrideSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 60, 20);
    manualBpmOverrideSlider.onValueChange = [this]
    {
        processor.setManualBpmOverrideValue (manualBpmOverrideSlider.getValue());
        updateAfterSampleOrSliceChange(); // refreshes the "~X BPM" label live while dragging
    };

    controlsContent.addAndMakeVisible (pitchModeLabel);
    pitchModeLabel.setText ("Pitch mode", juce::dontSendNotification);
    pitchModeLabel.setJustificationType (juce::Justification::centredLeft);

    controlsContent.addAndMakeVisible (pitchModeSegments);
    pitchModeSegments.setOptions ({ { "Repitch", std::nullopt }, { "Time-Stretch", std::nullopt } });
    {
        const auto currentMode = processor.getPitchMode();
        const int selectedIndex = currentMode == SlicerAudioProcessor::PitchMode::timeStretch ? 1 : 0;
        pitchModeSegments.setSelectedIndex (selectedIndex, juce::dontSendNotification);
    }
    pitchModeSegments.onSelectionChanged = [this] (int selectedIndex)
    {
        const auto mode = selectedIndex == 1 ? SlicerAudioProcessor::PitchMode::timeStretch
                                              : SlicerAudioProcessor::PitchMode::repitch;
        processor.setPitchMode (mode);
        updatePitchModeVisibility();
    };

    controlsContent.addAndMakeVisible (beatQuantizeToggleRepitch);
    beatQuantizeToggleRepitch.setToggleState (processor.getBeatQuantizeSliceLengthEnabledRepitch(), juce::dontSendNotification);
    beatQuantizeToggleRepitch.onClick = [this]
    {
        processor.setBeatQuantizeSliceLengthEnabledRepitch (beatQuantizeToggleRepitch.getToggleState());
    };

    controlsContent.addAndMakeVisible (grainSizeLabel);
    grainSizeLabel.setText ("Grain size (ms)", juce::dontSendNotification);
    grainSizeLabel.setJustificationType (juce::Justification::centredLeft);

    controlsContent.addAndMakeVisible (grainSizeSlider);
    grainSizeSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    grainSizeSlider.setScrollWheelEnabled (false); // scrolling the controlsViewport over this shouldn't also nudge the value
    grainSizeSlider.setRange (20.0, 150.0, 1.0);
    grainSizeSlider.setValue (processor.getGrainSizeMs(), juce::dontSendNotification);
    grainSizeSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 50, 20);
    grainSizeSlider.onValueChange = [this]
    {
        processor.setGrainSizeMs ((float) grainSizeSlider.getValue());
    };

    controlsContent.addAndMakeVisible (windowShapeLabel);
    windowShapeLabel.setText ("Window shape", juce::dontSendNotification);
    windowShapeLabel.setJustificationType (juce::Justification::centredLeft);

    controlsContent.addAndMakeVisible (windowShapeSelector);
    windowShapeSelector.addItem ("Hann", 1);
    windowShapeSelector.addItem ("Triangular", 2);
    windowShapeSelector.setSelectedId (processor.getGrainWindowShape() == SlicerAudioProcessor::GrainWindowShape::triangular ? 2 : 1,
                                        juce::dontSendNotification);
    windowShapeSelector.onChange = [this]
    {
        const bool triangular = windowShapeSelector.getSelectedId() == 2;
        processor.setGrainWindowShape (triangular ? SlicerAudioProcessor::GrainWindowShape::triangular
                                                    : SlicerAudioProcessor::GrainWindowShape::hann);
    };

    controlsContent.addAndMakeVisible (beatQuantizeToggle);
    beatQuantizeToggle.setToggleState (processor.getBeatQuantizeSliceLengthEnabled(), juce::dontSendNotification);
    beatQuantizeToggle.onClick = [this]
    {
        processor.setBeatQuantizeSliceLengthEnabled (beatQuantizeToggle.getToggleState());
    };

    controlsContent.addAndMakeVisible (pitchShiftLabel);
    pitchShiftLabel.setText ("Pitch shift (semitones)", juce::dontSendNotification);
    pitchShiftLabel.setJustificationType (juce::Justification::centredLeft);

    controlsContent.addAndMakeVisible (pitchShiftSlider);
    pitchShiftSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    pitchShiftSlider.setScrollWheelEnabled (false); // scrolling the controlsViewport over this shouldn't also nudge the value
    pitchShiftSlider.setRange (-24.0, 24.0, 1.0);
    pitchShiftSlider.setValue (processor.getPitchShiftSemitones(), juce::dontSendNotification);
    pitchShiftSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 50, 20);
    pitchShiftSlider.onValueChange = [this]
    {
        processor.setPitchShiftSemitones ((float) pitchShiftSlider.getValue());
    };

    controlsContent.addAndMakeVisible (sensitivityLabel);
    sensitivityLabel.setText ("Transient sensitivity", juce::dontSendNotification);
    sensitivityLabel.setJustificationType (juce::Justification::centredLeft);

    controlsContent.addAndMakeVisible (sensitivityKnob);
    sensitivityKnob.setRange (0.0, 1.0, 0.01);
    sensitivityKnob.setNumDecimalPlacesToDisplay (2);
    sensitivityKnob.setValue (processor.getSensitivity(), juce::dontSendNotification);
    sensitivityKnob.onValueChange = [this]
    {
        // Committing (which resets every slice probability and briefly
        // restarts the chain) on EVERY value change would fire many times
        // a second while the user scrubs the slider — that's what was
        // causing the "rapid retriggering" glitch, and it was also
        // silently wiping any probability edits made on the waveform with
        // every pixel of movement. While actually dragging, just show a
        // live PREVIEW of where slices would land — no commit, no sound,
        // no lost edits — and only commit for real on release (onDragEnd)
        // or for non-drag changes.
        if (sensitivityKnob.isDragging())
        {
            auto preview = processor.previewSlicesAtSensitivity ((float) sensitivityKnob.getValue());
            waveformDisplay.showPreviewSlices (preview);
            return;
        }

        processor.setSensitivityAndRedetect ((float) sensitivityKnob.getValue());
        updateAfterSampleOrSliceChange();
    };
    sensitivityKnob.onDragEnd = [this]
    {
        processor.setSensitivityAndRedetect ((float) sensitivityKnob.getValue());
        updateAfterSampleOrSliceChange();
    };

    controlsContent.addAndMakeVisible (quantizeTransientsToggle);
    quantizeTransientsToggle.setToggleState (processor.getQuantizeTransientsEnabled(), juce::dontSendNotification);
    quantizeTransientsToggle.onClick = [this]
    {
        processor.setQuantizeTransientsEnabled (quantizeTransientsToggle.getToggleState());
        updateQuantizeTransientsVisibility();
        updateAfterSampleOrSliceChange();
    };

    controlsContent.addAndMakeVisible (quantizeGridLabel);
    quantizeGridLabel.setText ("Grid", juce::dontSendNotification);
    quantizeGridLabel.setJustificationType (juce::Justification::centredLeft);

    controlsContent.addAndMakeVisible (quantizeGridSelector);
    for (int i = 0; i < SlicerAudioProcessor::numNoteValueOptions; ++i)
        quantizeGridSelector.addItem (SlicerAudioProcessor::getNoteValueName (i), i + 1); // JUCE item IDs are 1-based
    quantizeGridSelector.setSelectedId (processor.getQuantizeGridIndex() + 1, juce::dontSendNotification);
    quantizeGridSelector.onChange = [this]
    {
        processor.setQuantizeGridIndex (quantizeGridSelector.getSelectedId() - 1);
        updateAfterSampleOrSliceChange();
    };

    controlsContent.addAndMakeVisible (fadeInLabel);
    fadeInLabel.setText ("Fade in (ms)", juce::dontSendNotification);
    fadeInLabel.setJustificationType (juce::Justification::centredLeft);

    controlsContent.addAndMakeVisible (fadeInSlider);
    fadeInSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    fadeInSlider.setScrollWheelEnabled (false); // scrolling the controlsViewport over this shouldn't also nudge the value
    fadeInSlider.setRange (0.0, 100.0, 0.5);
    fadeInSlider.setValue (processor.getFadeInMs(), juce::dontSendNotification);
    fadeInSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 50, 20);
    fadeInSlider.onValueChange = [this]
    {
        processor.setFadeInMs ((float) fadeInSlider.getValue());
    };

    controlsContent.addAndMakeVisible (fadeOutLabel);
    fadeOutLabel.setText ("Fade out (ms)", juce::dontSendNotification);
    fadeOutLabel.setJustificationType (juce::Justification::centredLeft);

    controlsContent.addAndMakeVisible (fadeOutSlider);
    fadeOutSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    fadeOutSlider.setScrollWheelEnabled (false); // scrolling the controlsViewport over this shouldn't also nudge the value
    fadeOutSlider.setRange (0.0, 100.0, 0.5);
    fadeOutSlider.setValue (processor.getFadeOutMs(), juce::dontSendNotification);
    fadeOutSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 50, 20);
    fadeOutSlider.onValueChange = [this]
    {
        processor.setFadeOutMs ((float) fadeOutSlider.getValue());
    };

    controlsContent.addAndMakeVisible (triggerModeLabel);
    triggerModeLabel.setText ("Trigger mode", juce::dontSendNotification);
    triggerModeLabel.setJustificationType (juce::Justification::centredLeft);

    controlsContent.addAndMakeVisible (triggerModeSegments);
    triggerModeSegments.setOptions ({ { "Slice Length", std::nullopt }, { "Clock", std::nullopt }, { "Sequenced", std::nullopt } });
    {
        const auto currentMode = processor.getTriggerMode();
        const int selectedIndex = currentMode == SlicerAudioProcessor::TriggerMode::clock ? 1
                                 : currentMode == SlicerAudioProcessor::TriggerMode::sequenced ? 2
                                                                                                : 0;
        triggerModeSegments.setSelectedIndex (selectedIndex, juce::dontSendNotification);
    }
    triggerModeSegments.onSelectionChanged = [this] (int selectedIndex)
    {
        if (selectedIndex == 2) // Sequenced
        {
            // No Sequenced-specific controls render on Generate this pass
            // (they live on the Sequence tab) -- jump there directly.
            // subModeTabs' own onSelectionChanged calls
            // syncTriggerModeToActiveTab() for us, which sets
            // processor.setTriggerMode(sequenced) faithfully (including the
            // wasPerformance trim-rebuild side effect, if applicable).
            subModeTabs.setSelectedIndex (1, juce::sendNotification);
            return;
        }

        lastGenerateTriggerMode = selectedIndex == 1 ? SlicerAudioProcessor::TriggerMode::clock
                                                      : SlicerAudioProcessor::TriggerMode::sliceLength;
        syncTriggerModeToActiveTab();
    };

    controlsContent.addAndMakeVisible (playbackStyleSegments);
    {
        std::vector<SegmentedButtonRow::Option> styleOptions;
        for (int i = 0; i < SlicerAudioProcessor::numPlaybackStyleOptions; ++i)
            styleOptions.push_back ({ SlicerAudioProcessor::getPlaybackStyleName (i), PlaybackStylePalette::getStyleColour (i) });
        playbackStyleSegments.setOptions (std::move (styleOptions));
    }
    playbackStyleSegments.onSelectionChanged = [this] (int selectedIndex)
    {
        playbackStyleParameterPanel.setSelectedStyle (selectedIndex);
        playbackStyleParameterPanel.repaint();
    };

    controlsContent.addAndMakeVisible (playbackStyleLabel);
    playbackStyleLabel.setText ("Playback style", juce::dontSendNotification);
    playbackStyleLabel.setJustificationType (juce::Justification::centredLeft);

    controlsContent.addAndMakeVisible (playbackStyleGrid);

    controlsContent.addAndMakeVisible (playbackStyleParametersLabel);
    playbackStyleParametersLabel.setText ("Style parameters", juce::dontSendNotification);
    playbackStyleParametersLabel.setJustificationType (juce::Justification::centredLeft);

    controlsContent.addAndMakeVisible (playbackStyleParameterPanel);
    // playbackStyleSegments above is the real selector now -- hide the
    // panel's own internal style ComboBox so the two don't sit redundantly
    // on top of each other (see setStyleSelectorVisible()'s own doc comment).
    playbackStyleParameterPanel.setStyleSelectorVisible (false);

    controlsContent.addAndMakeVisible (clockReferenceLabel);
    clockReferenceLabel.setText ("Clock reference", juce::dontSendNotification);
    clockReferenceLabel.setJustificationType (juce::Justification::centredLeft);

    controlsContent.addAndMakeVisible (clockReferenceSelector);
    for (int i = 0; i < SlicerAudioProcessor::numNoteValueOptions; ++i)
        clockReferenceSelector.addItem (SlicerAudioProcessor::getNoteValueName (i), i + 1); // JUCE item IDs are 1-based
    clockReferenceSelector.setSelectedId (processor.getClockReferenceIndex() + 1, juce::dontSendNotification);
    clockReferenceSelector.onChange = [this]
    {
        processor.setClockReferenceIndex (clockReferenceSelector.getSelectedId() - 1);
    };

    controlsContent.addAndMakeVisible (tapeStopScopeLabel);
    tapeStopScopeLabel.setText ("Tape Stop scope", juce::dontSendNotification);
    tapeStopScopeLabel.setJustificationType (juce::Justification::centredLeft);

    controlsContent.addAndMakeVisible (tapeStopScopeSelector);
    for (int i = 0; i < SlicerAudioProcessor::numTapeStopScopeOptions; ++i)
        tapeStopScopeSelector.addItem (SlicerAudioProcessor::getTapeStopScopeName (i), i + 1); // JUCE item IDs are 1-based
    tapeStopScopeSelector.setSelectedId (processor.getTapeStopScope() == SlicerAudioProcessor::TapeStopScope::perTick ? 2 : 1,
                                          juce::dontSendNotification);
    tapeStopScopeSelector.onChange = [this]
    {
        processor.setTapeStopScope (tapeStopScopeSelector.getSelectedId() == 2
                                         ? SlicerAudioProcessor::TapeStopScope::perTick
                                         : SlicerAudioProcessor::TapeStopScope::wholeWindow);
    };

    controlsContent.addAndMakeVisible (filterSweepScopeLabel);
    filterSweepScopeLabel.setText ("Filter Sweep scope", juce::dontSendNotification);
    filterSweepScopeLabel.setJustificationType (juce::Justification::centredLeft);

    controlsContent.addAndMakeVisible (filterSweepScopeSelector);
    for (int i = 0; i < SlicerAudioProcessor::numFilterSweepScopeOptions; ++i)
        filterSweepScopeSelector.addItem (SlicerAudioProcessor::getFilterSweepScopeName (i), i + 1); // JUCE item IDs are 1-based
    filterSweepScopeSelector.setSelectedId (processor.getFilterSweepScope() == SlicerAudioProcessor::FilterSweepScope::perTick ? 2 : 1,
                                             juce::dontSendNotification);
    filterSweepScopeSelector.onChange = [this]
    {
        processor.setFilterSweepScope (filterSweepScopeSelector.getSelectedId() == 2
                                            ? SlicerAudioProcessor::FilterSweepScope::perTick
                                            : SlicerAudioProcessor::FilterSweepScope::wholeWindow);
    };

    controlsContent.addAndMakeVisible (resetEveryLabel);
    resetEveryLabel.setText ("Reset every", juce::dontSendNotification);
    resetEveryLabel.setJustificationType (juce::Justification::centredLeft);

    controlsContent.addAndMakeVisible (resetEverySelector);
    for (int i = 0; i < SlicerAudioProcessor::numResetBarsOptions; ++i)
        resetEverySelector.addItem (SlicerAudioProcessor::getResetBarsName (i), i + 1); // JUCE item IDs are 1-based
    resetEverySelector.setSelectedId (processor.getResetBarsIndex() + 1, juce::dontSendNotification);
    resetEverySelector.onChange = [this]
    {
        processor.setResetBarsIndex (resetEverySelector.getSelectedId() - 1);
    };

    controlsContent.addAndMakeVisible (subdivisionTableLabel);
    subdivisionTableLabel.setText ("Subdivision probability", juce::dontSendNotification);
    subdivisionTableLabel.setJustificationType (juce::Justification::centredLeft);

    controlsContent.addAndMakeVisible (subdivisionGrid);

    controlsContent.addAndMakeVisible (stepResolutionLabel);
    stepResolutionLabel.setText ("Step resolution", juce::dontSendNotification);
    stepResolutionLabel.setJustificationType (juce::Justification::centredLeft);

    controlsContent.addAndMakeVisible (stepResolutionSelector);
    for (int i = 0; i < SlicerAudioProcessor::numNoteValueOptions; ++i)
        stepResolutionSelector.addItem (SlicerAudioProcessor::getNoteValueName (i), i + 1); // JUCE item IDs are 1-based
    stepResolutionSelector.setSelectedId (processor.getStepResolutionIndex() + 1, juce::dontSendNotification);
    stepResolutionSelector.onChange = [this]
    {
        processor.setStepResolutionIndex (stepResolutionSelector.getSelectedId() - 1);
    };

    controlsContent.addAndMakeVisible (patternLengthLabel);
    patternLengthLabel.setText ("Pattern length (bars)", juce::dontSendNotification);
    patternLengthLabel.setJustificationType (juce::Justification::centredLeft);

    controlsContent.addAndMakeVisible (patternLengthSelector);
    for (int i = 0; i < SlicerAudioProcessor::numPatternLengthBarsOptions; ++i)
        patternLengthSelector.addItem (SlicerAudioProcessor::getPatternLengthBarsName (i), i + 1); // JUCE item IDs are 1-based
    patternLengthSelector.setSelectedId (processor.getPatternLengthBarsIndex() + 1, juce::dontSendNotification);
    patternLengthSelector.onChange = [this]
    {
        processor.setPatternLengthBarsIndex (patternLengthSelector.getSelectedId() - 1);
    };

    controlsContent.addAndMakeVisible (randomizeSequenceButton);
    randomizeSequenceButton.addListener (this);

    controlsContent.addAndMakeVisible (clearSequenceButton);
    clearSequenceButton.addListener (this);

    controlsContent.addAndMakeVisible (playbackStylePalette);
    controlsContent.addAndMakeVisible (patternBankPanel);

    // Pattern Switch Timing (Pass 2) -- governs when a pattern-bank recall
    // note-on (see patternBankPanel just above) actually takes effect.
    // Item IDs are 1-based, in PatternSwitchTiming enum order.
    controlsContent.addAndMakeVisible (patternSwitchTimingLabel);
    patternSwitchTimingLabel.setText ("Pattern switch timing", juce::dontSendNotification);
    patternSwitchTimingLabel.setJustificationType (juce::Justification::centredLeft);

    controlsContent.addAndMakeVisible (patternSwitchTimingSelector);
    patternSwitchTimingSelector.addItem ("Immediate", 1);
    patternSwitchTimingSelector.addItem ("Set Interval", 2);
    patternSwitchTimingSelector.addItem ("End of Pattern", 3);
    patternSwitchTimingSelector.setSelectedId (
        static_cast<int> (processor.getPatternSwitchTiming()) + 1, juce::dontSendNotification);
    patternSwitchTimingSelector.onChange = [this]
    {
        processor.setPatternSwitchTiming (
            static_cast<SlicerAudioProcessor::PatternSwitchTiming> (patternSwitchTimingSelector.getSelectedId() - 1));
        updateActiveTabVisibility(); // Set Interval's own note-value picker only shows for that one timing mode (Sequence tab)
    };

    // Set Interval's grid point -- same note-value palette as Clock
    // reference/Step resolution above, shown only while Set Interval is
    // the selected timing (see updateTriggerModeVisibility()).
    controlsContent.addAndMakeVisible (patternSwitchIntervalLabel);
    patternSwitchIntervalLabel.setText ("Switch interval", juce::dontSendNotification);
    patternSwitchIntervalLabel.setJustificationType (juce::Justification::centredLeft);

    controlsContent.addAndMakeVisible (patternSwitchIntervalSelector);
    for (int i = 0; i < SlicerAudioProcessor::numNoteValueOptions; ++i)
        patternSwitchIntervalSelector.addItem (SlicerAudioProcessor::getNoteValueName (i), i + 1); // JUCE item IDs are 1-based
    patternSwitchIntervalSelector.setSelectedId (processor.getPatternSwitchIntervalIndex() + 1, juce::dontSendNotification);
    patternSwitchIntervalSelector.onChange = [this]
    {
        processor.setPatternSwitchIntervalIndex (patternSwitchIntervalSelector.getSelectedId() - 1);
    };

    // Performance mode (Pass 1) -- style/params panel points at Performance
    // mode's own working-state storage (see the constructor init list
    // above), never the global default values Slice Length/Clock use.
    controlsContent.addAndMakeVisible (performanceStyleParametersLabel);
    performanceStyleParametersLabel.setText ("Style parameters", juce::dontSendNotification);
    performanceStyleParametersLabel.setJustificationType (juce::Justification::centredLeft);

    controlsContent.addAndMakeVisible (performanceStyleParameterPanel);

    controlsContent.addAndMakeVisible (performanceLoopToggle);
    performanceLoopToggle.setToggleState (processor.getPerformanceWorkingLoop(), juce::dontSendNotification);
    performanceLoopToggle.onClick = [this]
    {
        processor.setPerformanceWorkingLoop (performanceLoopToggle.getToggleState());
    };

    controlsContent.addAndMakeVisible (performanceSyncToggle);
    performanceSyncToggle.setToggleState (processor.getPerformanceWorkingSync(), juce::dontSendNotification);
    performanceSyncToggle.onClick = [this]
    {
        processor.setPerformanceWorkingSync (performanceSyncToggle.getToggleState());
    };

    // Trim Snap mode -- Transients (existing behaviour) / Grid (fixed
    // musical grid at the established tempo). Item IDs mirror
    // SlicerAudioProcessor::TrimSnapMode's declaration order (1 = transients,
    // 2 = grid), same "JUCE item IDs are 1-based" convention as every other
    // enum-backed selector here.
    controlsContent.addAndMakeVisible (performanceTrimSnapLabel);
    performanceTrimSnapLabel.setText ("Trim snap", juce::dontSendNotification);
    performanceTrimSnapLabel.setJustificationType (juce::Justification::centredLeft);

    controlsContent.addAndMakeVisible (performanceTrimSnapSelector);
    performanceTrimSnapSelector.addItem ("Transients", 1);
    performanceTrimSnapSelector.addItem ("Grid", 2);
    performanceTrimSnapSelector.setSelectedId (
        static_cast<int> (processor.getPerformanceTrimSnapMode()) + 1, juce::dontSendNotification);
    performanceTrimSnapSelector.onChange = [this]
    {
        processor.setPerformanceTrimSnapMode (
            static_cast<SlicerAudioProcessor::TrimSnapMode> (performanceTrimSnapSelector.getSelectedId() - 1));
        updatePerformanceTrimSnapVisibility();
    };

    // Grid resolution -- same note-value palette as Clock reference/
    // Quantize Transients' Grid/Subdivide, shown only while Grid is the
    // selected snap mode (see updatePerformanceTrimSnapVisibility()).
    controlsContent.addAndMakeVisible (performanceTrimGridLabel);
    performanceTrimGridLabel.setText ("Grid", juce::dontSendNotification);
    performanceTrimGridLabel.setJustificationType (juce::Justification::centredLeft);

    controlsContent.addAndMakeVisible (performanceTrimGridSelector);
    for (int i = 0; i < SlicerAudioProcessor::numNoteValueOptions; ++i)
        performanceTrimGridSelector.addItem (SlicerAudioProcessor::getNoteValueName (i), i + 1); // JUCE item IDs are 1-based
    performanceTrimGridSelector.setSelectedId (processor.getPerformanceTrimGridIndex() + 1, juce::dontSendNotification);
    performanceTrimGridSelector.onChange = [this]
    {
        processor.setPerformanceTrimGridIndex (performanceTrimGridSelector.getSelectedId() - 1);
    };

    // Quantize Recall -- off (immediate, unchanged) by default, same
    // "preserve existing behaviour until explicitly opted into" convention
    // as Trim Snap's own toggle above.
    controlsContent.addAndMakeVisible (performanceQuantizeRecallToggle);
    performanceQuantizeRecallToggle.setToggleState (processor.getPerformanceQuantizeRecallEnabled(), juce::dontSendNotification);
    performanceQuantizeRecallToggle.onClick = [this]
    {
        processor.setPerformanceQuantizeRecallEnabled (performanceQuantizeRecallToggle.getToggleState());
        updatePerformanceQuantizeRecallVisibility();
    };

    // Quantize Recall's grid point -- same note-value palette as Clock
    // reference/Set Interval/Trim Snap's own Grid picker above, shown only
    // while the toggle is on (see updatePerformanceQuantizeRecallVisibility()).
    controlsContent.addAndMakeVisible (performanceQuantizeRecallIntervalLabel);
    performanceQuantizeRecallIntervalLabel.setText ("Recall interval", juce::dontSendNotification);
    performanceQuantizeRecallIntervalLabel.setJustificationType (juce::Justification::centredLeft);

    controlsContent.addAndMakeVisible (performanceQuantizeRecallIntervalSelector);
    for (int i = 0; i < SlicerAudioProcessor::numNoteValueOptions; ++i)
        performanceQuantizeRecallIntervalSelector.addItem (SlicerAudioProcessor::getNoteValueName (i), i + 1); // JUCE item IDs are 1-based
    performanceQuantizeRecallIntervalSelector.setSelectedId (processor.getPerformanceQuantizeRecallIntervalIndex() + 1, juce::dontSendNotification);
    performanceQuantizeRecallIntervalSelector.onChange = [this]
    {
        processor.setPerformanceQuantizeRecallIntervalIndex (performanceQuantizeRecallIntervalSelector.getSelectedId() - 1);
    };

    controlsContent.addAndMakeVisible (performanceKeyboardPanel);

    controlsContent.addAndMakeVisible (sequencerViewport);
    sequencerViewport.setViewedComponent (&sequencerGrid, false); // we own it, don't let the viewport delete it
    sequencerViewport.setScrollBarsShown (true, true);

    // Pass 1 tab structure -- every component belonging to each of
    // Generate/Sequence/Perform, for updateActiveTabVisibility()'s blanket
    // show/hide (see its own doc comment). Populated here, now that every
    // control referenced below is fully constructed.
    generateComponents = {
        &sampleSectionPanel, &trimTempoSectionPanel, &detectionSectionPanel, &engineSectionPanel, &playbackStyleSectionPanel,
        &loadButton, &resetEditsButton, &statusLabel, &undoButton, &redoButton,
        &auditionButton, &loopLengthLabel, &loopLengthKnob, &calculatedBpmLabel,
        &manualBpmOverrideToggle, &manualBpmOverrideLabel, &manualBpmOverrideSlider,
        &fadeInLabel, &fadeInSlider, &fadeOutLabel, &fadeOutSlider,
        &sensitivityLabel, &sensitivityKnob, &quantizeTransientsToggle, &quantizeGridLabel, &quantizeGridSelector,
        &pitchModeLabel, &pitchModeSegments, &beatQuantizeToggleRepitch,
        &grainSizeLabel, &grainSizeSlider, &windowShapeLabel, &windowShapeSelector, &beatQuantizeToggle,
        &pitchShiftLabel, &pitchShiftSlider,
        &triggerModeLabel, &triggerModeSegments,
        &clockReferenceLabel, &clockReferenceSelector, &tapeStopScopeLabel, &tapeStopScopeSelector,
        &filterSweepScopeLabel, &filterSweepScopeSelector, &resetEveryLabel, &resetEverySelector,
        &subdivisionTableLabel, &subdivisionGrid,
        &playbackStyleSegments, &playbackStyleLabel, &playbackStyleGrid,
        &playbackStyleParametersLabel, &playbackStyleParameterPanel
    };

    sequenceComponents = {
        &stepResolutionLabel, &stepResolutionSelector, &patternLengthLabel, &patternLengthSelector,
        &randomizeSequenceButton, &clearSequenceButton, &playbackStylePalette, &sequencerViewport, &patternBankPanel,
        &patternSwitchTimingLabel, &patternSwitchTimingSelector, &patternSwitchIntervalLabel, &patternSwitchIntervalSelector
    };

    performComponents = {
        &performanceStyleParametersLabel, &performanceStyleParameterPanel, &performanceLoopToggle, &performanceSyncToggle,
        &performanceTrimSnapLabel, &performanceTrimSnapSelector, &performanceTrimGridLabel, &performanceTrimGridSelector,
        &performanceQuantizeRecallToggle, &performanceQuantizeRecallIntervalLabel, &performanceQuantizeRecallIntervalSelector,
        &performanceKeyboardPanel
    };

    syncTriggerModeToActiveTab(); // no-op: both tab rows were already seeded from processor.getTriggerMode() above
    updateActiveTabVisibility(); // also drives updateTriggerModeVisibility()/updatePitchModeVisibility()/updateManualBpmOverrideVisibility()/updateQuantizeTransientsVisibility() for whichever tab is active

    // Zoom/pan (Step 31) — live directly on the editor, not controlsContent,
    // so they stay visible next to the waveform regardless of scroll
    // position, same reasoning as waveformDisplay itself living there.
    addAndMakeVisible (zoomToTrimsButton);
    zoomToTrimsButton.addListener (this);

    addAndMakeVisible (resetZoomButton);
    resetZoomButton.addListener (this);

    addAndMakeVisible (waveformDisplay);
    waveformDisplay.onSampleChanged = [this] { updateAfterSampleOrSliceChange(); };
    waveformDisplay.onTrimChanged = [this]
    {
        updateAfterSampleOrSliceChange();

        // Loop Length (bars) doesn't auto-adjust when the trim range
        // changes -- there's no way to guess the right new value -- so
        // flag it as needing a fresh look instead (Step 33), cleared the
        // moment the user touches loopLengthKnob (see its onValueChange/
        // onDragEnd above).
        loopLengthNeedsAttention = true;
        repaint();
    };

    // Fixed window size regardless of how much lives inside controlsContent
    // (it scrolls internally within controlsViewport's fixed height) —
    // this no longer needs to grow every time a control gets added, and
    // comfortably fits any modern laptop screen, 16" MacBook included.
    // Widened from 600 (Step 31) to give the waveform display significantly
    // more horizontal room for zoom/pan and the beat-number grid.
    setSize (900, 780);

    if (processor.hasSample())
        updateAfterSampleOrSliceChange();
}

SlicerAudioProcessorEditor::~SlicerAudioProcessorEditor()
{
    loadButton.removeListener (this);
    resetEditsButton.removeListener (this);
    undoButton.removeListener (this);
    redoButton.removeListener (this);
    auditionButton.removeListener (this);
    zoomToTrimsButton.removeListener (this);
    resetZoomButton.removeListener (this);
    randomizeSequenceButton.removeListener (this);
    clearSequenceButton.removeListener (this);
}

void SlicerAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));

    g.setColour (juce::Colours::white.withAlpha (0.6f));
    g.setFont (14.0f);
    g.drawFittedText ("NeditVST - step 46: Filter Type, Curve Shape, Stretch Grain step editing",
                       getLocalBounds().removeFromTop (30), juce::Justification::centred, 1);

    // Loop Length staleness highlight (Step 33). loopLengthLabel/Knob live
    // inside controlsContent, scrolled by controlsViewport -- rather than
    // computing their position by hand, getLocalArea() walks the whole
    // parent chain (including the viewport's current scroll offset) to get
    // their real on-screen rectangle in THIS component's coordinate space,
    // so the highlight tracks correctly regardless of scroll position and
    // regardless of how deeply nested inside a SectionPanel's visual area
    // the knob ends up (getLocalArea() doesn't care, only real bounds
    // matter -- see SectionPanel's own class doc comment). Clipped to the
    // viewport's own bounds so nothing is drawn if scrolled out of view.
    // Loop Length only exists on the Generate page (Pass 1) -- guarded so
    // this doesn't draw a highlight around a hidden, off-tab control.
    const bool generateTabActive = topLevelModeTabs.getSelectedIndex() == 0 && subModeTabs.getSelectedIndex() == 0;

    if (loopLengthNeedsAttention && generateTabActive && loopLengthLabel.isVisible())
    {
        juce::Graphics::ScopedSaveState saveState (g);
        g.reduceClipRegion (controlsViewport.getBounds());

        const auto labelBounds = getLocalArea (&loopLengthLabel, loopLengthLabel.getLocalBounds());
        const auto knobBounds = getLocalArea (&loopLengthKnob, loopLengthKnob.getLocalBounds());
        const auto highlightBounds = labelBounds.getUnion (knobBounds).expanded (4);

        g.setColour (juce::Colours::orange);
        g.drawRect (highlightBounds, 2);
    }
}

void SlicerAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced (20);
    area.removeFromTop (30); // space for the paint() header text

    controlsViewport.setBounds (area.removeFromTop (controlsViewportHeight));
    area.removeFromTop (20);

    // Vertical scrolling only (setScrollBarsShown (true, false) in the
    // constructor) — content is exactly as wide as the visible area minus
    // whatever room the vertical scrollbar itself needs, so nothing ever
    // needs to scroll sideways too.
    const int contentWidth = controlsViewport.getWidth() - controlsViewport.getScrollBarThickness();
    const int contentHeight = layoutControlsContent (contentWidth);
    controlsContent.setSize (contentWidth, contentHeight);

    // Zoom/pan (Step 31) — a small fixed row above the waveform, always
    // visible alongside it regardless of controlsContent's scroll position.
    auto zoomButtonsRow = area.removeFromTop (30);
    zoomToTrimsButton.setBounds (zoomButtonsRow.removeFromLeft (150));
    zoomButtonsRow.removeFromLeft (10);
    resetZoomButton.setBounds (zoomButtonsRow.removeFromLeft (150));
    area.removeFromTop (10);

    // SequencerGrid's own width (Step 38) -- driven by contentWidth (the
    // SAME width basis layoutControlsContent()/the palette row above
    // already used, computed just above at line ~497), NOT the outer
    // editor's own `area.getWidth()` -- those two differ by exactly
    // controlsViewport's own vertical scrollbar thickness (visible
    // whenever the controls list -- which now includes a 9-row style
    // palette -- is taller than controlsViewportHeight, i.e. essentially
    // always). Using the wider, un-reduced `area.getWidth()` here sized
    // the grid a scrollbar's-width too wide for the space sequencerViewport
    // (nested inside controlsContent, which IS sized to contentWidth)
    // actually gives it, so the grid's own rightmost sliver spilled past
    // its container and picked up an unwanted horizontal scrollbar --
    // this is the palette/grid misalignment. Reduced by the Style
    // Palette's own width + the gap beside it (Step 41), so the COMBINED
    // [palette][grid] row still matches contentWidth overall, same as the
    // grid alone did before the palette existed.
    sequencerGrid.setTargetWidth (contentWidth - PlaybackStylePalette::preferredWidth - 10);

    waveformDisplay.setBounds (area); // takes up all remaining space, always fully visible
}

juce::Rectangle<int> SlicerAudioProcessorEditor::sectionContentArea (const SectionPanel& panel)
{
    return panel.getBounds().withTrimmedTop (SectionPanel::titleBarHeight).reduced (8, 6);
}

int SlicerAudioProcessorEditor::layoutControlsContent (int contentWidth)
{
    // Tab rows sit above whichever page is active, always laid out
    // regardless of selection. Below them, exactly one of the five
    // per-page layout functions runs -- the tab-driven analogue of the old
    // single flat control list, now split so each page only pays for (and
    // only needs scroll height for) its own content.
    topLevelModeTabs.setBounds (0, 0, contentWidth, SegmentedButtonRow::preferredHeight);
    int y = SegmentedButtonRow::preferredHeight + 10;

    const bool beats = topLevelModeTabs.getSelectedIndex() == 0;

    if (beats)
    {
        subModeTabs.setBounds (0, y, contentWidth, SegmentedButtonRow::preferredHeight);
        y += SegmentedButtonRow::preferredHeight + 14;
    }


    int pageHeight = 0;

    if (! beats)
    {
        pageHeight = layoutTexturesPlaceholder (contentWidth, y);
    }
    else
    {
        switch (subModeTabs.getSelectedIndex())
        {
            case 1: pageHeight = layoutSequenceTab (contentWidth, y); break;
            case 2: pageHeight = layoutControlPlaceholder (contentWidth, y); break;
            case 3: pageHeight = layoutPerformTab (contentWidth, y); break;
            default: pageHeight = layoutGenerateTab (contentWidth, y); break;
        }
    }

    return y + pageHeight;
}

int SlicerAudioProcessorEditor::layoutGenerateTab (int contentWidth, int startY)
{
    constexpr int sacrificialHeight = 4000; // comfortably larger than any realistic total content height
    constexpr int panelGap = 14;
    constexpr int sectionSacrificialHeight = 1200;

    juce::Rectangle<int> area (0, startY, contentWidth, sacrificialHeight);

    // Sizes a SectionPanel to fit exactly whatever layoutFn lays out inside
    // its content area (see SectionPanel's own class doc comment for why
    // the real controls stay direct children of controlsContent rather than
    // being reparented into the panel) -- a two-pass measure-then-shrink,
    // so each section's own row layout below can stay a plain top-to-bottom
    // removeFromTop() sequence, same style every other layout function in
    // this file already uses, without hand-computing its total height.
    auto layoutSection = [&] (SectionPanel& panel, auto&& layoutFn)
    {
        panel.setBounds (area.getX(), area.getY(), area.getWidth(), sectionSacrificialHeight);
        auto content = sectionContentArea (panel);
        const int startHeight = content.getHeight();

        layoutFn (content);

        const int consumed = startHeight - content.getHeight();
        const int panelHeight = SectionPanel::titleBarHeight + consumed + 12; // 12 = getContentArea()'s reduced(8,6) top+bottom padding
        panel.setBounds (area.getX(), area.getY(), area.getWidth(), panelHeight);
        area.removeFromTop (panelHeight);
        area.removeFromTop (panelGap);
    };

    layoutSection (sampleSectionPanel, [this] (juce::Rectangle<int>& content)
    {
        auto topButtonRow = content.removeFromTop (40);
        loadButton.setBounds (topButtonRow.removeFromLeft (topButtonRow.getWidth() - 110));
        topButtonRow.removeFromLeft (10);
        resetEditsButton.setBounds (topButtonRow);
        content.removeFromTop (10);

        statusLabel.setBounds (content.removeFromTop (30));
        content.removeFromTop (10);

        auto undoRedoRow = content.removeFromTop (30);
        undoButton.setBounds (undoRedoRow.removeFromLeft (100));
        undoRedoRow.removeFromLeft (10);
        redoButton.setBounds (undoRedoRow.removeFromLeft (100));
    });

    layoutSection (trimTempoSectionPanel, [this] (juce::Rectangle<int>& content)
    {
        auditionButton.setBounds (content.removeFromTop (30));
        content.removeFromTop (10);

        auto loopRow = content.removeFromTop (juce::jmax (30, RotaryKnob::preferredSize + 14));
        loopLengthLabel.setBounds (loopRow.removeFromLeft (140).withSizeKeepingCentre (140, 20));
        loopLengthKnob.setBounds (loopRow.removeFromLeft (90));
        loopRow.removeFromLeft (10);
        calculatedBpmLabel.setBounds (loopRow);
        content.removeFromTop (14);

        manualBpmOverrideToggle.setBounds (content.removeFromTop (24));
        content.removeFromTop (6);

        auto manualBpmValueRow = content.removeFromTop (30);
        manualBpmOverrideLabel.setBounds (manualBpmValueRow.removeFromLeft (140));
        manualBpmOverrideSlider.setBounds (manualBpmValueRow);
        content.removeFromTop (14);

        auto fadeInRow = content.removeFromTop (30);
        fadeInLabel.setBounds (fadeInRow.removeFromLeft (140));
        fadeInSlider.setBounds (fadeInRow);
        content.removeFromTop (10);

        auto fadeOutRow = content.removeFromTop (30);
        fadeOutLabel.setBounds (fadeOutRow.removeFromLeft (140));
        fadeOutSlider.setBounds (fadeOutRow);
    });

    layoutSection (detectionSectionPanel, [this] (juce::Rectangle<int>& content)
    {
        auto sensitivityRow = content.removeFromTop (juce::jmax (30, RotaryKnob::preferredSize + 14));
        sensitivityLabel.setBounds (sensitivityRow.removeFromLeft (140).withSizeKeepingCentre (140, 20));
        sensitivityKnob.setBounds (sensitivityRow.removeFromLeft (90));
        content.removeFromTop (14);

        quantizeTransientsToggle.setBounds (content.removeFromTop (24));
        content.removeFromTop (6);

        auto quantizeGridRow = content.removeFromTop (30);
        quantizeGridLabel.setBounds (quantizeGridRow.removeFromLeft (140));
        quantizeGridSelector.setBounds (quantizeGridRow.removeFromLeft (150));
    });

    layoutSection (engineSectionPanel, [this] (juce::Rectangle<int>& content)
    {
        pitchModeLabel.setBounds (content.removeFromTop (20));
        pitchModeSegments.setBounds (content.removeFromTop (SegmentedButtonRow::preferredHeight));
        content.removeFromTop (10);

        beatQuantizeToggleRepitch.setBounds (content.removeFromTop (24));
        content.removeFromTop (10);

        auto grainSizeRow = content.removeFromTop (30);
        grainSizeLabel.setBounds (grainSizeRow.removeFromLeft (140));
        grainSizeSlider.setBounds (grainSizeRow);
        content.removeFromTop (10);

        auto windowShapeRow = content.removeFromTop (30);
        windowShapeLabel.setBounds (windowShapeRow.removeFromLeft (140));
        windowShapeSelector.setBounds (windowShapeRow.removeFromLeft (150));
        content.removeFromTop (10);

        beatQuantizeToggle.setBounds (content.removeFromTop (24));
        content.removeFromTop (10);

        auto pitchShiftRow = content.removeFromTop (30);
        pitchShiftLabel.setBounds (pitchShiftRow.removeFromLeft (140));
        pitchShiftSlider.setBounds (pitchShiftRow);
        content.removeFromTop (16);

        triggerModeLabel.setBounds (content.removeFromTop (20));
        triggerModeSegments.setBounds (content.removeFromTop (SegmentedButtonRow::preferredHeight));
        content.removeFromTop (10);

        auto clockReferenceRow = content.removeFromTop (30);
        clockReferenceLabel.setBounds (clockReferenceRow.removeFromLeft (140));
        clockReferenceSelector.setBounds (clockReferenceRow.removeFromLeft (150));
        content.removeFromTop (10);

        auto tapeStopScopeRow = content.removeFromTop (30);
        tapeStopScopeLabel.setBounds (tapeStopScopeRow.removeFromLeft (140));
        tapeStopScopeSelector.setBounds (tapeStopScopeRow.removeFromLeft (150));
        content.removeFromTop (10);

        auto filterSweepScopeRow = content.removeFromTop (30);
        filterSweepScopeLabel.setBounds (filterSweepScopeRow.removeFromLeft (140));
        filterSweepScopeSelector.setBounds (filterSweepScopeRow.removeFromLeft (150));
        content.removeFromTop (10);

        auto resetEveryRow = content.removeFromTop (30);
        resetEveryLabel.setBounds (resetEveryRow.removeFromLeft (140));
        resetEverySelector.setBounds (resetEveryRow.removeFromLeft (150));
        content.removeFromTop (10);

        subdivisionTableLabel.setBounds (content.removeFromTop (20));
        subdivisionGrid.setBounds (content.removeFromTop (SubdivisionProbabilityGrid::getPreferredHeight()));
    });

    layoutSection (playbackStyleSectionPanel, [this] (juce::Rectangle<int>& content)
    {
        playbackStyleSegments.setBounds (content.removeFromTop (SegmentedButtonRow::preferredHeight));
        content.removeFromTop (10);

        playbackStyleLabel.setBounds (content.removeFromTop (20));
        playbackStyleGrid.setBounds (content.removeFromTop (PlaybackStyleGrid::getPreferredHeight()));
        content.removeFromTop (14);

        playbackStyleParametersLabel.setBounds (content.removeFromTop (20));
        playbackStyleParameterPanel.setBounds (content.removeFromTop (PlaybackStyleParameterPanel::getPreferredHeight()));
    });

    return sacrificialHeight - area.getHeight(); // total consumed height, independent of startY (only position, not the height field, is offset by it)
}

int SlicerAudioProcessorEditor::layoutSequenceTab (int contentWidth, int startY)
{
    constexpr int sacrificialHeight = 4000;
    juce::Rectangle<int> area (0, startY, contentWidth, sacrificialHeight);

    auto stepResolutionRow = area.removeFromTop (30);
    stepResolutionLabel.setBounds (stepResolutionRow.removeFromLeft (140));
    stepResolutionSelector.setBounds (stepResolutionRow.removeFromLeft (150));
    area.removeFromTop (10);

    auto patternLengthRow = area.removeFromTop (30);
    patternLengthLabel.setBounds (patternLengthRow.removeFromLeft (140));
    patternLengthSelector.setBounds (patternLengthRow.removeFromLeft (150));
    area.removeFromTop (10);

    auto randomizeClearRow = area.removeFromTop (30);
    randomizeSequenceButton.setBounds (randomizeClearRow.removeFromLeft (randomizeClearRow.getWidth() - 110));
    randomizeClearRow.removeFromLeft (10);
    clearSequenceButton.setBounds (randomizeClearRow);
    area.removeFromTop (10);

    // This row's own height must accommodate whichever of the two is
    // taller: sequencerViewportHeight (the viewport's own fixed, scrolling
    // height -- unrelated to style count) or playbackStylePalette's
    // current preferred height (one row per PlaybackStyle, via
    // PlaybackStylePalette::getPreferredHeight() -- grows automatically as
    // styles are added, e.g. Scratch's addition already grew it past the
    // old fixed sequencerViewportHeight, which used to just clip/squash
    // the palette's own bottom row(s) short since setBounds() overrides
    // whatever height the palette's constructor gave itself). Not
    // hardcoded either way, so this needs no revisiting next time another
    // style is added.
    const int sequencerRowHeight = juce::jmax (sequencerViewportHeight, PlaybackStylePalette::getPreferredHeight());
    auto sequencerRow = area.removeFromTop (sequencerRowHeight);
    playbackStylePalette.setBounds (sequencerRow.removeFromLeft (PlaybackStylePalette::preferredWidth)
                                                 .removeFromTop (PlaybackStylePalette::getPreferredHeight()));
    sequencerRow.removeFromLeft (10);
    sequencerViewport.setBounds (sequencerRow);
    // Dynamic row-height scaling (see SequencerGrid's own doc comment) --
    // sequencerViewport's own height IS the available vertical space the
    // grid scales its rows against, so it's passed straight through.
    sequencerGrid.setAvailableHeight (sequencerViewport.getHeight());
    area.removeFromTop (20);

    // Pattern bank (MIDI input) -- its own row, rather than squeezed into
    // sequencerRow where it'd fight sequencerViewport for width.
    auto patternBankRow = area.removeFromTop (PatternBankPanel::getPreferredHeight());
    patternBankPanel.setBounds (patternBankRow.removeFromLeft (PatternBankPanel::preferredWidth));
    area.removeFromTop (20);

    auto patternSwitchTimingRow = area.removeFromTop (30);
    patternSwitchTimingLabel.setBounds (patternSwitchTimingRow.removeFromLeft (140));
    patternSwitchTimingSelector.setBounds (patternSwitchTimingRow.removeFromLeft (150));
    area.removeFromTop (10);

    auto patternSwitchIntervalRow = area.removeFromTop (30);
    patternSwitchIntervalLabel.setBounds (patternSwitchIntervalRow.removeFromLeft (140));
    patternSwitchIntervalSelector.setBounds (patternSwitchIntervalRow.removeFromLeft (150));
    area.removeFromTop (10);

    return sacrificialHeight - area.getHeight(); // total consumed height, independent of startY (only position, not the height field, is offset by it)
}

int SlicerAudioProcessorEditor::layoutPerformTab (int contentWidth, int startY)
{
    constexpr int sacrificialHeight = 4000;
    juce::Rectangle<int> area (0, startY, contentWidth, sacrificialHeight);

    performanceStyleParametersLabel.setBounds (area.removeFromTop (20));
    performanceStyleParameterPanel.setBounds (area.removeFromTop (PlaybackStyleParameterPanel::getPreferredHeight()));
    area.removeFromTop (20);

    auto performanceToggleRow = area.removeFromTop (24);
    performanceLoopToggle.setBounds (performanceToggleRow.removeFromLeft (100));
    performanceToggleRow.removeFromLeft (10);
    performanceSyncToggle.setBounds (performanceToggleRow.removeFromLeft (100));
    area.removeFromTop (10);

    // Trim Snap mode + its Grid-resolution picker.
    auto performanceTrimSnapRow = area.removeFromTop (30);
    performanceTrimSnapLabel.setBounds (performanceTrimSnapRow.removeFromLeft (140));
    performanceTrimSnapSelector.setBounds (performanceTrimSnapRow.removeFromLeft (150));
    area.removeFromTop (10);

    auto performanceTrimGridRow = area.removeFromTop (30);
    performanceTrimGridLabel.setBounds (performanceTrimGridRow.removeFromLeft (140));
    performanceTrimGridSelector.setBounds (performanceTrimGridRow.removeFromLeft (150));
    area.removeFromTop (10);

    // Quantize Recall toggle + its own note-value picker.
    auto performanceQuantizeRecallRow = area.removeFromTop (24);
    performanceQuantizeRecallToggle.setBounds (performanceQuantizeRecallRow.removeFromLeft (150));
    area.removeFromTop (10);

    auto performanceQuantizeRecallIntervalRow = area.removeFromTop (30);
    performanceQuantizeRecallIntervalLabel.setBounds (performanceQuantizeRecallIntervalRow.removeFromLeft (140));
    performanceQuantizeRecallIntervalSelector.setBounds (performanceQuantizeRecallIntervalRow.removeFromLeft (150));
    area.removeFromTop (10);

    // Full contentWidth, unlike the old compact bank grid -- a real
    // keyboard needs the room to be usable.
    auto performanceKeyboardRow = area.removeFromTop (PerformanceKeyboardPanel::getPreferredHeight());
    performanceKeyboardPanel.setBounds (performanceKeyboardRow);

    return sacrificialHeight - area.getHeight(); // total consumed height, independent of startY (only position, not the height field, is offset by it)
}

int SlicerAudioProcessorEditor::layoutControlPlaceholder (int contentWidth, int startY)
{
    constexpr int height = 200;
    controlPlaceholder.setBounds (0, startY, contentWidth, height);
    return height;
}

int SlicerAudioProcessorEditor::layoutTexturesPlaceholder (int contentWidth, int startY)
{
    constexpr int height = 200;
    texturesPlaceholder.setBounds (0, startY, contentWidth, height);
    return height;
}

void SlicerAudioProcessorEditor::buttonClicked (juce::Button* button)
{
    if (button == &loadButton)
        chooseAndLoadFile();
    else if (button == &resetEditsButton)
    {
        processor.resetAllManualEdits();
        updateAfterSampleOrSliceChange();
    }
    else if (button == &undoButton)
    {
        processor.undoLastEdit();
        updateAfterSampleOrSliceChange();
    }
    else if (button == &redoButton)
    {
        processor.redoLastEdit();
        updateAfterSampleOrSliceChange();
    }
    else if (button == &auditionButton)
    {
        processor.setAuditionActive (! processor.getAuditionActive());
    }
    else if (button == &zoomToTrimsButton)
    {
        waveformDisplay.zoomToTrims();
    }
    else if (button == &resetZoomButton)
    {
        waveformDisplay.resetZoom();
    }
    else if (button == &randomizeSequenceButton)
    {
        processor.randomizeSequence();
        sequencerGrid.repaint(); // immediate feedback rather than waiting for the next 30fps timer tick
    }
    else if (button == &clearSequenceButton)
    {
        processor.clearSequence();
        sequencerGrid.repaint();
    }
}

void SlicerAudioProcessorEditor::timerCallback()
{
    undoButton.setEnabled (processor.canUndoEdit());
    redoButton.setEnabled (processor.canRedoEdit());

    // Polled rather than driven only by the button's own click, since the
    // processor can also stop an audition on its own (host transport
    // started) — the label/colour has to reflect that auto-stop too.
    const bool auditioning = processor.getAuditionActive();
    auditionButton.setButtonText (auditioning ? "Stop Audition" : "Audition");
    auditionButton.setColour (juce::TextButton::buttonColourId,
                               auditioning ? juce::Colours::orange.withAlpha (0.6f)
                                           : getLookAndFeel().findColour (juce::TextButton::buttonColourId));

    // These two are otherwise write-only (UI -> processor via onChange
    // below) -- a MIDI pattern-bank recall can change the processor's
    // stepResolutionIndex/patternLengthBarsIndex on its own, out from under
    // them, so poll and resync rather than let them show a stale value
    // while SequencerGrid (which already polls dimensions live) shows the
    // newly recalled grid underneath.
    const int processorStepResolutionId = processor.getStepResolutionIndex() + 1;
    if (stepResolutionSelector.getSelectedId() != processorStepResolutionId)
        stepResolutionSelector.setSelectedId (processorStepResolutionId, juce::dontSendNotification);

    const int processorPatternLengthId = processor.getPatternLengthBarsIndex() + 1;
    if (patternLengthSelector.getSelectedId() != processorPatternLengthId)
        patternLengthSelector.setSelectedId (processorPatternLengthId, juce::dontSendNotification);

    // Performance mode's working state -- same "poll and resync" reasoning
    // as the pair just above: a click on the on-screen keyboard
    // (setFocusedPerformanceStateSlot()) loads a new slot straight into
    // performanceWorkingState from the keyboard panel's own click handler,
    // out from under this parameter panel/these toggles, so a focus change
    // made from the keyboard has to be reflected here too, not just heard.
    // Also unconditionally repaints the parameter panel -- its rows read
    // performanceWorkingState live via
    // the getValue lambda passed to its constructor, but nothing else
    // triggers a repaint when that value changes from outside a drag on
    // this panel itself.
    performanceStyleParameterPanel.setSelectedStyle (processor.getPerformanceWorkingStyle());
    performanceStyleParameterPanel.repaint();

    const bool processorPerformanceLoop = processor.getPerformanceWorkingLoop();
    if (performanceLoopToggle.getToggleState() != processorPerformanceLoop)
        performanceLoopToggle.setToggleState (processorPerformanceLoop, juce::dontSendNotification);

    const bool processorPerformanceSync = processor.getPerformanceWorkingSync();
    if (performanceSyncToggle.getToggleState() != processorPerformanceSync)
        performanceSyncToggle.setToggleState (processorPerformanceSync, juce::dontSendNotification);
}

void SlicerAudioProcessorEditor::chooseAndLoadFile()
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
            processor.loadSample (file);
            updateAfterSampleOrSliceChange();
        }
    });
}

void SlicerAudioProcessorEditor::updateTriggerModeVisibility()
{
    // Generate-page-only now (Pass 1) -- Sequenced/Performance's own
    // controls moved to the Sequence/Perform tabs, gated by
    // updateActiveTabVisibility() instead. Only called while Generate is
    // actually the active tab (from updateActiveTabVisibility() itself, or
    // from triggerModeSegments' own onSelectionChanged, which can only fire
    // while that control is visible) -- no need to re-check that here.
    const int selectedIndex = triggerModeSegments.getSelectedIndex();
    const bool sliceLength = selectedIndex == 0;
    const bool clock = selectedIndex == 1;

    clockReferenceLabel.setVisible (clock);
    clockReferenceSelector.setVisible (clock);
    tapeStopScopeLabel.setVisible (clock);
    tapeStopScopeSelector.setVisible (clock);
    filterSweepScopeLabel.setVisible (clock);
    filterSweepScopeSelector.setVisible (clock);
    subdivisionTableLabel.setVisible (clock);
    subdivisionGrid.setVisible (clock);

    // Reset every (Step 34) — Slice Length mode only, since Clock mode
    // already has its own window-boundary mechanism.
    resetEveryLabel.setVisible (sliceLength);
    resetEverySelector.setVisible (sliceLength);
}

void SlicerAudioProcessorEditor::updateManualBpmOverrideVisibility()
{
    const bool enabled = manualBpmOverrideToggle.getToggleState();

    manualBpmOverrideLabel.setVisible (enabled);
    manualBpmOverrideSlider.setVisible (enabled);
}

void SlicerAudioProcessorEditor::updateQuantizeTransientsVisibility()
{
    const bool enabled = quantizeTransientsToggle.getToggleState();

    quantizeGridLabel.setVisible (enabled);
    quantizeGridSelector.setVisible (enabled);
}

void SlicerAudioProcessorEditor::updatePerformanceTrimSnapVisibility()
{
    // Grid resolution only matters -- and is only shown -- while Performance
    // mode is the active trigger mode AND Grid is the selected Trim Snap
    // mode; Transients needs no resolution picker at all. Reads
    // processor.getTriggerMode() directly now (Pass 1) rather than a UI
    // selector's index -- more robust than re-deriving "is Performance
    // active" from tab state, and this control only exists (and this
    // function is only called) while the Perform tab is actually showing it.
    const bool performance = processor.getTriggerMode() == SlicerAudioProcessor::TriggerMode::performance;
    const bool grid = performanceTrimSnapSelector.getSelectedId() == 2;

    performanceTrimGridLabel.setVisible (performance && grid);
    performanceTrimGridSelector.setVisible (performance && grid);
}

void SlicerAudioProcessorEditor::updatePerformanceQuantizeRecallVisibility()
{
    // Recall interval only matters -- and is only shown -- while Performance
    // mode is the active trigger mode AND Quantize Recall is switched on;
    // off needs no interval picker at all (immediate, unchanged behaviour).
    const bool performance = processor.getTriggerMode() == SlicerAudioProcessor::TriggerMode::performance;
    const bool quantized = performanceQuantizeRecallToggle.getToggleState();

    performanceQuantizeRecallIntervalLabel.setVisible (performance && quantized);
    performanceQuantizeRecallIntervalSelector.setVisible (performance && quantized);
}

void SlicerAudioProcessorEditor::updatePitchModeVisibility()
{
    const int selectedIndex = pitchModeSegments.getSelectedIndex();
    const bool repitch = selectedIndex == 0;
    const bool timeStretch = selectedIndex == 1;

    beatQuantizeToggleRepitch.setVisible (repitch);

    grainSizeLabel.setVisible (timeStretch);
    grainSizeSlider.setVisible (timeStretch);
    windowShapeLabel.setVisible (timeStretch);
    windowShapeSelector.setVisible (timeStretch);
    beatQuantizeToggle.setVisible (timeStretch);
    pitchShiftLabel.setVisible (timeStretch);
    pitchShiftSlider.setVisible (timeStretch);
}

void SlicerAudioProcessorEditor::updateActiveTabVisibility()
{
    const bool beats = topLevelModeTabs.getSelectedIndex() == 0;
    const int subIndex = subModeTabs.getSelectedIndex();
    const bool generateActive = beats && subIndex == 0;
    const bool sequenceActive = beats && subIndex == 1;
    const bool controlActive = beats && subIndex == 2;
    const bool performActive = beats && subIndex == 3;

    subModeTabs.setVisible (beats);
    texturesPlaceholder.setVisible (! beats);
    controlPlaceholder.setVisible (controlActive);

    for (auto* c : generateComponents)
        c->setVisible (generateActive);

    if (generateActive)
    {
        // Fine-grained sub-visibility WITHIN Generate (Clock-only vs
        // Slice-Length-only groups, Repitch vs Time-Stretch, Grid dropdown,
        // BPM field) -- same functions this editor always had, just now
        // run after the blanket show above rather than unconditionally.
        updateTriggerModeVisibility();
        updatePitchModeVisibility();
        updateManualBpmOverrideVisibility();
        updateQuantizeTransientsVisibility();
    }

    for (auto* c : sequenceComponents)
        c->setVisible (sequenceActive);

    if (sequenceActive)
    {
        const bool setInterval = patternSwitchTimingSelector.getSelectedId()
            == static_cast<int> (SlicerAudioProcessor::PatternSwitchTiming::setInterval) + 1;
        patternSwitchIntervalLabel.setVisible (setInterval);
        patternSwitchIntervalSelector.setVisible (setInterval);
    }

    for (auto* c : performComponents)
        c->setVisible (performActive);

    if (performActive)
    {
        updatePerformanceTrimSnapVisibility();
        updatePerformanceQuantizeRecallVisibility();
    }
}

void SlicerAudioProcessorEditor::syncTriggerModeToActiveTab()
{
    using TM = SlicerAudioProcessor::TriggerMode;

    if (topLevelModeTabs.getSelectedIndex() != 0) // Textures: no trigger-mode implication
        return;

    const TM desired = subModeTabs.getSelectedIndex() == 1 ? TM::sequenced   // Sequence tab
                      : subModeTabs.getSelectedIndex() == 3 ? TM::performance // Perform tab
                                                             : lastGenerateTriggerMode; // Generate/Control

    if (processor.getTriggerMode() == desired)
        return; // guard: setTriggerMode() unconditionally resets clock/reset/sequenced/performance init flags even for a no-op mode

    const bool wasPerformance = processor.getTriggerMode() == TM::performance;
    processor.setTriggerMode (desired);

    // Leaving Performance mode: Performance's own focus-change path
    // deliberately raw-stores trim (see setFocusedPerformanceStateSlot()'s
    // own doc comment) rather than rebuilding slices, so `slices` can be
    // stale against wherever Performance mode left the trim. Force one
    // rebuild here -- a one-off UI-thread action, not a hot path -- by
    // re-invoking the existing public trim setter with the same value it
    // already has, purely for its rebuild side effect (same fix the old
    // old triggerModeSelector.onChange applied).
    if (wasPerformance)
        processor.setTrimEndSample (processor.getTrimEndSample(), false);

    // Only refresh Generate's own Clock-only/Slice-Length-only sub-groups if
    // Generate is actually the active tab right now -- updateTriggerModeVisibility()
    // has no active-tab guard of its own (it trusts callers, same convention
    // this editor always used), so calling it while e.g. the Sequence tab is
    // active would incorrectly re-show Generate-only controls that
    // updateActiveTabVisibility()'s blanket hide (called just before this,
    // from both tab rows' onSelectionChanged) already turned off.
    if (topLevelModeTabs.getSelectedIndex() == 0 && subModeTabs.getSelectedIndex() == 0)
        updateTriggerModeVisibility();
}

void SlicerAudioProcessorEditor::updateAfterSampleOrSliceChange()
{
    const int numSlices = processor.getNumSlices();
    const juce::String text = processor.getLoadedFileName()
                             + "  —  " + juce::String (numSlices)
                             + " slice" + (numSlices == 1 ? "" : "s");
    statusLabel.setText (text, juce::dontSendNotification);

    const double bpm = processor.getCalculatedOriginalBpm();
    calculatedBpmLabel.setText (bpm > 0.0 ? ("~" + juce::String (bpm, 1) + " BPM") : "",
                                 juce::dontSendNotification);

    waveformDisplay.refresh();
}
