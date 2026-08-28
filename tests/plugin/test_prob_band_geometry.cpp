// Paint-overlay geometry for the style-probability band (see
// ProbBandGeometry.h): the mapping that turns a frame-space drag into a
// style column + weight must match what the vertical bars use.

#include "plugin/ProbBandGeometry.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace nedit::plugin::ui {

TEST_CASE ("prob band geometry: column from frame x clamps to the band")
{
    constexpr double bandLeft = 36.0;
    constexpr double colW = 98.7;

    CHECK (probColumnFromX (36.0, bandLeft, colW) == 0);
    CHECK (probColumnFromX (36.0 + colW - 0.5, bandLeft, colW) == 0);
    CHECK (probColumnFromX (36.0 + colW + 1.0, bandLeft, colW) == 1);
    CHECK (probColumnFromX (36.0 + 8.0 * colW + 40.0, bandLeft, colW) == 8);

    // Drags that leave the band left/right paint the edge columns.
    CHECK (probColumnFromX (-50.0, bandLeft, colW) == 0);
    CHECK (probColumnFromX (1000.0, bandLeft, colW) == state::kNumPlaybackStyles - 1);

    // Degenerate column width cannot divide by zero.
    CHECK (probColumnFromX (100.0, bandLeft, 0.0) == 0);
}

TEST_CASE ("prob band geometry: value from y clamps like the vertical bars")
{
    constexpr double top = 36.0;
    constexpr double bottom = 240.0;

    CHECK (probValueFromY (36.0, top, bottom) == Catch::Approx (1.0f));
    CHECK (probValueFromY (138.0, top, bottom) == Catch::Approx (0.5f));
    CHECK (probValueFromY (240.0, top, bottom) == Catch::Approx (0.0f));

    // Outside the band: clamp, never overshoot.
    CHECK (probValueFromY (0.0, top, bottom) == Catch::Approx (1.0f));
    CHECK (probValueFromY (500.0, top, bottom) == Catch::Approx (0.0f));

    // Degenerate band height (nothing to drag against).
    CHECK (probValueFromY (100.0, 50.0, 50.0) == Catch::Approx (0.0f));
}

} // namespace nedit::plugin::ui