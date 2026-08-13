#include <doctest/doctest.h>
#include "SlicerModel.h"

// Harness smoke test: construct the model and exercise a couple of simple
// get/set round-trips, proving the test target compiles and links the real
// engine/model sources against JUCE (no plugin client, no GUI) and runs under
// ctest. Real coverage lands in sibling files as the suite grows.
TEST_CASE ("SlicerModel default state")
{
    SlicerModel model;

    CHECK (model.getLoopLengthBars() == 1);
    CHECK (model.getTrimStartSample() == 0);
    CHECK (model.getTrimEndSample() == 0);
    CHECK_FALSE (model.getAuditionActive());
    CHECK (model.getManualSlicePoints().empty());
    CHECK_FALSE (model.canUndoEdit());
    CHECK_FALSE (model.canRedoEdit());
}

TEST_CASE ("SlicerModel loop-length set/get and clamping")
{
    SlicerModel model;

    model.setLoopLengthBars (4);
    CHECK (model.getLoopLengthBars() == 4);

    model.setLoopLengthBars (8);
    CHECK (model.getLoopLengthBars() == 8);

    // jmax (1, bars) -- anything below 1 collapses to 1.
    model.setLoopLengthBars (0);
    CHECK (model.getLoopLengthBars() == 1);
}
