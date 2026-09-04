// Nedit -- UI-layer: framework-free view-model for the Sequencer step grid.
//
// Pure C++ (no VSTGUI): procedural sizing, hit-testing, scroll math and the
// bar-span/declared-length computation the grid renders. The VSTGUI
// SequencerGridView control in nedit_plugin is a thin renderer over these;
// keeping the math here makes it unit-testable and lets the editor adjust
// the geometry (we're not "dynamic", but we'll be tweaking sizes often, so
// everything derives from these helpers rather than scattered literals).

#pragma once

#include <state/Types.h>
#include <state/StyleParameters.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace nedit::ui {

// Beat value of a (possibly unsanitized) note-value index; 0.0 if bogus.
// Mirrors the engine's local helper (Scheduler.cpp / SequenceRandomizer.cpp).
[[nodiscard]] inline double noteValueBeats (int index) noexcept
{
    return state::isValidNoteValueIndex (index)
        ? state::kNoteValues[static_cast<std::size_t> (index)].beats
        : 0.0;
}

// Procedural layout of the step grid for a given pixel viewport + grid
// dimensions. Everything is derived from the inputs; nothing is a magic
// number the editor must remember to keep in sync with hit-testing.
struct SequencerGridLayout
{
    int visibleRows = 0;    // how many rows fully fit in the viewport
    int totalRows = 0;      // number of slices (rows) in the grid
    int totalCols = 0;      // number of steps (columns)
    double rowHeight = 0.0; // derived row height in px (>= kMinRowHeight)
    double colWidth = 0.0;  // derived column width in px
    bool scrolls = false;   // whether rows exceed the viewport height
    int maxScroll = 0;      // greatest valid scroll offset (rows): totalRows - visibleRows
};

// Hard floor on a row's height. Below this the grid scrolls rather than
// squeezing slices into unusably thin bands.
inline constexpr double kMinSequencerRowH = 14.0;

// Natural slice length quantized to the step grid (the "natural length" the
// original's piano-roll bars use, shared with the audio path).
//   sliceLengthFrames, sampleSampleRate, originalBpm : source-tempo inputs
//   stepBeats : beats per step (note-value palette beats)
// Returns >= 1 (a degenerate slice still occupies its starting step).
[[nodiscard]] inline int naturalStepsForSlice (std::int64_t sliceLengthFrames,
                                               double sampleSampleRate,
                                               double originalBpm,
                                               double stepBeats) noexcept
{
    if (sliceLengthFrames <= 0 || sampleSampleRate <= 0.0
        || originalBpm <= 0.0 || stepBeats <= 0.0)
        return 1;

    const double sliceSeconds = static_cast<double> (sliceLengthFrames) / sampleSampleRate;
    const double naturalBeats = sliceSeconds * (originalBpm / 60.0);
    return std::max (1, static_cast<int> (std::lround (naturalBeats / stepBeats)));
}

[[nodiscard]] inline SequencerGridLayout computeSequencerLayout (double viewW,
                                                                 double viewH,
                                                                 int totalRows,
                                                                 int totalCols) noexcept
{
    SequencerGridLayout layout;
    layout.totalRows = std::max (0, totalRows);
    layout.totalCols = std::max (1, totalCols);

    if (viewW <= 0.0 || viewH <= 0.0)
        return layout;

    layout.colWidth = viewW / static_cast<double> (layout.totalCols);

    if (layout.totalRows <= 0)
    {
        layout.rowHeight = kMinSequencerRowH;
        return layout;
    }

    // If every row fits at >= the minimum height, spread them to fill the
    // viewport (no scrolling). Otherwise fix the minimum height and scroll.
    const double fill = viewH / static_cast<double> (layout.totalRows);
    if (fill >= kMinSequencerRowH)
    {
        layout.rowHeight = fill;
        layout.visibleRows = layout.totalRows;
        layout.scrolls = false;
    }
    else
    {
        layout.rowHeight = kMinSequencerRowH;
        layout.visibleRows = std::max (1, static_cast<int> (viewH / kMinSequencerRowH));
        layout.scrolls = layout.visibleRows < layout.totalRows;
    }

    layout.maxScroll = std::max (0, layout.totalRows - layout.visibleRows);
    return layout;
}

