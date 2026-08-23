// Nedit -- Engine layer.
//
// foldPosition: the shared position mapper used identically by BOTH pitch
// modes' render paths -- the granular stretcher's grain-start scheduling
// and the direct-read (Repitch) path. Folds an unbounded "elapsed source
// samples since pick start" into the slice:
//
//   forward  -- identity (unbounded march)
//   pingPong -- triangle fold over a 2*length period: counting up for the
//               first length (forward leg), back down for the next
//               (backward leg). Used by Ping-Pong (slice-derived cycle)
//               and Scratch (Rate-derived cycle).
//   loop     -- plain modulo wraparound: always counting forward,
//               restarting at 0 every length. Stretch's step-extension
//               fill ("repeat the same stretched pass").
//
// forwardCurve/backwardCurve (Scratch v2): optional per-leg easing that
// re-warps the within-leg position. Linear/Linear reproduces the plain
// triangle math exactly and is kept as a genuinely separate branch so
// existing callers stay bit-for-bit identical (same guarantee the
// original made).

#pragma once

#include "Easing.h"

#include <cmath>

namespace nedit::engine {

enum class FoldStyle
{
    forward,
    pingPong,
    loop
};

[[nodiscard]] inline double foldPosition (double elapsedSourceSamples, double sliceLength,
                                          FoldStyle style,
                                          state::EasingCurve forwardCurve = state::EasingCurve::linear,
                                          state::EasingCurve backwardCurve = state::EasingCurve::linear) noexcept
{
    if (style == FoldStyle::forward || sliceLength <= 0.0)
        return elapsedSourceSamples;

    if (style == FoldStyle::loop)
    {
        double cycle = std::fmod (elapsedSourceSamples, sliceLength);

        if (cycle < 0.0)  // defensive -- elapsed should never go negative
            cycle += sliceLength;

        return cycle;
    }

    // pingPong
    const double period = 2.0 * sliceLength;
    double cycle = std::fmod (elapsedSourceSamples, period);

    if (cycle < 0.0)  // defensive
        cycle += period;

    const bool isForwardLeg = (cycle < sliceLength);

    // Curve shaping: a separate branch from the plain linear fold below
    // (not "always run the shaped formula") so every non-Scratch caller's
    // output stays bit-for-bit identical to the unshaped math.
    if (forwardCurve != state::EasingCurve::linear
        || backwardCurve != state::EasingCurve::linear)
    {
        const double legProgress = isForwardLeg ? (cycle / sliceLength)
                                                : ((cycle - sliceLength) / sliceLength);
        const double shaped = applyEasingCurve (legProgress,
                                                isForwardLeg ? forwardCurve : backwardCurve);

        return isForwardLeg ? (shaped * sliceLength)
                            : (sliceLength - shaped * sliceLength);
    }

    return isForwardLeg ? cycle : (period - cycle);
}

} // namespace nedit::engine
