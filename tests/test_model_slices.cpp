#include <doctest/doctest.h>
#include "SlicerModel.h"

namespace
{
    // Populates a 2-second, 1-bar, 120 BPM sample so tempo-derived grid
    // math (bpm = 4 beats / 2s) works, plus matching trim markers.
    void setTempoSample (SlicerModel& model, int numSamples = 44100 * 2)
    {
        model.sampleBuffer.setSize (1, numSamples);
        model.sampleBuffer.clear();
        model.sampleSampleRate = 44100.0;
        model.sampleLoaded = true;
        model.tempoTrimStartSample.store (0);
        model.tempoTrimEndSample.store (numSamples);
        model.trimStartSample.store (0);
        model.trimEndSample.store (numSamples);
        model.loopLengthBars.store (1);
    }
}

TEST_CASE ("mergeOnsetsIntoSlices passes auto slices through unchanged")
{
    SlicerModel model;

    std::vector<Slice> autoSlices = { { 0, 1000 }, { 1000, 2000 }, { 2000, 3000 } };
    auto merged = model.mergeOnsetsIntoSlices (autoSlices, 0, 3000);

    REQUIRE (merged.size() == 3);
    CHECK (merged[0].startSample == 0);
    CHECK (merged[0].endSample == 1000);
    CHECK (merged[1].startSample == 1000);
    CHECK (merged[1].endSample == 2000);
    CHECK (merged[2].startSample == 2000);
    CHECK (merged[2].endSample == 3000);
}

TEST_CASE ("mergeOnsetsIntoSlices prepends the trim start when it is missing")
{
    SlicerModel model;

    std::vector<Slice> autoSlices = { { 500, 1500 } };
    auto merged = model.mergeOnsetsIntoSlices (autoSlices, 0, 2000);

    REQUIRE (merged.size() == 2);
    CHECK (merged[0].startSample == 0);
    CHECK (merged[0].endSample == 500);
    CHECK (merged[1].startSample == 500);
    CHECK (merged[1].endSample == 2000);
}

TEST_CASE ("mergeOnsetsIntoSlices merges manual points and dedups boundaries")
{
    SlicerModel model;
    model.manualPoints.push_back ({ 7, 1500 });

    // A manual point coinciding with an auto boundary (1000) is deduped.
    model.manualPoints.push_back ({ 8, 1000 });

    std::vector<Slice> autoSlices = { { 0, 1000 }, { 1000, 2000 } };
    auto merged = model.mergeOnsetsIntoSlices (autoSlices, 0, 2000);

    REQUIRE (merged.size() == 3);
    CHECK (merged[0].startSample == 0);
    CHECK (merged[0].endSample == 1000);
    CHECK (merged[1].startSample == 1000);
    CHECK (merged[1].endSample == 1500);
    CHECK (merged[2].startSample == 1500);
    CHECK (merged[2].endSample == 2000);
}

TEST_CASE ("mergeOnsetsIntoSlices filters manual points outside the trim")
{
    SlicerModel model;
    model.manualPoints.push_back ({ 7, 4000 }); // outside (0, 2000) -- soft-excluded

    std::vector<Slice> autoSlices = { { 0, 1000 }, { 1000, 2000 } };
    auto merged = model.mergeOnsetsIntoSlices (autoSlices, 0, 2000);

    REQUIRE (merged.size() == 2);
    CHECK (merged[0].endSample == 1000);
    CHECK (merged[1].endSample == 2000);
}

TEST_CASE ("mergeOnsetsIntoSlices applies exclusions by proximity")
{
    SlicerModel model;
    model.excludedPoints.push_back ({ 3, 1000 });

    std::vector<Slice> autoSlices = { { 0, 1000 }, { 1000, 8000 }, { 8000, 9000 } };
    auto merged = model.mergeOnsetsIntoSlices (autoSlices, 0, 9000);

    // The 1000 boundary is excluded (within the 50ms match tolerance = 2205
    // samples); the trim start and the 8000 boundary (7000 samples away) survive.
    REQUIRE (merged.size() == 2);
    CHECK (merged[0].startSample == 0);
    CHECK (merged[0].endSample == 8000);
    CHECK (merged[1].startSample == 8000);
    CHECK (merged[1].endSample == 9000);
}

TEST_CASE ("mergeOnsetsIntoSlices quantizes onsets to the grid when enabled")
{
    SlicerModel model;
    setTempoSample (model); // 120 BPM, 4n = 0.5s per beat
    model.quantizeTransientsEnabled.store (true);
    model.quantizeGridIndex.store (13);

    std::vector<Slice> autoSlices = { { 0, 57000 }, { 57000, 88200 } };
    auto merged = model.mergeOnsetsIntoSlices (autoSlices, 0, 88200);

    // 57000s -> 2.585 beats -> rounds to 3 beats = 1.5s = 66150.
    REQUIRE (merged.size() == 2);
    CHECK (merged[0].startSample == 0);
    CHECK (merged[0].endSample == 66150);
    CHECK (merged[1].startSample == 66150);
    CHECK (merged[1].endSample == 88200);
}