// Clamp a scroll offset (in rows) into [0, maxScroll].
[[nodiscard]] inline int clampSequencerScroll (const SequencerGridLayout& layout,
                                               int scrollRows) noexcept
{
    return std::clamp (scrollRows, 0, layout.maxScroll);
}

// Column (step) index at pixel x. -1 when outside the grid's x range.
[[nodiscard]] inline int columnFromX (const SequencerGridLayout& layout,
                                      double x, double viewLeft) noexcept
{
    if (layout.totalCols <= 0 || layout.colWidth <= 0.0 || x < viewLeft)
        return -1;
    const double rel = (x - viewLeft) / layout.colWidth;
    const int col = static_cast<int> (rel);
    return (col >= 0 && col < layout.totalCols) ? col : -1;
}

// Slice (row) index at pixel y, for a piano-roll grid (row 0 at the BOTTOM)
// with the given scroll offset (in rows). -1 when outside or no rows.
[[nodiscard]] inline int rowFromBottomY (const SequencerGridLayout& layout,
                                         double y, double viewTop, double viewBottom,
                                         int scrollRows) noexcept
{
    if (layout.totalRows <= 0 || layout.rowHeight <= 0.0 || layout.visibleRows <= 0)
        return -1;

    // Flip: y measured from the top of the viewport, rows grow upward.
    // Pixel y belongs to the band whose bottom edge it sits above:
    // fromBottom = ceil((viewBottom - y) / rowHeight) - 1.
    if (y < viewTop || y >= viewBottom)
        return -1;

    const double fromBottomRaw = (viewBottom - y) / layout.rowHeight;
    const int fromBottom = static_cast<int> (std::ceil (fromBottomRaw)) - 1;
    // The leftover strip above the top visible band (when
    // visibleRows*rowHeight < viewH) maps to the topmost visible row.
    const int clamped = std::clamp (fromBottom, 0, layout.visibleRows - 1);
    int row = scrollRows + clamped;

    return (row >= 0 && row < layout.totalRows) ? row : -1;
}

// ---------------------------------------------------------------------------
// Persistent 2D viewport over the sequencer step grid (issue #2).
//
// The grid's content is (totalRows rows x totalCols cols); the editor sized
// the base cell width/height via computeSequencerLayout(). The user can then
// ZOOM either axis (scale the cell size, H&V together by default) and PAN
// (scroll the content within the viewport) with two overlay scrollbars, a
// wheel (zoom) or a middle-mouse drag (pan). The origin is offset in pixels
// from the top-left of the content; the whole window is tracked as a
// NORMALIZED [0,1] origin so it survives pattern-length changes and session
// reload (whose stale values caused the original editor's SIGSEGV -- see
// UiState's doc comment).
// ---------------------------------------------------------------------------

// Per-axis zoom: 1.0 = the base cell size from computeSequencerLayout().
// zooms both axes together by default; a scrollbar-locked wheel only moves
// its own axis.
// The bounds live in state::Types.h (shared with the state sanitizer);
// only the UI interaction constants stay here.
using state::kMinSequencerZoom;
using state::kMaxSequencerZoom;

inline constexpr double kSequencerZoomPerNotch = 1.15;  // multiplier per wheel notch

// The full scroll/pan state for the grid's viewport. All numbers derive
// from the model + view dims; nothing is a magic literal the view must keep
// in sync.
struct SequencerViewport
{
    double zoomX = 1.0;
    double zoomY = 1.0;
    // Origin as a fraction of the (scrolled) content extent, [0,1]:
    // 0 = fully scrolled to the top/left, 1 = fully scrolled to the
    // bottom/right. Normalized so it is independent of zoom and dims.
    double originX = 0.0;
    double originY = 0.0;
};

// Constrain a zoom to [kMinSequencerZoom, kMaxSequencerZoom].
[[nodiscard]] inline double clampSequencerZoom (double zoom) noexcept
{
    return std::clamp (zoom, kMinSequencerZoom, kMaxSequencerZoom);
}

