#include <doctest/doctest.h>
#include "model/SlicerModel.h"

TEST_CASE ("SlicerModel tempo math with no sample")
{
    SlicerModel model;

    CHECK (model.getCalculatedOriginalBpm() == 0.0);
    CHECK (model.computeSourceSpanSeconds() == 0.0);
    CHECK (model.computeMinimumHoldoffMs() == SlicerModel::defaultHoldoffMs);
}

TEST_CASE ("SlicerModel getCalculatedOriginalBpm from the trim span")
{
    SlicerModel model;

    // 2 seconds of mono audio, populated directly through the public state.
    model.sampleBuffer.setSize (1, 44100 * 2);
    model.sampleBuffer.clear();
    model.sampleSampleRate = 44100.0;
    model.sampleLoaded = true;

    model.tempoTrimStartSample.store (0);
    model.tempoTrimEndSample.store (44100 * 2);

    // One bar (4 beats) over the full 2s span -> 120 BPM.
    model.loopLengthBars.store (1);
    CHECK (doctest::Approx (model.getCalculatedOriginalBpm()) == 120.0);

    // Same span, two bars -> 240 BPM.
    model.loopLengthBars.store (2);
    CHECK (doctest::Approx (model.getCalculatedOriginalBpm()) == 240.0);

    // Trim-only span change: half the span at one bar -> 240 BPM.
    model.loopLengthBars.store (1);
    model.tempoTrimEndSample.store (44100);
    CHECK (doctest::Approx (model.getCalculatedOriginalBpm()) == 240.0);
}

TEST_CASE ("SlicerModel manual BPM override replaces the bars-derived tempo")
{
    SlicerModel model;

    model.sampleBuffer.setSize (1, 44100);
    model.sampleBuffer.clear();
    model.sampleSampleRate = 44100.0;
    model.sampleLoaded = true;
    model.tempoTrimStartSample.store (0);
    model.tempoTrimEndSample.store (44100);
    model.loopLengthBars.store (4);

    // 4 bars over 1s would be 960 BPM from bars; the override wins outright.
    model.setManualBpmOverrideEnabled (true);
    model.setManualBpmOverrideValue (87.0);
    CHECK (doctest::Approx (model.getCalculatedOriginalBpm()) == 87.0);

    // Disabling the override restores the bars-derived value.
    model.setManualBpmOverrideEnabled (false);
    CHECK (doctest::Approx (model.getCalculatedOriginalBpm()) == 960.0);
}

TEST_CASE ("SlicerModel computeMinimumHoldoffMs scales with tempo")
{
    SlicerModel model;

    // No sample -> the fixed 30ms floor.
    CHECK (model.computeMinimumHoldoffMs() == SlicerModel::defaultHoldoffMs);

    // 120 BPM -> one 32nd note = 60000 / 120 / 8 = 62.5ms.
    model.sampleBuffer.setSize (1, 44100 * 2);
    model.sampleBuffer.clear();
    model.sampleSampleRate = 44100.0;
    model.sampleLoaded = true;
    model.tempoTrimStartSample.store (0);
    model.tempoTrimEndSample.store (44100 * 2);
    model.loopLengthBars.store (1);
    CHECK (doctest::Approx (model.computeMinimumHoldoffMs()) == 62.5);

    // 240 BPM -> 31.25ms.
    model.loopLengthBars.store (2);
    CHECK (doctest::Approx (model.computeMinimumHoldoffMs()) == 31.25);
}

