#include <doctest/doctest.h>
#include "model/SlicerModel.h"

// State persistence (nextsteps 1.4) round-trips. Every test that exercises
// restoreState() binds onPickStateInvalidated first -- restore ends with the
// unguarded callback, exactly like the slice-rebuild paths.

TEST_CASE ("SlicerModel saveState/restoreState round-trips globals and style globals")
{
    SlicerModel model;
    model.onPickStateInvalidated = [] {};

    model.loopLengthBars.store (3);
    model.manualBpmOverrideEnabled.store (true);
    model.manualBpmOverrideValue.store (128.5);
    model.currentSensitivity.store (0.42f);
    model.fadeInMs.store (7.0f);
    model.fadeOutMs.store (23.0f);
    model.quantizeTransientsEnabled.store (true);
    model.quantizeGridIndex.store (5);
    model.triggerMode.store (SlicerModel::TriggerMode::clock);
    model.clockReferenceIndex.store (9);
    model.tapeStopScope.store (SlicerModel::TapeStopScope::perTick);
    model.filterSweepScope.store (SlicerModel::FilterSweepScope::wholeWindow);
    model.resetBarsIndex.store (3);
    model.pitchMode.store (SlicerModel::PitchMode::timeStretch);
    model.grainSizeMs.store (45.0f);
    model.grainWindowShape.store (SlicerModel::GrainWindowShape::triangular);
    model.pitchShiftSemitones.store (3.5f);
    model.beatQuantizeSliceLengthEnabled.store (false);
    model.beatQuantizeSliceLengthEnabledRepitch.store (true);

    model.stretchGrainSizeMsValue.store (22.0f);
    model.stretchSpeedMultiplierValue.store (2.5f);
    model.filterSweepResonanceValue.store (3.2f);
    model.filterSweepFilterTypeValue.store (2);
    model.curveShapeValue.store (1);
    model.bitcrushRateReductionGlobalValue.store (8.0f);
    model.bitcrushRateReductionModeGlobalValue.store (2);
    model.bitcrushBitDepthGlobalValue.store (11.0f);
    model.bitcrushBitDepthModeGlobalValue.store (1);
    model.scratchRateGlobalValue.store (13);
    model.scratchForwardCurveGlobalValue.store (3);
    model.scratchBackwardCurveGlobalValue.store (2);
    model.flangerDelayTimeGlobalValue.store (9.0f);
    model.flangerDelayTimeModeGlobalValue.store (1);
    model.flangerMixGlobalValue.store (0.7f);
    model.flangerMixModeGlobalValue.store (2);
    model.flangerFeedbackGlobalValue.store (0.4f);
    model.flangerFeedbackModeGlobalValue.store (1);

    juce::MemoryBlock block;
    model.saveState (block);
    CHECK (block.getSize() > 0);

    SlicerModel restored;
    restored.onPickStateInvalidated = [] {};
    restored.restoreState (block.getData(), (int) block.getSize());

    CHECK (restored.loopLengthBars.load() == 3);
    CHECK (restored.manualBpmOverrideEnabled.load());
    CHECK (restored.manualBpmOverrideValue.load() == doctest::Approx (128.5));
    CHECK (restored.currentSensitivity.load() == doctest::Approx (0.42f));
    CHECK (restored.fadeInMs.load() == doctest::Approx (7.0f));
    CHECK (restored.fadeOutMs.load() == doctest::Approx (23.0f));
    CHECK (restored.quantizeTransientsEnabled.load());
    CHECK (restored.quantizeGridIndex.load() == 5);
    CHECK (restored.triggerMode.load() == SlicerModel::TriggerMode::clock);
    CHECK (restored.clockReferenceIndex.load() == 9);
    CHECK (restored.tapeStopScope.load() == SlicerModel::TapeStopScope::perTick);
    CHECK (restored.filterSweepScope.load() == SlicerModel::FilterSweepScope::wholeWindow);
    CHECK (restored.resetBarsIndex.load() == 3);
    CHECK (restored.pitchMode.load() == SlicerModel::PitchMode::timeStretch);
    CHECK (restored.grainSizeMs.load() == doctest::Approx (45.0f));
    CHECK (restored.grainWindowShape.load() == SlicerModel::GrainWindowShape::triangular);
    CHECK (restored.pitchShiftSemitones.load() == doctest::Approx (3.5f));
    CHECK (! restored.beatQuantizeSliceLengthEnabled.load());
    CHECK (restored.beatQuantizeSliceLengthEnabledRepitch.load());

    CHECK (restored.stretchGrainSizeMsValue.load() == doctest::Approx (22.0f));
    CHECK (restored.stretchSpeedMultiplierValue.load() == doctest::Approx (2.5f));
    CHECK (restored.filterSweepResonanceValue.load() == doctest::Approx (3.2f));
    CHECK (restored.filterSweepFilterTypeValue.load() == 2);
    CHECK (restored.curveShapeValue.load() == 1);
    CHECK (restored.bitcrushRateReductionGlobalValue.load() == doctest::Approx (8.0f));
    CHECK (restored.bitcrushRateReductionModeGlobalValue.load() == 2);
    CHECK (restored.bitcrushBitDepthGlobalValue.load() == doctest::Approx (11.0f));
    CHECK (restored.bitcrushBitDepthModeGlobalValue.load() == 1);
    CHECK (restored.scratchRateGlobalValue.load() == 13);
    CHECK (restored.scratchForwardCurveGlobalValue.load() == 3);
    CHECK (restored.scratchBackwardCurveGlobalValue.load() == 2);
    CHECK (restored.flangerDelayTimeGlobalValue.load() == doctest::Approx (9.0f));
    CHECK (restored.flangerDelayTimeModeGlobalValue.load() == 1);
    CHECK (restored.flangerMixGlobalValue.load() == doctest::Approx (0.7f));
    CHECK (restored.flangerMixModeGlobalValue.load() == 2);
    CHECK (restored.flangerFeedbackGlobalValue.load() == doctest::Approx (0.4f));
    CHECK (restored.flangerFeedbackModeGlobalValue.load() == 1);
}