// The factor that moves `currentZoom` toward `currentZoom * multiplier`,
// clamped so the RESULT stays within [min, max]. Used to work in factors
// (an axis-locked wheel keeps the other axis at factor 1.0).
[[nodiscard]] inline double newZoomFactor (double currentZoom, double multiplier) noexcept
{
    return clampSequencerZoom (currentZoom * multiplier) / std::max (1e-9, currentZoom);
}

// Clamp a normalized origin into [0,1].
[[nodiscard]] inline double clampSequencerOrigin (double origin) noexcept
{
    return std::clamp (origin, 0.0, 1.0);
}

// Floor/ceiling the zoom on both axes and clamp both origins.
[[nodiscard]] inline SequencerViewport clampSequencerViewport (SequencerViewport v) noexcept
{
    v.zoomX = clampSequencerZoom (v.zoomX);
    v.zoomY = clampSequencerZoom (v.zoomY);
    v.originX = clampSequencerOrigin (v.originX);
    v.originY = clampSequencerOrigin (v.originY);
    return v;
}

// The pixel extent of the full (unscrolled) content on one axis, at the
// given zoom. Zero if the layout/grid is degenerate.
[[nodiscard]] inline double sequencerContentExtent (const SequencerGridLayout& layout,
                                                    bool vertical,
                                                    const SequencerViewport& vp) noexcept
{
    if (vertical)
        return layout.rowHeight * static_cast<double> (layout.totalRows) * vp.zoomY;
    return layout.colWidth * static_cast<double> (layout.totalCols) * vp.zoomX;
}

// The logical zoom-OUT floor for an axis: the zoom at which the content
// exactly fills the viewport (every row/column visible, no empty gap). Zooming
// out past this would shrink the content below the view and leave dead space,
// which is never useful -- so it, not the fixed kMinSequencerZoom, is the real
// lower bound. computeSequencerLayout() sizes the base cell so the content
// already fills the view whenever it fits (colWidth = viewW/totalCols; and
// rowHeight = viewH/totalRows in the fit case), so this returns 1.0 for an
// axis that fits at base zoom and < 1.0 only when the min-row-height floor made
// the base content overflow (many rows). Always in [kMinSequencerZoom, 1.0];
// falls back to kMinSequencerZoom for degenerate inputs.
[[nodiscard]] inline double sequencerMinZoom (const SequencerGridLayout& layout,
                                              bool vertical, double viewExtent) noexcept
{
    const SequencerViewport unit;   // zoom 1.0 on both axes
    const double baseContent = sequencerContentExtent (layout, vertical, unit);
    if (baseContent <= 0.0 || viewExtent <= 0.0)
        return kMinSequencerZoom;
    // fit is in (0,1]; keep it within the absolute band as a safety net.
    return std::clamp (viewExtent / baseContent, kMinSequencerZoom, kMaxSequencerZoom);
}

// Effective per-axis pixel sizes of a rendered cell at the current zoom.
// (The base colWidth/rowHeight from computeSequencerLayout() are multiplied
// by the per-axis zoom.)
[[nodiscard]] inline double sequencerCellWidth (const SequencerGridLayout& layout,
                                                const SequencerViewport& vp) noexcept
{
    return std::max (0.0, layout.colWidth * vp.zoomX);
}

[[nodiscard]] inline double sequencerCellHeight (const SequencerGridLayout& layout,
                                                 const SequencerViewport& vp) noexcept
{
    return std::max (0.0, layout.rowHeight * vp.zoomY);
}

// The maximum scrollable pixel offset on an axis: how far the content can be
// scrolled past the origin while still filling the viewport. 0 when the
// content fits (fully zoomed out / few rows).
[[nodiscard]] inline double sequencerMaxScroll (const SequencerGridLayout& layout,
                                                bool vertical, double viewExtent,
                                                const SequencerViewport& vp) noexcept
{
    const double content = sequencerContentExtent (layout, vertical, vp);
    const double limit = std::max (0.0, content - viewExtent);
    return limit;
}

