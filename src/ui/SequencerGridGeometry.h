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