TEST_CASE ("SlicerModel restoreState clamps out-of-range indices and values")
{
    SlicerModel model;
    model.onPickStateInvalidated = [] {};

    juce::XmlElement root ("neditvst");
    auto* globals = root.createNewChildElement ("globals");
    globals->setAttribute ("triggerMode", 99);
    globals->setAttribute ("clockReferenceIndex", 999);
    globals->setAttribute ("tapeStopScope", -5);
    globals->setAttribute ("pitchMode", 42);
    globals->setAttribute ("grainWindowShape", 123);
    globals->setAttribute ("beatQuantizeSliceLengthEnabledRepitch", true);

    auto* styleGlobals = root.createNewChildElement ("styleGlobals");
    styleGlobals->setAttribute ("filterSweepFilterType", 77);
    styleGlobals->setAttribute ("scratchRate", 12345);
    styleGlobals->setAttribute ("scratchBackwardCurve", -2);
    styleGlobals->setAttribute ("stretchSpeedMultiplier", 999.0f);

    const juce::String xml = root.toString();
    model.restoreState (xml.toRawUTF8(), xml.getNumBytesAsUTF8());

    CHECK (model.triggerMode.load() == SlicerModel::TriggerMode::performance);
    CHECK (model.clockReferenceIndex.load() == SlicerModel::numNoteValueOptions - 1);
    CHECK (model.tapeStopScope.load() == SlicerModel::TapeStopScope::wholeWindow);
    CHECK (model.pitchMode.load() == SlicerModel::PitchMode::timeStretch);
    CHECK (model.grainWindowShape.load() == SlicerModel::GrainWindowShape::triangular);
    CHECK (model.beatQuantizeSliceLengthEnabledRepitch.load());

    CHECK (model.filterSweepFilterTypeValue.load() == SlicerModel::numFilterSweepFilterTypeOptions - 1);
    CHECK (model.scratchRateGlobalValue.load() == SlicerModel::numNoteValueOptions - 1);
    CHECK (model.scratchBackwardCurveGlobalValue.load() == 0);
    CHECK (model.stretchSpeedMultiplierValue.load() == doctest::Approx (SlicerModel::maxStretchSpeedMultiplier));
}

