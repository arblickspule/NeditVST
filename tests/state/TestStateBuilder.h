// Shared test helper: builds a PluginState with every field moved off its
// default (and internally consistent, so sanitize() leaves it unchanged).
// Used by the binary and JSON round-trip tests -- if serialization drops
// or corrupts any field, equality with this state fails.

#pragma once

#include <state/PluginState.h>

namespace nedit::test {

[[nodiscard]] inline nedit::state::PluginState makeFullyMutatedState()
{
    using namespace nedit::state;

    PluginState state;

    state.triggerMode = TriggerMode::sequenced;

    // --- sample ---
    auto& sample = state.sample;
    sample.samplePath = "/home/user/samples/break beat #1 (mästered).wav";
    sample.sampleContentHash = 0x0123456789abcdefULL;
    sample.sampleLengthFrames = 220500;
    sample.sampleSampleRate = 48000.0;
    sample.trimStartFrame = 1200;
    sample.trimEndFrame = 219000;
    sample.sensitivity = 0.73f;
    sample.quantizeTransients = true;
    sample.quantizeGridIndex = kNoteValue16n;
    sample.manualPoints = { { 1, 5000 }, { 2, 90000 }, { 7, 180000 } };
    sample.excludedPoints = { { 1, 22000 } };
    sample.nextManualPointId = 8;
    sample.nextExcludedPointId = 2;
    sample.loopLengthBars = 2;
    sample.manualBpmOverrideEnabled = true;
    sample.manualBpmOverrideValue = 174.5;

    // --- render ---
    auto& render = state.render;
    render.fadeInMs = 2.5f;
    render.fadeOutMs = 30.0f;
    render.pitchMode = PitchMode::timeStretch;
    render.grainSizeMs = 90.0f;
    render.grainWindowShape = GrainWindowShape::triangular;
    render.pitchShiftSemitones = -7.0f;
    render.beatQuantizeTimeStretch = false;
    render.beatQuantizeRepitch = true;

    // --- generate ---
    auto& generate = state.generate;
    generate.generateMode = TriggerMode::clock;
    generate.sliceWeights = { 1.0f, 0.0f, 0.5f, 0.25f, 1.0f };
    generate.styleWeights = { { 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f, 0.9f } };
    generate.styleParams.set (StyleParamId::filterResonance, 6.5f);
    generate.styleParams.set (StyleParamId::subdivide, 9.0f);
    generate.styleParams.set (StyleParamId::flangerMix, 0.8f);
    generate.resetBarsIndex = 3;
    generate.clockReferenceIndex = kNoteValue16n;
    generate.subdivisionWeights[0] = 0.0f;
    generate.subdivisionWeights[7] = 0.5f;
    generate.subdivisionWeights[19] = 0.25f;
    generate.tapeStopScope = WindowScope::perTick;
    generate.filterSweepScope = WindowScope::wholeWindow;

    // --- sequencer ---
    auto& sequencer = state.sequencer;
    sequencer.stepResolutionIndex = kNoteValue4n;
    sequencer.patternLengthBarsIndex = 2;
    sequencer.rows = 4;
    sequencer.columns = 16;
    sequencer.grid.assign (static_cast<std::size_t> (4 * 16), static_cast<std::int8_t> (-1));
    sequencer.grid[0] = 0;
    sequencer.grid[17] = 3;
    sequencer.grid[35] = 8;
    sequencer.overrides[17] = { { StyleParamId::grainSizeMs, 22.0f },
                                { StyleParamId::volume, 0.6f } };
    sequencer.extensions[17] = 3;
    sequencer.fallbackParams.set (StyleParamId::bitDepth, 8.0f);
    sequencer.randomizeStyleWeights = { { 0.9f, 0.8f, 0.7f, 0.6f, 0.5f, 0.4f, 0.3f, 0.2f, 0.1f } };
    sequencer.randomizeParametersForStyle[2] = true;
    sequencer.randomizeParametersForStyle[7] = true;
    sequencer.selectedDrawingStyle = 5;

    auto& pattern = sequencer.patternBank[60];
    pattern.populated = true;
    pattern.rows = 3;
    pattern.columns = 8;
    pattern.stepResolutionIndex = kNoteValue16n;
    pattern.patternLengthBarsIndex = 1;
    pattern.grid.assign (static_cast<std::size_t> (3 * 8), static_cast<std::int8_t> (-1));
    pattern.grid[5] = 2;
    pattern.overrides[5] = { { StyleParamId::curveShape, 1.0f } };
    pattern.extensions[5] = 2;

    sequencer.patternSwitchTiming = PatternSwitchTiming::endOfPattern;
    sequencer.patternSwitchIntervalIndex = kNoteValue4n;

    // --- performance ---
    auto& performance = state.performance;

    auto& slot36 = performance.bank[36];
    slot36.populated = true;
    slot36.trimStartFrame = 4000;
    slot36.trimEndFrame = 60000;
    slot36.style = 7;
    slot36.params.set (StyleParamId::scratchRate, 3.0f);
    slot36.params.set (StyleParamId::scratchForwardCurve, 2.0f);
    slot36.loop = true;
    slot36.sync = false;

    auto& slot127 = performance.bank[127];
    slot127.populated = true;
    slot127.trimStartFrame = 100000;
    slot127.trimEndFrame = 220500;
    slot127.style = 4;

    performance.workingState.populated = true;
    performance.workingState.trimStartFrame = 500;
    performance.workingState.trimEndFrame = 30000;
    performance.workingState.style = 2;
    performance.workingState.params.set (StyleParamId::curveShape, 1.0f);
    performance.workingState.loop = true;

    performance.focusedSlot = 36;
    performance.trimSnapMode = TrimSnapMode::grid;
    performance.trimGridIndex = kNoteValue16n;
    performance.quantizeRecallEnabled = true;
    performance.quantizeRecallIntervalIndex = kNoteValue1n;

    // --- control ---
    auto& control = state.control;
    control.baseNote = 48;
    control.gateMode = true;
    control.activeStyle = 6;
    control.styleParams.set (StyleParamId::srReduction, 24.0f);

    // --- ui ---
    state.ui.activeTab = UiTab::perform;
    state.ui.visibleStartNorm = 0.25;
    state.ui.visibleEndNorm = 0.75;

    return state;
}

} // namespace nedit::test