// Convert a normalized [0,1] origin into the equivalent absolute pixel
// offset (see sequencerMaxScroll). Clamped so the content never leaves a
// gap at the far end of the viewport.
[[nodiscard]] inline double sequencerOriginPx (const SequencerGridLayout& layout,
                                               bool vertical, double viewExtent,
                                               const SequencerViewport& vp) noexcept
{
    const double limit = sequencerMaxScroll (layout, vertical, viewExtent, vp);
    const double origin = vertical ? vp.originY : vp.originX;
    return std::clamp (origin * limit, 0.0, std::max (0.0, limit));
}

// Zoom/scroll-aware version of columnFromX (which maps the unzoomed,
// unscrolled grid). Column index at pixel x given the scrolled origin in
// pixels; -1 outside the grid.
[[nodiscard]] inline int sequencerColumnFromX (const SequencerGridLayout& layout,
                                               const SequencerViewport& vp,
                                               double x, double viewLeft,
                                               double originXpx) noexcept
{
    const double cw = sequencerCellWidth (layout, vp);
    if (cw <= 0.0 || x < viewLeft)
        return -1;
    const int col = static_cast<int> ((x - viewLeft + originXpx) / cw);
    return (col >= 0 && col < layout.totalCols) ? col : -1;
}

// Zoom/scroll-aware version of rowFromBottomY. Slice (row) index at pixel y
// for the bottom-up grid given the scrolled origin in pixels; -1 when
// outside the content's row bands or the viewport. Same boundary convention
// as rowFromBottomY: the pixel at a band's top edge belongs to the band
// above it (the lower row).
[[nodiscard]] inline int sequencerRowFromY (const SequencerGridLayout& layout,
                                            const SequencerViewport& vp,
                                            double y, double viewTop, double viewBottom,
                                            double originYpx) noexcept
{
    const double ch = sequencerCellHeight (layout, vp);
    if (ch <= 0.0 || layout.totalRows <= 0 || y < viewTop || y >= viewBottom)
        return -1;
    const double v = (y - viewTop + originYpx) / ch;   // content pixel in cell units
    const int row = static_cast<int> (static_cast<double> (layout.totalRows) - v);
    return (row >= 0 && row < layout.totalRows) ? row : -1;
}

// The pixel x (top of a given column, in grid-content space) of a step
// column, offset by the current origin so callers can place + cull bars.
[[nodiscard]] inline double sequencerColumnX (const SequencerGridLayout& layout,
                                              const SequencerViewport& vp,
                                              double originXpx, int column) noexcept
{
    return static_cast<double> (column) * sequencerCellWidth (layout, vp) - originXpx;
}

// The pixel y of a row band's BOTTOM edge in viewport-local space, under the
// canonical "window over the content" model: the view shows content pixels
// [originPx, originPx + viewExtent], content pixel 0 = content TOP (row
// totalRows-1's upper edge), and the piano-roll rows grow downward (row 0 at
// the content's bottom). originYpx is the scrolled offset in pixels. A row
// whose bottom lies below viewBottom (visible with origin near 0) is simply
// outside the window; callers cull.
[[nodiscard]] inline double sequencerRowBottom (const SequencerGridLayout& layout,
                                                const SequencerViewport& vp,
                                                double viewTop, double originYpx,
                                                int row) noexcept
{
    const double ch = sequencerCellHeight (layout, vp);
    return viewTop + (static_cast<double> (layout.totalRows) - static_cast<double> (row)) * ch
         - originYpx;
}

// One overlay scrollbar's geometry: a track the knob can travel and the
// knob itself, sized to the visible fraction and placed by the normalized
// origin. No scroll needed when the content fits (`scrollable` false).
struct SequencerScrollBar
{
    bool scrollable = false;      // content overflows the viewport on this axis
    double trackStart = 0.0;      // knob leading-edge travel range (px)
    double trackEnd = 0.0;        // ... exclusive end
    double knobStart = 0.0;       // knob leading edge (px), for drawing/hit-test
    double knobEnd = 0.0;         // knob trailing edge (px)
    double visibleFraction = 1.0; // visible / all content on this axis
};

// Minimum grab surface for an overlay knob -- the visible fraction alone
// can be absurdly thin when zoomed out.
inline constexpr double kSequencerScrollBarMinKnob = 12.0;

