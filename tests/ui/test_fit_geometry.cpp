// Tests for ui::computeEditorFit -- the host-size-fit helper behind the
// editor's CFrame transform (fixed 960x800 design, arbitrary host window).

#include <ui/FitGeometry.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace {

constexpr double kDesignW = 960.0;
constexpr double kDesignH = 800.0;

} // namespace

TEST_CASE ("editor fit: exact design size is identity", "[ui][fit]")
{
    const auto fit = nedit::ui::computeEditorFit (kDesignW, kDesignH, kDesignW, kDesignH);
    CHECK (fit.scale == Catch::Approx (1.0));
    CHECK (fit.offsetX == Catch::Approx (0.0));
    CHECK (fit.offsetY == Catch::Approx (0.0));
}

TEST_CASE ("editor fit: smaller window scales down, keeps everything visible",
           "[ui][fit]")
{
    // Half the height: the whole plan shrinks to 0.5x and is centred.
    const auto fit = nedit::ui::computeEditorFit (960.0, 400.0, kDesignW, kDesignH);
    CHECK (fit.scale == Catch::Approx (0.5));
    CHECK (fit.offsetX == Catch::Approx (240.0));
    CHECK (fit.offsetY == Catch::Approx (0.0));

    // Scaled design fits the window exactly on both axes.
    CHECK (960.0 * fit.scale + 2.0 * fit.offsetX == Catch::Approx (960.0));
    CHECK (800.0 * fit.scale + 2.0 * fit.offsetY == Catch::Approx (400.0));
}

TEST_CASE ("editor fit: smaller window scales by the binding axis", "[ui][fit]")
{
    // Window narrower than the design: bound on width.
    const auto fit = nedit::ui::computeEditorFit (480.0, 800.0, kDesignW, kDesignH);
    CHECK (fit.scale == Catch::Approx (0.5));
    CHECK (fit.offsetX == Catch::Approx (0.0));
    CHECK (fit.offsetY == Catch::Approx (200.0));
}

TEST_CASE ("editor fit: larger window keeps native size, centres it", "[ui][fit]")
{
    // 2x both axes (e.g. a Retina host negotiating point-vs-pixel sizes):
    // native resolution, centred, margins around.
    const auto fit = nedit::ui::computeEditorFit (1920.0, 1600.0, kDesignW, kDesignH);
    CHECK (fit.scale == Catch::Approx (1.0));
    CHECK (fit.offsetX == Catch::Approx (480.0));
    CHECK (fit.offsetY == Catch::Approx (400.0));
}

TEST_CASE ("editor fit: wider-but-not-taller window keeps native scale",
           "[ui][fit]")
{
    // Width bigger, height exactly the design: no upscale, centred x.
    const auto fit = nedit::ui::computeEditorFit (1200.0, 800.0, kDesignW, kDesignH);
    CHECK (fit.scale == Catch::Approx (1.0));
    CHECK (fit.offsetX == Catch::Approx (120.0));
    CHECK (fit.offsetY == Catch::Approx (0.0));
}

TEST_CASE ("editor fit: min-scale clamp never overflows the window", "[ui][fit]")
{
    // Tiny window: the pure scale 0.05 is below the clamp, but the clamped
    // content must stay within the viewport bounds (offets pinned to 0).
    const auto fit = nedit::ui::computeEditorFit (100.0, 50.0, kDesignW, kDesignH, 0.2);
    CHECK (fit.scale == Catch::Approx (0.2));
    CHECK (fit.offsetX == Catch::Approx (0.0));
    CHECK (fit.offsetY == Catch::Approx (0.0));
    // content width (192) exceeds the 100px window -- clamp engaged, but
    // reported offsets stay non-negative so nothing is off-screen left/top.
    REQUIRE (fit.offsetX >= 0.0);
    REQUIRE (fit.offsetY >= 0.0);
}

TEST_CASE ("editor fit: degenerate inputs are identity", "[ui][fit]")
{
    const auto zero = nedit::ui::computeEditorFit (0.0, 800.0, kDesignW, kDesignH);
    CHECK (zero.scale == Catch::Approx (1.0));
    CHECK (zero.offsetX == Catch::Approx (0.0));
    CHECK (zero.offsetY == Catch::Approx (0.0));

    const auto neg = nedit::ui::computeEditorFit (960.0, -400.0, kDesignW, kDesignH);
    CHECK (neg.scale == Catch::Approx (1.0));
    CHECK (neg.offsetX == Catch::Approx (0.0));

    const auto badDesign = nedit::ui::computeEditorFit (960.0, 800.0, 0.0, 800.0);
    CHECK (badDesign.scale == Catch::Approx (1.0));
    CHECK (badDesign.offsetY == Catch::Approx (0.0));
}