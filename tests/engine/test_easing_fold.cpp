// Easing curves and the shared foldPosition mapper.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <engine/Fold.h>

using namespace nedit::engine;
using nedit::state::EasingCurve;
using Catch::Matchers::WithinAbs;

TEST_CASE ("easing endpoints are fixed for every curve", "[easing]")
{
    for (const auto curve : { EasingCurve::linear, EasingCurve::easeIn,
                              EasingCurve::easeOut, EasingCurve::easeInEaseOut })
    {
        CHECK_THAT (applyEasingCurve (0.0, curve), WithinAbs (0.0, 1e-12));
        CHECK_THAT (applyEasingCurve (1.0, curve), WithinAbs (1.0, 1e-12));
    }
}

TEST_CASE ("easing midpoint characters", "[easing]")
{
    CHECK_THAT (applyEasingCurve (0.5, EasingCurve::linear), WithinAbs (0.5, 1e-12));
    CHECK_THAT (applyEasingCurve (0.5, EasingCurve::easeIn), WithinAbs (0.25, 1e-12));
    CHECK_THAT (applyEasingCurve (0.5, EasingCurve::easeOut), WithinAbs (0.75, 1e-12));
    CHECK_THAT (applyEasingCurve (0.5, EasingCurve::easeInEaseOut), WithinAbs (0.5, 1e-12));
}

TEST_CASE ("easing is monotonic and clamped", "[easing]")
{
    for (const auto curve : { EasingCurve::linear, EasingCurve::easeIn,
                              EasingCurve::easeOut, EasingCurve::easeInEaseOut })
    {
        double previous = -1.0;

        for (int i = 0; i <= 100; ++i)
        {
            const double shaped = applyEasingCurve (i / 100.0, curve);
            CHECK (shaped >= previous);
            previous = shaped;
        }

        // Out-of-range input clamps, never extrapolates.
        CHECK (applyEasingCurve (-5.0, curve) == 0.0);
        CHECK (applyEasingCurve (5.0, curve) == 1.0);
    }
}

TEST_CASE ("foldPosition forward is the identity", "[fold]")
{
    CHECK (foldPosition (0.0, 100.0, FoldStyle::forward) == 0.0);
    CHECK (foldPosition (250.0, 100.0, FoldStyle::forward) == 250.0);
    CHECK (foldPosition (1e9, 100.0, FoldStyle::forward) == 1e9);
}

TEST_CASE ("foldPosition degenerate slice length is the identity", "[fold]")
{
    CHECK (foldPosition (42.0, 0.0, FoldStyle::pingPong) == 42.0);
    CHECK (foldPosition (42.0, -1.0, FoldStyle::loop) == 42.0);
}

TEST_CASE ("foldPosition pingPong is a triangle over 2*length", "[fold]")
{
    const double length = 100.0;

    // Forward leg: identity.
    CHECK_THAT (foldPosition (0.0, length, FoldStyle::pingPong), WithinAbs (0.0, 1e-12));
    CHECK_THAT (foldPosition (50.0, length, FoldStyle::pingPong), WithinAbs (50.0, 1e-12));
    CHECK_THAT (foldPosition (99.0, length, FoldStyle::pingPong), WithinAbs (99.0, 1e-12));

    // Backward leg: counting back down.
    CHECK_THAT (foldPosition (100.0, length, FoldStyle::pingPong), WithinAbs (100.0, 1e-12));
    CHECK_THAT (foldPosition (150.0, length, FoldStyle::pingPong), WithinAbs (50.0, 1e-12));
    CHECK_THAT (foldPosition (199.0, length, FoldStyle::pingPong), WithinAbs (1.0, 1e-12));

    // Period 2L: repeats.
    CHECK_THAT (foldPosition (200.0, length, FoldStyle::pingPong), WithinAbs (0.0, 1e-12));
    CHECK_THAT (foldPosition (250.0, length, FoldStyle::pingPong), WithinAbs (50.0, 1e-12));
    CHECK_THAT (foldPosition (350.0, length, FoldStyle::pingPong), WithinAbs (50.0, 1e-12));

    // Output always stays within [0, length].
    for (int i = 0; i < 1000; ++i)
    {
        const double folded = foldPosition (i * 7.3, length, FoldStyle::pingPong);
        CHECK (folded >= 0.0);
        CHECK (folded <= length);
    }
}

TEST_CASE ("foldPosition loop is a plain modulo", "[fold]")
{
    const double length = 100.0;

    CHECK_THAT (foldPosition (0.0, length, FoldStyle::loop), WithinAbs (0.0, 1e-12));
    CHECK_THAT (foldPosition (99.0, length, FoldStyle::loop), WithinAbs (99.0, 1e-12));
    CHECK_THAT (foldPosition (100.0, length, FoldStyle::loop), WithinAbs (0.0, 1e-12));
    CHECK_THAT (foldPosition (250.0, length, FoldStyle::loop), WithinAbs (50.0, 1e-12));

    // Defensive negative handling.
    CHECK_THAT (foldPosition (-30.0, length, FoldStyle::loop), WithinAbs (70.0, 1e-12));
}

TEST_CASE ("pingPong easing warps within the leg", "[fold]")
{
    const double length = 100.0;

    // easeIn forward leg: halfway through the leg's TIME = a quarter of
    // its DISTANCE.
    CHECK_THAT (foldPosition (50.0, length, FoldStyle::pingPong,
                              EasingCurve::easeIn, EasingCurve::linear),
                WithinAbs (25.0, 1e-12));

    // Backward leg uses the backward curve: halfway back at easeIn
    // shaped 0.25 -> position length - 25.
    CHECK_THAT (foldPosition (150.0, length, FoldStyle::pingPong,
                              EasingCurve::linear, EasingCurve::easeIn),
                WithinAbs (75.0, 1e-12));

    // Leg endpoints are unaffected by any curve (easing is endpoint-fixed).
    CHECK_THAT (foldPosition (100.0, length, FoldStyle::pingPong,
                              EasingCurve::easeInEaseOut, EasingCurve::easeOut),
                WithinAbs (100.0, 1e-12));
    CHECK_THAT (foldPosition (200.0, length, FoldStyle::pingPong,
                              EasingCurve::easeIn, EasingCurve::easeIn),
                WithinAbs (0.0, 1e-12));
}

TEST_CASE ("linear/linear shaped path equals the plain triangle", "[fold]")
{
    // The implementation keeps a separate unshaped branch; verify the
    // shaped formula would agree anyway at representative points (the
    // original guaranteed bit-identical output for non-Scratch callers).
    const double length = 137.0;

    for (int i = 0; i < 500; ++i)
    {
        const double elapsed = i * 3.7;
        CHECK_THAT (foldPosition (elapsed, length, FoldStyle::pingPong,
                                  EasingCurve::linear, EasingCurve::linear),
                    WithinAbs (foldPosition (elapsed, length, FoldStyle::pingPong), 1e-12));
    }
}
