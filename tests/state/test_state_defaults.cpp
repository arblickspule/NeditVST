// Default-constructed PluginState must match the original plugin's
// documented defaults, and the separated (previously shared) state must
// genuinely be independent.

#include <catch2/catch_test_macros.hpp>

#include <state/PluginState.h>

using namespace nedit::state;

TEST_CASE ("top-level defaults", "[defaults]")
{
    const PluginState state;

    CHECK (state.triggerMode == TriggerMode::sliceLength);
    CHECK_FALSE (state.sample.hasSample());
}

TEST_CASE ("sample defaults", "[defaults]")
{
    const SampleState sample;

    CHECK (sample.sensitivity == 0.5f);
    CHECK_FALSE (sample.quantizeTransients);
    CHECK (sample.quantizeGridIndex == kNoteValue4n);
    CHECK (sample.loopLengthBars == 1);
    CHECK_FALSE (sample.manualBpmOverrideEnabled);
    CHECK (sample.manualBpmOverrideValue == 120.0);
    CHECK (sample.manualPoints.empty());
    CHECK (sample.excludedPoints.empty());
    CHECK (sample.nextManualPointId == 1);
    CHECK (sample.nextExcludedPointId == 1);
}

TEST_CASE ("render defaults", "[defaults]")
{
    const RenderState render;

    CHECK (render.fadeInMs == 5.0f);
    CHECK (render.fadeOutMs == 10.0f);
    CHECK (render.pitchMode == PitchMode::repitch);
    CHECK (render.grainSizeMs == 60.0f);
    CHECK (render.grainWindowShape == GrainWindowShape::hann);
    CHECK (render.pitchShiftSemitones == 0.0f);
    CHECK (render.beatQuantizeTimeStretch);          // deliberately different
    CHECK_FALSE (render.beatQuantizeRepitch);        // defaults per pitch mode
}

TEST_CASE ("generate defaults", "[defaults]")
{
    const GenerateState generate;

    CHECK (generate.generateMode == TriggerMode::sliceLength);
    CHECK (generate.sliceWeights.empty());

    // Forward-only style weights.
    CHECK (generate.styleWeights[0] == 1.0f);
    for (std::size_t i = 1; i < kNumPlaybackStyles; ++i)
        CHECK (generate.styleWeights[i] == 0.0f);

    CHECK (generate.resetBarsIndex == kDefaultResetBarsIndex);
    CHECK (generate.clockReferenceIndex == kNoteValue4n);

    for (const float w : generate.subdivisionWeights)
        CHECK (w == 1.0f);

    // Opposite scope defaults are deliberate.
    CHECK (generate.tapeStopScope == WindowScope::wholeWindow);
    CHECK (generate.filterSweepScope == WindowScope::perTick);
}

TEST_CASE ("sequencer defaults", "[defaults]")
{
    const SequencerState sequencer;

    CHECK (sequencer.stepResolutionIndex == kNoteValue16n);
    CHECK (sequencer.patternLengthBarsIndex == kDefaultPatternLengthBarsIndex);
    CHECK (sequencer.rows == 0);
    CHECK (sequencer.columns == 0);
    CHECK (sequencer.grid.empty());
    CHECK (sequencer.overrides.empty());
    CHECK (sequencer.extensions.empty());
    CHECK (sequencer.selectedDrawingStyle == 0);
    CHECK (sequencer.patternSwitchTiming == PatternSwitchTiming::immediate);
    CHECK (sequencer.patternSwitchIntervalIndex == kNoteValue1n);

    // The randomizer's own weights (decoupled from Generate's).
    CHECK (sequencer.randomizeStyleWeights[0] == 1.0f);

    for (const auto& pattern : sequencer.patternBank)
        CHECK_FALSE (pattern.populated);

    for (const bool flag : sequencer.randomizeParametersForStyle)
        CHECK_FALSE (flag);
}

TEST_CASE ("performance defaults", "[defaults]")
{
    const PerformanceState performance;

    CHECK (performance.focusedSlot == -1);
    CHECK (performance.trimSnapMode == TrimSnapMode::transients);
    CHECK (performance.trimGridIndex == kNoteValue4n);
    CHECK_FALSE (performance.quantizeRecallEnabled);
    CHECK (performance.quantizeRecallIntervalIndex == kNoteValue4n);

    for (const auto& snapshot : performance.bank)
        CHECK_FALSE (snapshot.populated);

    // Snapshot flag defaults: one-shot, synced.
    const PerformanceSnapshot snapshot;
    CHECK_FALSE (snapshot.loop);
    CHECK (snapshot.sync);
}

TEST_CASE ("control defaults", "[defaults]")
{
    const ControlState control;

    CHECK (control.baseNote == 36);  // C1
    CHECK_FALSE (control.gateMode);
    CHECK (control.activeStyle == 0);
}

TEST_CASE ("ui defaults", "[defaults]")
{
    const UiState ui;

    CHECK (ui.activeTab == UiTab::generate);
    CHECK (ui.visibleStartNorm == 0.0);
    CHECK (ui.visibleEndNorm == 1.0);
}

TEST_CASE ("previously-shared state is now independent", "[defaults][pitfalls]")
{
    PluginState state;

    // Pitfall 1 (original): style probabilities shared between Generate
    // and the sequencer's Randomize. Now: independent tables.
    state.generate.styleWeights[3] = 0.75f;
    CHECK (state.sequencer.randomizeStyleWeights[3] == 0.0f);

    // Pitfall 2 (original): Performance mode repointed the global trim.
    // Now: performance snapshots own their trim; the global trim is
    // untouched by construction.
    state.performance.workingState.trimStartFrame = 1000;
    state.performance.workingState.trimEndFrame = 2000;
    CHECK (state.sample.trimStartFrame == 0);
    CHECK (state.sample.trimEndFrame == 0);

    // Pitfall 3 (original): one global parameter table shared by
    // Generate, Control and sequencer fallback. Now: three copies.
    state.generate.styleParams.set (StyleParamId::filterResonance, 9.0f);
    CHECK (state.control.styleParams.filterResonance == 2.0f);
    CHECK (state.sequencer.fallbackParams.filterResonance == 2.0f);
}
