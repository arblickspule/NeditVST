// Nedit -- UI layer.
//
// Pure geometry/math behind the future waveform view: peak columns,
// slice-marker and playhead pixel positions, zoom/pan manipulation.
//
// This layer exists so the VSTGUI views can be dumb shells over fully
// tested logic (AGENTS rule: "views are stateless renderers of
// PluginState"). No framework includes here -- inputs are raw channel
// pointers and state-layer types only.

#pragma once

#include <engine/Slice.h>
#include <state/UiState.h>

#include <cstdint>
#include <vector>

namespace nedit::ui {

struct WaveformColumn
{
    float min = 0.0f;
    float max = 0.0f;
};

// Peak columns for one drawn frame of the waveform view. Buckets the
// samples of every channel (max magnitude wins per column) across the
// visible window derived from `ui`, confined to [rangeStart, rangeEnd).
// Columns whose bucket holds no samples (extreme zoom-in) repeat the
// nearest sampled value rather than dropping to zero.
[[nodiscard]] std::vector<WaveformColumn> computeWaveformPeaks (
    const float* const* channels, int numChannels, std::int64_t frames,
    std::int64_t rangeStart, std::int64_t rangeEnd, const state::UiState& ui,
    int numColumns);

// Pixel x-positions (in [0, width]) of slice boundaries that fall inside
// the visible window. One entry per boundary; boundaries outside the
// window are skipped entirely.
[[nodiscard]] std::vector<double> computeSliceMarkerX (
    const std::vector<engine::Slice>& slices, std::int64_t rangeStart,
    std::int64_t rangeEnd, const state::UiState& ui, double width);

// Pixel x of an arbitrary frame position (e.g. the playhead); not
// clamped -- callers decide whether off-screen values mean "hidden".
[[nodiscard]] double frameToX (std::int64_t frame, std::int64_t rangeStart,
                               std::int64_t rangeEnd, const state::UiState& ui,
                               double width);

// Inverse of frameToX for click handling; -1 when `x` lies outside the
// widget.
[[nodiscard]] std::int64_t xToFrame (double x, std::int64_t rangeStart,
                                     std::int64_t rangeEnd, const state::UiState& ui,
                                     double width);

// Zoom around an anchor point (the frame under the mouse, as a fraction
// of the trimmed range): scaleFactor > 1 zooms out. Keeps the anchor
// visually stationary. Result respects kMinVisibleSpanNorm and stays
// inside [0,1]; written back into ui by the caller's edit path.
struct VisibleWindow
{
    double start = 0.0;
    double end = 1.0;
};

inline constexpr double kMinVisibleSpanNorm = 1.0 / 1024.0;

[[nodiscard]] VisibleWindow zoomedWindow (const state::UiState& ui,
                                          double anchorNorm, double scaleFactor);

// Pan by a fraction of the currently visible span (positive moves right).
[[nodiscard]] VisibleWindow pannedWindow (const state::UiState& ui, double deltaSpan);

} // namespace nedit::ui
