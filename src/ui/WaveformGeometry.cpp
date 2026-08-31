// Nedit -- UI layer. See WaveformGeometry.h.

#include "WaveformGeometry.h"

#include <algorithm>
#include <cmath>

namespace nedit::ui {
namespace {

struct VisibleFrames
{
    std::int64_t start = 0;
    std::int64_t end = 0;
};

[[nodiscard]] VisibleFrames visibleFrames (std::int64_t rangeStart, std::int64_t rangeEnd,
                                           const state::UiState& ui) noexcept
{
    const auto span = std::max<std::int64_t> (1, rangeEnd - rangeStart);
    VisibleFrames v;
    v.start = rangeStart
            + static_cast<std::int64_t> (ui.visibleStartNorm * static_cast<double> (span));
    v.end = rangeStart
          + static_cast<std::int64_t> (ui.visibleEndNorm * static_cast<double> (span));
    if (v.end <= v.start)
        v.end = v.start + 1;
    return v;
}

} // namespace

std::vector<WaveformColumn> computeWaveformPeaks (
    const float* const* channels, int numChannels, std::int64_t frames,
    std::int64_t rangeStart, std::int64_t rangeEnd, const state::UiState& ui,
    int numColumns)
{
    if (numColumns <= 0 || channels == nullptr || numChannels <= 0 || frames <= 0
        || rangeEnd - rangeStart <= 0)
        return {};

    std::vector<WaveformColumn> columns (static_cast<std::size_t> (numColumns));

    const auto visible = visibleFrames (rangeStart, rangeEnd, ui);
    const auto windowFrames = std::min<std::int64_t> (frames, visible.end)
                            - std::min<std::int64_t> (frames, visible.start);
    if (windowFrames <= 0)
        return columns;   // trimmed out entirely: flat zero columns

    for (int c = 0; c < numColumns; ++c)
    {
        const double t0 = static_cast<double> (c) / static_cast<double> (numColumns);
        const double t1 = static_cast<double> (c + 1) / static_cast<double> (numColumns);

        auto first = visible.start
                   + static_cast<std::int64_t> (t0 * static_cast<double> (windowFrames));
        auto last = visible.start
                  + static_cast<std::int64_t> (t1 * static_cast<double> (windowFrames));
        first = std::clamp<std::int64_t> (first, visible.start, visible.end);
        last = std::clamp<std::int64_t> (last, first, visible.end);

        // Seed from actual data so all-positive/negative signals get true
        // extrema (never silently clamped to zero).
        float lo = 0.0f;
        float hi = 0.0f;
        bool seeded = false;

        if (last > first)
        {
            for (int chIdx = 0; chIdx < numChannels && ! seeded; ++chIdx)
            {
                const float* data = channels[chIdx];
                for (auto f = first; f < last && ! seeded; ++f)
                {
                    const float s = data[static_cast<std::size_t> (f)];
                    lo = hi = s;
                    seeded = true;
                }
            }
            for (int chIdx = 0; chIdx < numChannels; ++chIdx)
            {
                const float* data = channels[chIdx];
                for (auto f = first; f < last; ++f)
                {
                    const float s = data[static_cast<std::size_t> (f)];
                    lo = std::min (lo, s);
                    hi = std::max (hi, s);
                }
            }
        }
        else if (first < frames && numChannels > 0)
        {
            // Extreme zoom-in: bucket between pixels. Nearest sample wins.
            const float s = channels[0][static_cast<std::size_t> (first)];
            lo = hi = s;
        }

        columns[static_cast<std::size_t> (c)] = { lo, hi };
    }

    return columns;
}

// Pixel x-positions of slice boundaries that fall inside the visible window.
// `boundaries` are absolute frame positions of each slice start/end; adjacent
// slices share a boundary, so it is emitted once.
std::vector<double> computeSliceMarkerX (
    const std::vector<std::int64_t>& boundaries, std::int64_t rangeStart,
    std::int64_t rangeEnd, const state::UiState& ui, double width)
{
    std::vector<double> result;
    if (width <= 0.0 || rangeEnd - rangeStart <= 0)
        return result;

    const auto visible = visibleFrames (rangeStart, rangeEnd, ui);
    result.reserve (boundaries.size() + 1);

    std::vector<std::int64_t> emitted;

    for (const std::int64_t boundary : boundaries)
    {
        if (boundary < visible.start || boundary > visible.end)
            continue;
        if (std::find (emitted.begin(), emitted.end(), boundary) != emitted.end())
            continue;

        emitted.push_back (boundary);
        const double t = static_cast<double> (boundary - visible.start)
                       / static_cast<double> (visible.end - visible.start);
        result.push_back (t * width);
    }
    return result;
}

double frameToX (std::int64_t frame, std::int64_t rangeStart, std::int64_t rangeEnd,
                 const state::UiState& ui, double width)
{
    if (width <= 0.0 || rangeEnd - rangeStart <= 0)
        return 0.0;

    const auto visible = visibleFrames (rangeStart, rangeEnd, ui);
    const double t = static_cast<double> (frame - visible.start)
                   / static_cast<double> (visible.end - visible.start);
    return t * width;
}

std::int64_t xToFrame (double x, std::int64_t rangeStart, std::int64_t rangeEnd,
                       const state::UiState& ui, double width)
{
    if (width <= 0.0 || x < 0.0 || x > width || rangeEnd - rangeStart <= 0)
        return -1;

    const auto visible = visibleFrames (rangeStart, rangeEnd, ui);
    const double t = x / width;
    return visible.start
         + static_cast<std::int64_t> (
             t * static_cast<double> (visible.end - visible.start));
}

VisibleWindow zoomedWindow (const state::UiState& ui, double anchorNorm,
                            double scaleFactor)
{
    VisibleWindow w { ui.visibleStartNorm, ui.visibleEndNorm };

    if (! std::isfinite (anchorNorm) || ! std::isfinite (scaleFactor)
        || scaleFactor <= 0.0)
        return w;

    const double span = w.end - w.start;
    const double newSpan =
        std::clamp (span / scaleFactor, kMinVisibleSpanNorm, 1.0);

    // Keep the anchor stationary in normalized coordinates.
    const double anchorClamped = std::clamp (anchorNorm, w.start, w.end);
    const double anchorT = span > 0.0 ? (anchorClamped - w.start) / span : 0.5;

    double newStart = anchorClamped - anchorT * newSpan;
    newStart = std::clamp (newStart, 0.0, 1.0 - newSpan);

    w.start = newStart;
    w.end = newStart + newSpan;
    return w;
}

VisibleWindow pannedWindow (const state::UiState& ui, double deltaSpan)
{
    VisibleWindow w { ui.visibleStartNorm, ui.visibleEndNorm };
    const double span = w.end - w.start;

    if (! std::isfinite (deltaSpan))
        return w;

    double newStart = w.start + deltaSpan;
    newStart = std::clamp (newStart, 0.0, 1.0 - span);

    w.start = newStart;
    w.end = newStart + span;
    return w;
}

} // namespace nedit::ui