TEST_CASE ("SlicerModel restoreState tolerates garbage, wrong roots, and absent sections")
{
    SlicerModel model;
    model.onPickStateInvalidated = [] {};
    model.loopLengthBars.store (3);

    const juce::String garbage ("this is not xml at all");
    model.restoreState (garbage.toRawUTF8(), garbage.getNumBytesAsUTF8());
    CHECK (model.loopLengthBars.load() == 3);

    juce::XmlElement wrongRoot ("somethingElse");
    const juce::String wrongRootXml = wrongRoot.toString();
    model.restoreState (wrongRootXml.toRawUTF8(), wrongRootXml.getNumBytesAsUTF8());
    CHECK (model.loopLengthBars.load() == 3);

    // A valid root with no globals section leaves current state untouched.
    juce::XmlElement emptyRoot ("neditvst");
    const juce::String emptyXml = emptyRoot.toString();
    model.restoreState (emptyXml.toRawUTF8(), emptyXml.getNumBytesAsUTF8());
    CHECK (model.loopLengthBars.load() == 3);
}

TEST_CASE ("SlicerModel restoreState size-guards slice-indexed tables against a different sample")
{
    SlicerModel source;
    source.onPickStateInvalidated = [] {};
    source.slices.resize (2);
    source.resetSequencerGrid();
    source.sliceProbabilities.assign (2, 0.25f);
    source.subdivisionProbabilities.assign (SlicerModel::numNoteValueOptions, 0.5f);
    source.playbackStyleProbabilities.assign (SlicerModel::numPlaybackStyleOptions, 0.25f);
    source.sequencerGrid[0] = 3; // Stretch in row 0, column 0
    source.sequencerCellParameterOverrides[0]["Grain Size"] = 30.0f;
    source.sequencerCellExtendedLengthSteps[0] = 4;

    juce::MemoryBlock block;
    source.saveState (block);

    // Different slice count -> grid + slice probabilities must be skipped.
    SlicerModel restored;
    restored.onPickStateInvalidated = [] {};
    restored.slices.resize (5);
    restored.sliceProbabilities.assign (5, 1.0f);
    restored.resetSequencerGrid();
    restored.restoreState (block.getData(), (int) block.getSize());

    CHECK (restored.sliceProbabilities.size() == 5); // untouched default, not the stored size-2 table
    CHECK (restored.subdivisionProbabilities[(size_t) 0] == doctest::Approx (0.5f));
    CHECK (restored.playbackStyleProbabilities[(size_t) 0] == doctest::Approx (0.25f));
    CHECK (restored.sequencerGrid[(size_t) 0] == -1); // grid not overwritten
    CHECK (restored.sequencerCellParameterOverrides.empty());
    CHECK (restored.sequencerCellExtendedLengthSteps.empty());
}