// Compute one axis's overlay scrollbar geometry. `viewExtent` is the
// viewport size on that axis (width for H, height for V) and `vp` carries
// the per-axis zoom + origin.
[[nodiscard]] inline SequencerScrollBar computeSequencerScrollBar (
    bool vertical, const SequencerGridLayout& layout, double viewExtent,
    const SequencerViewport& vp)
{
    SequencerScrollBar bar;
    const double content = sequencerContentExtent (layout, vertical, vp);
    if (viewExtent <= 0.0 || content <= 0.0)
        return bar;

    const double visible = std::min (1.0, viewExtent / content);
    bar.visibleFraction = visible;
    bar.scrollable = visible < 1.0;
    if (! bar.scrollable)
        return bar;

    const double trackLen = viewExtent;
    bar.trackStart = 0.0;
    bar.trackEnd = trackLen;

    double knobLen = std::clamp (visible * trackLen,
                                 kSequencerScrollBarMinKnob, trackLen);
    const double norm = vertical ? vp.originY : vp.originX;
    const double lead = norm * (trackLen - knobLen);   // classic scrollbar mapping
    bar.knobStart = lead;
    bar.knobEnd = lead + knobLen;
    return bar;
}

// Wheel zoom on one axis (`xFactor`/`yFactor` are independent so an
// overlay-scrollbar-locked wheel can scale a single axis), about the
// cursor position (a [0,1] fraction of the viewport on each axis). The
// content pixel under the cursor stays anchored under it. Returns a
// clamped viewport.
[[nodiscard]] inline SequencerViewport zoomSequencerViewport (
    const SequencerGridLayout& layout, double viewWidth, double viewHeight,
    SequencerViewport vp, double cursorNormX, double cursorNormY,
    double xFactor, double yFactor) noexcept
{
    // Per-axis lower bound is the "fill the viewport" fit, not the fixed floor:
    // zoom-out stops once every row/column is visible (issue: no dead space).
    const double minX = sequencerMinZoom (layout, false, viewWidth);
    const double minY = sequencerMinZoom (layout, true, viewHeight);

    // Clamp the factors so the RESULT lands in [axisMin, max] (an axis-locked
    // wheel keeps the other factor at 1.0).
    xFactor = std::clamp (vp.zoomX * xFactor, minX, kMaxSequencerZoom)
            / std::max (1e-9, vp.zoomX);
    yFactor = std::clamp (vp.zoomY * yFactor, minY, kMaxSequencerZoom)
            / std::max (1e-9, vp.zoomY);

    const double oldCX = sequencerContentExtent (layout, false, vp);
    const double oldCY = sequencerContentExtent (layout, true, vp);
    const double oxPx = sequencerOriginPx (layout, false, viewWidth, vp);
    const double oyPx = sequencerOriginPx (layout, true, viewHeight, vp);

    const double cursorContentX = oxPx + cursorNormX * viewWidth;
    const double cursorContentY = oyPx + cursorNormY * viewHeight;

    const double newCX = std::max (0.0, oldCX * xFactor);
    const double newCY = std::max (0.0, oldCY * yFactor);
    const double newOxPx = cursorContentX * xFactor - cursorNormX * viewWidth;
    const double newOyPx = cursorContentY * yFactor - cursorNormY * viewHeight;

    vp.zoomX = std::clamp (vp.zoomX * xFactor, minX, kMaxSequencerZoom);
    vp.zoomY = std::clamp (vp.zoomY * yFactor, minY, kMaxSequencerZoom);

    const double newMaxX = std::max (0.0, newCX - viewWidth);
    const double newMaxY = std::max (0.0, newCY - viewHeight);
    vp.originX = newMaxX > 0.0 ? clampSequencerOrigin (newOxPx / newMaxX) : 0.0;
    vp.originY = newMaxY > 0.0 ? clampSequencerOrigin (newOyPx / newMaxY) : 0.0;
    return vp;
}