TEST_CASE ("SlicerModel trim clamping with snapToTransient disabled")
{
    SlicerModel model;
    model.onPickStateInvalidated = [] {}; // slice rebuilds invoke this; no engine in tests

    model.sampleBuffer.setSize (1, 44100);
    model.sampleBuffer.clear();
    model.sampleSampleRate = 44100.0;
    model.sampleLoaded = true;

    // End starts at 0: trim start clamps to [0, end - minTrimGap] = {0}.
    model.setTrimStartSample (1000, false);
    CHECK (model.getTrimStartSample() == 0);

    // Trim end clamps to [start + minTrimGap, bufferLength].
    model.setTrimEndSample (44100, false);
    CHECK (model.getTrimEndSample() == 44100);

    model.setTrimEndSample (100000, false); // beyond the buffer -> clamped down
    CHECK (model.getTrimEndSample() == 44100);

    model.setTrimEndSample (10, false);     // below start + gap -> clamped up
    CHECK (model.getTrimEndSample() == 64);

    // Trim start clamps to [0, end - minTrimGap].
    model.setTrimStartSample (100, false);  // beyond end - gap -> clamped down
    CHECK (model.getTrimStartSample() == 0);

    model.setTrimStartSample (0, false);
    CHECK (model.getTrimStartSample() == 0);
}

TEST_CASE ("SlicerModel quantize grid index clamps to the note-value palette")
{
    SlicerModel model;
    model.onPickStateInvalidated = [] {}; // slice rebuilds invoke this; no engine in tests

    model.setQuantizeGridIndex (7); // 16n
    CHECK (model.getQuantizeGridIndex() == 7);

    model.setQuantizeGridIndex (0);
    CHECK (model.getQuantizeGridIndex() == 0);

    model.setQuantizeGridIndex (-5); // clamps to 0
    CHECK (model.getQuantizeGridIndex() == 0);

    model.setQuantizeGridIndex (500); // clamps to the last palette entry
    CHECK (model.getQuantizeGridIndex() == SlicerModel::numNoteValueOptions - 1);
}

TEST_CASE ("SlicerModel getSequencerNaturalLengthSteps from slice spans")
{
    SlicerModel model;

    // No slices -- any row (including out-of-range) falls back to 1.
    CHECK (model.getSequencerNaturalLengthSteps (0) == 1);
    CHECK (model.getSequencerNaturalLengthSteps (-1) == 1);

    // 120 BPM sample (1 bar over 2s) with three known slice lengths:
    // 1 beat, 0.5 beats, and 2 beats (beat = 0.5s = 22050 samples @ 44100).
    model.sampleBuffer.setSize (1, 44100 * 2);
    model.sampleBuffer.clear();
    model.sampleSampleRate = 44100.0;
    model.sampleLoaded = true;
    model.tempoTrimStartSample.store (0);
    model.tempoTrimEndSample.store (44100 * 2);
    model.loopLengthBars.store (1);
    model.slices = { { 0, 22050 }, { 22050, 33075 }, { 33075, 77175 } };

    // Default step resolution is 16n (0.25 beats).
    model.stepResolutionIndex.store (7);
    CHECK (model.getSequencerNaturalLengthSteps (0) == 4);
    CHECK (model.getSequencerNaturalLengthSteps (1) == 2);
    CHECK (model.getSequencerNaturalLengthSteps (2) == 8);

    // Out-of-range row clamps to 1 even with slices present.
    CHECK (model.getSequencerNaturalLengthSteps (3) == 1);

    // 8n (0.5 beats) halves each natural length.
    model.stepResolutionIndex.store (10);
    CHECK (model.getSequencerNaturalLengthSteps (0) == 2);
    CHECK (model.getSequencerNaturalLengthSteps (2) == 4);

    // 4n (1.0 beats) makes each slice's length in beats its step count.
    model.stepResolutionIndex.store (13);
    CHECK (model.getSequencerNaturalLengthSteps (0) == 1);
    CHECK (model.getSequencerNaturalLengthSteps (2) == 2);

    // A slice too short to reach one step clamps up to 1.
    model.slices = { { 0, 100 } };
    model.stepResolutionIndex.store (7);
    CHECK (model.getSequencerNaturalLengthSteps (0) == 1);

    // A zero-length slice is treated the same way (sliceLength > 0 guard).
    model.slices = { { 1000, 1000 } };
    CHECK (model.getSequencerNaturalLengthSteps (0) == 1);

    // No loaded sample -> getCalculatedOriginalBpm() is 0 -> even a real
    // slice falls back to 1.
    SlicerModel bare;
    bare.slices = { { 0, 22050 } };
    CHECK (bare.getSequencerNaturalLengthSteps (0) == 1);
}
