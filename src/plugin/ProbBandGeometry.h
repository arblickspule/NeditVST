// Nedit -- Plugin layer.
//
// Pure geometry for the style-probability band and its paint-overlay
// gesture: mapping a frame-space pointer back into a probability column /
// weight value. Kept SDK-free (no VSTGUI) so the plugin tests can cover
// the mapping without instantiating editor controls.
//
// Must stay in lockstep with StyleProbSlider's drawing maths (a paint
// drag anywhere in a column sets exactly the value the vertical slider
// would for the same pointer height).

#pragma once

#include "state/Types.h"

#include <algorithm>

namespace nedit::plugin::ui {

// 0-based probability column under an x that is `colW` wide per column,
// the first column starting at `bandLeft` (frame-space). Clamps to the
// band's extent -- a paint drag may leave it left/right and still paints
// the edge columns.
[[nodiscard]] inline int probColumnFromX (double x, double bandLeft, double colW) noexcept
{
    if (colW <= 0.0)
        return 0;
    const int c = static_cast<int> ((x - bandLeft) / colW);
    return std::clamp (c, 0, state::kNumPlaybackStyles - 1);
}

// Weight for a pointer `y` within the slider band spanning [top, bottom]
// (same orientation as the vertical bars: top = 1.0, bottom = 0.0), as a
// normalized value clamped to [0,1].
[[nodiscard]] inline float probValueFromY (double y, double top, double bottom) noexcept
{
    const double span = bottom - top;
    double v = span > 0.0 ? 1.0 - (y - top) / span : 0.0;
    v = std::clamp (v, 0.0, 1.0);
    return static_cast<float> (v);
}

} // namespace nedit::plugin::ui