TEST_CASE ("SlicerModel saveState/restoreState round-trips the sequencer grid, pattern bank, and performance bank")
{
    SlicerModel source;
    source.onPickStateInvalidated = [] {};
    source.slices.resize (2);
    source.resetSequencerGrid();
    source.sequencerGrid[0] = 3;
    source.sequencerCellParameterOverrides[0]["Grain Size"] = 30.0f;
    source.sequencerCellExtendedLengthSteps[0] = 4;

    SlicerModel::SequencerPatternSnapshot pattern;
    pattern.populated = true;
    pattern.rows = 2;
    pattern.columns = 8;
    pattern.grid.assign (16, -1);
    pattern.grid[3] = 1; // Ping-Pong at row 0, column 3
    pattern.stepResolutionIndex = 5;
    pattern.patternLengthBarsIndex = 1;
    pattern.parameterOverrides[3]["Curve Shape"] = 1.0f;
    pattern.extendedLengthSteps[3] = 2;
    source.patternBank[60] = pattern;

    SlicerModel::PerformanceStateSnapshot state;
    state.populated = true;
    state.trimStartSample = 100;
    state.trimEndSample = 44100;
    state.style = 4; // Filter Down
    state.loop = true;
    state.sync = false;
    state.parameterValues.fill (0.5f);
    state.parameterValues[5] = 2.0f; // Subdivide
    state.parameterValues[20] = 1.0f; // Volume Mode
    source.performanceStateBank[90] = state;

    source.setPerformanceWorkingStyle (6);
    source.setPerformanceWorkingParameterValue (0, 2.5f);
    source.setPerformanceWorkingLoop (true);
    source.setPerformanceWorkingSync (false);
    source.focusedPerformanceStateSlot.store (90);

    juce::MemoryBlock block;
    source.saveState (block);

    SlicerModel restored;
    restored.onPickStateInvalidated = [] {};
    restored.slices.resize (2); // same slice count as source -> grid dimensions match
    restored.resetSequencerGrid();
    restored.restoreState (block.getData(), (int) block.getSize());

    // Sequencer grid + per-cell data.
    CHECK (restored.sequencerGrid[(size_t) 0] == 3);
    const float restoredGrainSize = restored.sequencerCellParameterOverrides[0]["Grain Size"];
    CHECK (restoredGrainSize == doctest::Approx (30.0f));
    const int restoredExtendedSteps = restored.sequencerCellExtendedLengthSteps[0];
    CHECK (restoredExtendedSteps == 4);

    // Pattern bank slot.
    const auto& restoredPattern = restored.patternBank[60];
    CHECK (restoredPattern.populated);
    CHECK (restoredPattern.rows == 2);
    CHECK (restoredPattern.columns == 8);
    CHECK (restoredPattern.grid[(size_t) 3] == 1);
    CHECK (restoredPattern.stepResolutionIndex == 5);
    CHECK (restoredPattern.patternLengthBarsIndex == 1);
    const float restoredCurve = restoredPattern.parameterOverrides.at (3).at ("Curve Shape");
    CHECK (restoredCurve == doctest::Approx (1.0f));
    const int restoredPatternExtended = restoredPattern.extendedLengthSteps.at (3);
    CHECK (restoredPatternExtended == 2);
    CHECK (! restored.patternBank[61].populated);

    // Performance bank slot.
    const auto& restoredState = restored.performanceStateBank[90];
    CHECK (restoredState.populated);
    CHECK (restoredState.trimStartSample == 100);
    CHECK (restoredState.trimEndSample == 44100);
    CHECK (restoredState.style == 4);
    CHECK (restoredState.loop);
    CHECK (! restoredState.sync);
    CHECK (restoredState.parameterValues[(size_t) 0] == doctest::Approx (0.5f));
    CHECK (restoredState.parameterValues[(size_t) 5] == doctest::Approx (2.0f));
    CHECK (restoredState.parameterValues[(size_t) 20] == doctest::Approx (1.0f));
    CHECK (! restored.performanceStateBank[91].populated);

    // Performance working state + focus.
    CHECK (restored.getPerformanceWorkingStyle() == 6);
    CHECK (restored.getPerformanceWorkingParameterValue (0) == doctest::Approx (2.5f));
    CHECK (restored.getPerformanceWorkingLoop());
    CHECK (! restored.getPerformanceWorkingSync());
    CHECK (restored.focusedPerformanceStateSlot.load() == 90);
}