TEST_CASE ("quantizeOnsetToGrid snaps to the beat grid")
{
    SlicerModel model;
    setTempoSample (model);
    model.quantizeGridIndex.store (13); // 4n = 1 beat, 120 BPM -> 0.5s/beat

    // 1s = 2 beats exactly.
    CHECK (model.quantizeOnsetToGrid (44100, 0, 88200) == 44100);

    // 1.009s = 2.018 beats -> back down to 2 beats.
    CHECK (model.quantizeOnsetToGrid (44500, 0, 88200) == 44100);

    // 1.293s = 2.585 beats -> up to 3 beats = 1.5s.
    CHECK (model.quantizeOnsetToGrid (57000, 0, 88200) == 66150);

    // The trim start is a fixed point.
    CHECK (model.quantizeOnsetToGrid (0, 0, 88200) == 0);

    // An onset near the trim end clamps inside it (trimEnd - 1).
    CHECK (model.quantizeOnsetToGrid (88000, 0, 88200) == 88199);

    // A 16n grid (0.25 beats) lands a 0.5-beat onset on itself.
    model.quantizeGridIndex.store (7);
    CHECK (model.quantizeOnsetToGrid (11025, 0, 88200) == 11025);

    // No sample / no tempo -> passthrough.
    SlicerModel bare;
    CHECK (bare.quantizeOnsetToGrid (1234, 0, 100) == 1234);
}

TEST_CASE ("findNearestGridSample anchors at the tempo trim start")
{
    SlicerModel model;
    setTempoSample (model);
    model.performanceTrimGridIndex.store (13); // 4n

    // anchor = tempoTrimStartSample = 0; 1s -> 2 beats -> sample 44100.
    CHECK (model.findNearestGridSample (44100) == 44100);
    CHECK (model.findNearestGridSample (50000) == 44100);
    CHECK (model.findNearestGridSample (60000) == 66150);
}

TEST_CASE ("Manual slice points add, undo and redo")
{
    SlicerModel model;
    model.onPickStateInvalidated = [] {}; // slice rebuilds invoke this; no engine in tests
    model.trimStartSample.store (0);
    model.trimEndSample.store (88200);

    CHECK_FALSE (model.canUndoEdit());
    CHECK_FALSE (model.canRedoEdit());

    const int id = model.addManualSlicePoint (10000, false);
    CHECK (id == 1);

    auto points = model.getManualSlicePoints();
    REQUIRE (points.size() == 1);
    CHECK (points[0].id == 1);
    CHECK (points[0].samplePosition == 10000);

    // Undo removes it.
    CHECK (model.undoLastEdit());
    CHECK (model.getManualSlicePoints().empty());
    CHECK_FALSE (model.canUndoEdit());
    CHECK (model.canRedoEdit());

    // Redo restores it.
    CHECK (model.redoLastEdit());
    points = model.getManualSlicePoints();
    REQUIRE (points.size() == 1);
    CHECK (points[0].samplePosition == 10000);
}

TEST_CASE ("Manual slice points remove and undo restores them")
{
    SlicerModel model;
    model.onPickStateInvalidated = [] {};
    model.trimStartSample.store (0);
    model.trimEndSample.store (88200);

    const int id1 = model.addManualSlicePoint (10000, false);
    const int id2 = model.addManualSlicePoint (20000, false);
    CHECK (model.getManualSlicePoints().size() == 2);

    model.removeManualSlicePoint (id1);
    auto points = model.getManualSlicePoints();
    REQUIRE (points.size() == 1);
    CHECK (points[0].id == id2);

    CHECK (model.undoLastEdit());
    CHECK (model.getManualSlicePoints().size() == 2);
}

TEST_CASE ("Manual slice points clamp to the trim range")
{
    SlicerModel model;
    model.onPickStateInvalidated = [] {};
    model.trimStartSample.store (1000);
    model.trimEndSample.store (2000);

    const int id = model.addManualSlicePoint (50000, false);
    auto points = model.getManualSlicePoints();
    REQUIRE (points.size() == 1);
    CHECK (points[0].samplePosition == 1999); // trimEnd - 1
}

TEST_CASE ("Manual slice points snap to transients")
{
    SlicerModel model;
    model.onPickStateInvalidated = [] {};
    model.sampleBuffer.setSize (1, 44100);
    model.sampleBuffer.clear();

    for (int i = 0; i < 5; ++i)
        model.sampleBuffer.setSample (0, 10000 + i, 1.0f);

    model.sampleSampleRate = 44100.0;
    model.sampleLoaded = true;
    model.trimStartSample.store (0);
    model.trimEndSample.store (44100);
    model.transientDetector.analyze (model.sampleBuffer, 44100.0);

    const int id = model.addManualSlicePoint (10100, true);
    auto points = model.getManualSlicePoints();
    REQUIRE (points.size() == 1);
    CHECK (points[0].samplePosition == 10000);
}