// Pan by a pixel delta on both axes; the origin moves by -deltaPx/maxScroll
// (dragging right/up scrolls content left/down -- standard grab-and-move
// viewport feel). Returns a clamped viewport.
[[nodiscard]] inline SequencerViewport panSequencerViewport (
    const SequencerGridLayout& layout, double viewWidth, double viewHeight,
    SequencerViewport vp, double deltaXPx, double deltaYPx) noexcept
{
    const double maxX = sequencerMaxScroll (layout, false, viewWidth, vp);
    const double maxY = sequencerMaxScroll (layout, true, viewHeight, vp);
    if (maxX > 0.0)
        vp.originX = clampSequencerOrigin (vp.originX - deltaXPx / maxX);
    if (maxY > 0.0)
        vp.originY = clampSequencerOrigin (vp.originY - deltaYPx / maxY);
    return vp;
}

// The scroll/pan zone classification for a point over the grid view;
// returned by sequencerScrollZone() so the view can dispatch the wheel to
// the right axis (issue #2: over a scrollbar, zoom is locked to that bar's
// orientation; over the canvas, zoom applies to both axes).
enum class SequencerScrollZone
{
    canvas,          // over the grid itself: wheel zooms both axes
    horizontalBar,   // over the H overlay scrollbar: wheel zooms X only
    verticalBar      // over the V overlay scrollbar: wheel zooms Y only
};

// Hit-test a point into a scroll zone. The overlay bars live at the bottom
// (H) and right (V) of the view; `hitExtend` widens their invisible grab
// strip beyond the visible `barThickness` (issue #2: "sliders hit test ...
// larger than the visible control").
[[nodiscard]] inline SequencerScrollZone sequencerScrollZone (
    double x, double y, double viewWidth, double viewHeight,
    double barThickness, double hitExtend) noexcept
{
    if (viewWidth <= 0.0 || viewHeight <= 0.0)
        return SequencerScrollZone::canvas;

    // The H bar owns the whole bottom band (including the bottom-right corner
    // its track fills); the V bar owns the right band ABOVE the H bar.
    if (x >= viewWidth - barThickness - hitExtend
        && y < viewHeight - barThickness)
        return SequencerScrollZone::verticalBar;
    if (y >= viewHeight - barThickness - hitExtend)
        return SequencerScrollZone::horizontalBar;
    return SequencerScrollZone::canvas;
}

// Accumulated pixel height of `nRows` rows from the bottom of the viewport.
[[nodiscard]] inline double rowsHeightFromBottom (const SequencerGridLayout& layout,
                                                  int nRows) noexcept
{
    return layout.rowHeight * static_cast<double> (nRows);
}

// A single occupied step cell's rendered bar: [startColumn, endColumn) in
// steps, plus the style ordinal to colour it. Mirrors the engine's
// Sequenced pick: natural length (slice duration quantized to the step
// resolution) raised by the Shift+drag extension, capped at the next
// active column anywhere in the grid (anticipatory fade in the audio).
struct SequencerCellBar
{
    int row = -1;
    int startColumn = -1;
    int endColumn = -1;   // exclusive
    std::int8_t style = -1;
};

template <typename ExtMap>
[[nodiscard]] std::vector<SequencerCellBar>
computeCellBars (int row,
                 std::int8_t style,
                 std::int64_t sliceLengthFrames,
                 double sampleSampleRate,
                 double originalBpm,
                 int stepResolutionIndex,
                 const SequencerGridLayout& layout,
                 const std::vector<std::int8_t>& grid,
                 const ExtMap& extensions) noexcept
{
    std::vector<SequencerCellBar> bars;

    const int rowsEffective = layout.totalRows;

    // Lambda: is `column` occupied by any row (used to find the next active
    // column that cuts this note off -- anticipatory fade in the audio)?
    const auto columnActive = [&] (int column) noexcept {
        if (column >= layout.totalCols)
            return false;
        for (int r = 0; r < rowsEffective; ++r)
        {
            const auto cc = static_cast<std::uint32_t> (r)
                          * static_cast<std::uint32_t> (layout.totalCols)
                          + static_cast<std::uint32_t> (column);
            if (cc < grid.size() && grid[cc] >= 0)
                return true;
        }
        return false;
    };

    for (int col = 0; col < layout.totalCols; ++col)
    {
        const auto cell = static_cast<std::uint32_t> (row) * static_cast<std::uint32_t> (layout.totalCols)
                        + static_cast<std::uint32_t> (col);
        if (cell >= grid.size() || grid[cell] < 0 || static_cast<int> (grid[cell]) != static_cast<int> (style))
            continue;

        const double stepBeats = noteValueBeats (stepResolutionIndex);
        int declared = naturalStepsForSlice (sliceLengthFrames, sampleSampleRate, originalBpm, stepBeats);

        if (const auto it = extensions.find (cell); it != extensions.end())
            declared = std::max (declared, static_cast<int> (it->second));

        // The bar runs `declared` steps forward but is cut short at the next
        // active column anywhere in the grid (the engine's anticipatory fade).
        int end = std::min (layout.totalCols, col + declared);
        for (int c = col + 1; c < end; ++c)
        {
            if (columnActive (c))
            {
                end = c;
                break;
            }
        }

        bars.push_back ({ row, col, end, style });
    }

    return bars;
}

