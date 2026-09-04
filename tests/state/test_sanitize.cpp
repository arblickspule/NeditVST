// sanitize(): out-of-range values are clamped, never crash, never remain
// out of range.

#include <catch2/catch_test_macros.hpp>

#include <state/PluginState.h>

using namespace nedit::state;

TEST_CASE ("sample sanitize clamps trim into the sample", "[sanitize]")
{
    SampleState sample;
    sample.samplePath = "/tmp/test.wav";
    sample.sampleLengthFrames = 44100;
    sample.trimStartFrame = -50;
    sample.trimEndFrame = 100000;
    sample.sanitize();

    CHECK (sample.trimStartFrame == 0);
    CHECK (sample.trimEndFrame == 44100);
}

TEST_CASE ("sample sanitize enforces the minimum trim gap", "[sanitize]")
{
    SampleState sample;
    sample.sampleLengthFrames = 44100;
    sample.trimStartFrame = 1000;
    sample.trimEndFrame = 1010;  // less than the 64-frame minimum gap
    sample.sanitize();

    CHECK (sample.trimEndFrame - sample.trimStartFrame >= SampleState::kMinTrimGapFrames);

    // Gap at the very end of the sample: start must move back.
    sample.trimStartFrame = 44100;
    sample.trimEndFrame = 44100;
    sample.sanitize();

    CHECK (sample.trimEndFrame <= 44100);
    CHECK (sample.trimEndFrame - sample.trimStartFrame >= SampleState::kMinTrimGapFrames);
}

TEST_CASE ("sample sanitize with no sample zeroes the trim", "[sanitize]")
{
    SampleState sample;
    sample.trimStartFrame = 5;
    sample.trimEndFrame = 500;
    sample.sanitize();

    CHECK (sample.trimStartFrame == 0);
    CHECK (sample.trimEndFrame == 0);
}

TEST_CASE ("generate sanitize clamps weights and indices", "[sanitize]")
{
    GenerateState generate;
    generate.generateMode = TriggerMode::performance;  // invalid for the Generate tab
    generate.sliceWeights = { -1.0f, 0.5f, 2.0f };
    generate.styleWeights[0] = 5.0f;
    generate.clockReferenceIndex = 99;
    generate.resetBarsIndex = -3;
    generate.sanitize();

    CHECK (generate.generateMode == TriggerMode::sliceLength);
    CHECK (generate.sliceWeights == std::vector<float> { 0.0f, 0.5f, 1.0f });
    CHECK (generate.styleWeights[0] == 1.0f);
    CHECK (generate.clockReferenceIndex == kNoteValue4n);
    CHECK (generate.resetBarsIndex == 0);
}

TEST_CASE ("sequencer sanitize resizes the grid and drops orphan overrides", "[sanitize]")
{
    SequencerState sequencer;
    sequencer.rows = 4;
    sequencer.columns = 8;
    sequencer.grid.assign (10, static_cast<std::int8_t> (99));  // wrong size, invalid cells
    sequencer.overrides[100] = { { StyleParamId::filterResonance, 0.5f } };  // out of grid
    sequencer.overrides[3] = { { StyleParamId::filterResonance, 999.0f } };  // in grid, out of range
    sequencer.extensions[500] = 4;  // out of grid
    sequencer.sanitize();

    CHECK (sequencer.grid.size() == 32);

    for (const auto cell : sequencer.grid)
        CHECK ((cell >= -1 && cell < kNumPlaybackStyles));

    CHECK (sequencer.overrides.count (100) == 0);
    CHECK (sequencer.overrides.count (3) == 1);
    CHECK (sequencer.overrides[3][StyleParamId::filterResonance] == kMaxFilterResonance);
    CHECK (sequencer.extensions.empty());
}

TEST_CASE ("sequencer sanitize wipes unpopulated bank slots", "[sanitize]")
{
    SequencerState sequencer;
    sequencer.patternBank[5].grid = { 1, 2, 3 };  // garbage in an unpopulated slot
    sequencer.sanitize();

    CHECK (sequencer.patternBank[5].grid.empty());
}

TEST_CASE ("sequencer sanitize clamps the grid viewport", "[sanitize]")
{
    SequencerState sequencer;
    sequencer.viewport = { 100.0, -5.0, 3.0, -0.5 };
    sequencer.sanitize();

    CHECK (sequencer.viewport.zoomX == kMaxSequencerZoom);
    CHECK (sequencer.viewport.zoomY == kMinSequencerZoom);
    CHECK (sequencer.viewport.originX == 1.0);
    CHECK (sequencer.viewport.originY == 0.0);
}

TEST_CASE ("sequencer viewport sanitize is idempotent", "[sanitize]")
{
    SequencerGridViewport vp { 1.0, 0.5, 0.5, 0.5 };
    const auto before = vp;
    vp.sanitize();
    CHECK (vp == before);
}

TEST_CASE ("performance sanitize clamps snapshots to the sample", "[sanitize]")
{
    PerformanceState performance;
    performance.bank[10].populated = true;
    performance.bank[10].trimStartFrame = -5;
    performance.bank[10].trimEndFrame = 1'000'000;
    performance.bank[10].style = 42;
    performance.focusedSlot = 500;
    performance.sanitize (44100);

    CHECK (performance.bank[10].trimStartFrame == 0);
    CHECK (performance.bank[10].trimEndFrame == 44100);
    CHECK (performance.bank[10].style == kNumPlaybackStyles - 1);
    CHECK (performance.focusedSlot == kNumMidiNotes - 1);
}

TEST_CASE ("control sanitize clamps notes and styles", "[sanitize]")
{
    ControlState control;
    control.baseNote = 300;
    control.activeStyle = -2;
    control.sanitize();

    CHECK (control.baseNote == 127);
    CHECK (control.activeStyle == 0);
}

TEST_CASE ("ui sanitize repairs inverted zoom", "[sanitize]")
{
    UiState ui;
    ui.visibleStartNorm = 0.9;
    ui.visibleEndNorm = 0.1;
    ui.sanitize();

    CHECK (ui.visibleStartNorm == 0.0);
    CHECK (ui.visibleEndNorm == 1.0);
}

TEST_CASE ("full plugin state sanitize is idempotent", "[sanitize]")
{
    PluginState state;
    state.sample.sampleLengthFrames = 44100;
    state.sample.trimEndFrame = 44100;
    state.sanitize();

    PluginState again = state;
    again.sanitize();

    CHECK (again == state);
}
