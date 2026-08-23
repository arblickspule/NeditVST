// Nedit -- Engine layer.
//
// Easing-curve evaluation (Scratch v2): shape a linear 0..1 TIME progress
// into a 0..1 DISTANCE progress. The derivative of each shape (w.r.t.
// progress) is what gives it its velocity character:
//
//   linear        -- constant speed throughout
//   easeIn        -- speed rises monotonically from 0, peaking at the end
//                    (a "smoother pull")
//   easeOut       -- speed starts at maximum and falls to 0 by the end
//                    (a "sharp flick")
//   easeInEaseOut -- smoothstep; slow-fast-slow, the speed hump a real
//                    scratch stroke traces
//
// Deliberately simple closed-form quadratic/cubic shapes, cheap enough to
// evaluate every host sample -- not a general animation-curve system.
// The enum itself lives in state/Types.h (it is serialized state).

#pragma once

#include <state/Types.h>

namespace nedit::engine {

[[nodiscard]] inline double applyEasingCurve (double progress,
                                              state::EasingCurve curve) noexcept
{
    progress = progress < 0.0 ? 0.0 : (progress > 1.0 ? 1.0 : progress);

    switch (curve)
    {
        case state::EasingCurve::easeIn:
            return progress * progress;

        case state::EasingCurve::easeOut:
            return progress * (2.0 - progress);  // == 1 - (1 - progress)^2

        case state::EasingCurve::easeInEaseOut:
            return progress * progress * (3.0 - 2.0 * progress);  // smoothstep

        case state::EasingCurve::linear:
            break;
    }

    return progress;
}

// The older, simpler two-shape progress remap used by Tape Stop's decel
// and Ping-Pong's turnaround fade (deliberately independent of the
// four-curve Scratch system above, same as the original): Linear is the
// identity; Exponential eases in (t*t -- slow start, fast finish), which
// reads as a surge-before-stopping for Tape Stop and a snappier
// turnaround for Ping-Pong.
[[nodiscard]] inline double applyCurveShape (double progress,
                                             state::CurveShape shape) noexcept
{
    if (shape == state::CurveShape::exponential)
        return progress * progress;

    return progress;
}

} // namespace nedit::engine