// ---------------------------------------------------------------------------
// Per-cell override menu classification (right-click on an active step),
// mirroring the original's getApplicableSequencerCellParameters() split:
//   * an applicable param whose flags say "discrete and NOT a stepped
//     slider" presents as a SUBMENU of its option names -- picking one
//     writes the override directly (no in-place slider)
//   * a SWEPT param (has a paired *Mode sibling = id+1) presents as a
//     submenu of its MODE choices first; picking a mode writes the mode
//     override AND opens the in-place slider for the VALUE id
//   * anything else -- a continuous param, or Subdivide (discrete but
//     stepped) -- opens the in-place drag slider
// The UI (VSTGUI SequencerGridView) renders these; this struct is the
// framework-free, testable spec of "how to present each entry".
// ---------------------------------------------------------------------------

enum class CellOverrideMenuKind : std::uint8_t
{
    submenu,        // discrete, non-stepped: top-level submenu of the param's own options
    modeSubmenu,    // swept: submenu of the paired *Mode choices, then open the slider
    slider          // continuous OR Subdivide (stepped): plain entry that opens the slider
};

// One top-level entry of the right-click override menu.
struct CellOverrideMenuEntry
{
    state::StyleParamId id;              // the VALUE param (the one stored)
    CellOverrideMenuKind kind;
    // For swept params, the paired mode id (id+1 in the vocabulary); the
    // mode's options become the submenu. Meaningful only for modeSubmenu.
    state::StyleParamId modeId = state::StyleParamId::filterResonance;
};

// The menu entries for a cell's style, derived from applicableStyleParams()
// (which already appends Subdivide + Volume for every style including
// Forward). Non-applicable / empty entries are dropped from the UI.
[[nodiscard]] inline std::vector<CellOverrideMenuEntry>
cellOverrideMenuEntries (state::PlaybackStyle style) noexcept
{
    std::vector<CellOverrideMenuEntry> out;

    const auto applicable = state::applicableStyleParams (style);
    for (int i = 0; i < applicable.count; ++i)
    {
        const auto id = applicable.ids[static_cast<std::size_t> (i)];
        const auto& info = state::styleParamInfo (id);

        CellOverrideMenuEntry e;
        e.id = id;
        if (info.swept)
        {
            // Swept param (Sample Rate Reduction, Bit Depth, Delay, Mix,
            // Feedback, Volume): submenu of the paired *Mode choices first;
            // picking one then opens the value slider.
            e.kind = CellOverrideMenuKind::modeSubmenu;
            e.modeId = static_cast<state::StyleParamId> (static_cast<int> (id) + 1);
        }
        else if (info.discrete && ! info.steppedSlider)
        {
            // Discrete, non-stepped (Filter Type, Curve Shape, the curve/
            // mode dropdowns): submenu of the param's own option names.
            e.kind = CellOverrideMenuKind::submenu;
        }
        else
        {
            // Continuous param, or Subdivide (discrete + stepped slider):
            // a plain entry that opens the in-place drag slider.
            e.kind = CellOverrideMenuKind::slider;
        }

        out.push_back (e);
    }

    return out;
}

} // namespace nedit::ui
