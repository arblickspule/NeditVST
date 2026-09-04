// Nedit -- UI-layer tests: Sequencer step-grid view-model geometry.
// Procedural sizing, scroll/hit-testing, piano-roll row mapping and the
// bar-span / declared-length math the grid renders (mirror of the engine's
// Sequenced pick length).

#include <ui/SequencerGridGeometry.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <map>
#include <vector>

using namespace nedit;

namespace {

constexpr double kViewW = 900.0;
constexpr double kViewH = 400.0;

} // namespace

TEST_CASE ("sequencer layout: spreads rows when they fit, no scroll")
{
    const auto l = ui::computeSequencerLayout (kViewW, kViewH, 16, 16);

    REQUIRE (l.totalRows == 16);
    REQUIRE (l.totalCols == 16);
    CHECK (l.colWidth == Catch::Approx (kViewW / 16.0));
    CHECK (l.rowHeight == Catch::Approx (kViewH / 16.0));
    CHECK (l.visibleRows == 16);
    CHECK_FALSE (l.scrolls);
    CHECK (l.maxScroll == 0);
}

TEST_CASE ("sequencer layout: scrolls when rows exceed the viewport")
{
    // 64 slices at min row height 14 => 896px > 400px viewport.
    const auto l = ui::computeSequencerLayout (kViewW, kViewH, 64, 32);

    CHECK (l.scrolls);
    CHECK (l.rowHeight == Catch::Approx (ui::kMinSequencerRowH));
    CHECK (l.visibleRows == static_cast<int> (kViewH / ui::kMinSequencerRowH)); // 28
    CHECK (l.maxScroll == 64 - l.visibleRows);
}

TEST_CASE ("sequencer layout: degenerate viewport is safe")
{
    const auto a = ui::computeSequencerLayout (0.0, 0.0, 10, 4);
    CHECK (a.totalCols == 4);

    const auto b = ui::computeSequencerLayout (kViewW, kViewH, 0, 0);
    CHECK (b.totalRows == 0);
    CHECK (b.totalCols == 1);

    CHECK (ui::clampSequencerScroll (b, 99) == 0);
}

TEST_CASE ("sequencer hit-testing: column from x")
{
    const auto l = ui::computeSequencerLayout (kViewW, kViewH, 16, 8);
    CHECK (ui::columnFromX (l, 0.0, 0.0) == 0);
    CHECK (ui::columnFromX (l, kViewW / 8.0 + 0.5, 0.0) == 1);
    CHECK (ui::columnFromX (l, kViewW - 0.5, 0.0) == 7);
    CHECK (ui::columnFromX (l, -1.0, 0.0) == -1);
    CHECK (ui::columnFromX (l, kViewW + 1.0, 0.0) == -1);
}

TEST_CASE ("sequencer hit-testing: piano-roll row from bottom-up y")
{
    const auto l = ui::computeSequencerLayout (kViewW, kViewH, 16, 8);

    // Row 0 (first slice) sits at the BOTTOM: near viewBottom is row 0.
    CHECK (ui::rowFromBottomY (l, kViewH - 1.0, 0.0, kViewH, 0) == 0);
    // Just above the row0/row1 boundary is row 1.
    CHECK (ui::rowFromBottomY (l, kViewH - l.rowHeight - 1.0, 0.0, kViewH, 0) == 1);
    // Top pixel maps to the top row (row 15).
    CHECK (ui::rowFromBottomY (l, 0.0, 0.0, kViewH, 0) == 15);
    // A band interior: y=300 spans band [300,325) => row 3.
    CHECK (ui::rowFromBottomY (l, 300.0, 0.0, kViewH, 0) == 3);

    // Out of range.
    CHECK (ui::rowFromBottomY (l, kViewH + 5.0, 0.0, kViewH, 0) == -1);
    CHECK (ui::rowFromBottomY (l, kViewH, 0.0, kViewH, 0) == -1);   // exactly at bottom = outside
    CHECK (ui::rowFromBottomY (l, -1.0, 0.0, kViewH, 0) == -1);
}

TEST_CASE ("sequencer hit-testing: scroll offset shifts the visible rows")
{
    const auto l = ui::computeSequencerLayout (kViewW, kViewH, 64, 8);
    const int scroll = 10;

    // Bottom of the viewport now shows row `scroll`.
    CHECK (ui::rowFromBottomY (l, kViewH - 0.5, 0.0, kViewH, scroll) == scroll);

    // Scroll is clamped.
    CHECK (ui::clampSequencerScroll (l, 999) == l.maxScroll);
    CHECK (ui::clampSequencerScroll (l, -5) == 0);
}

TEST_CASE ("sequencer natural length: slice duration quantized to steps")
{
    // 1 second slice at 120 bpm => 2 beats. At 16n (quarter note = 1 beat)
    // that's 2 steps.
    CHECK (ui::naturalStepsForSlice (48000, 48000, 120.0, 1.0) == 2);
    // Half the length => 1 beat => 1 step.
    CHECK (ui::naturalStepsForSlice (24000, 48000, 120.0, 1.0) == 1);
    // Shorter than a step still occupies its own step.
    CHECK (ui::naturalStepsForSlice (1000, 48000, 120.0, 1.0) == 1);
    // Degenerate inputs => 1.
    CHECK (ui::naturalStepsForSlice (0, 48000, 120.0, 1.0) == 1);
    CHECK (ui::naturalStepsForSlice (48000, 0.0, 120.0, 1.0) == 1);
}

TEST_CASE ("sequencer cell bars: natural span, clamped to next active column")
{
    // 16 rows x 8 cols, single row 0 occupied at col 0 with a 2-step natural
    // length; nothing else active => bar spans [0, 3) then wraps naturally.
    const auto l = ui::computeSequencerLayout (kViewW, kViewH, 16, 8);

    std::vector<std::int8_t> grid (16 * 8, -1);
    grid[0] = 0;   // row 0, col 0, style 0

    std::map<std::uint32_t, std::uint16_t> extensions;

    const auto bars = ui::computeCellBars (0, 0, 48000, 48000, 120.0,
                                           state::kNoteValue4n, l, grid, extensions);
    REQUIRE (bars.size() == 1);
    CHECK (bars[0].row == 0);
    CHECK (bars[0].startColumn == 0);
    CHECK (bars[0].endColumn == 2);   // 2-step natural length, empty grid ahead
}

TEST_CASE ("sequencer cell bars: extension extends the bar")
{
    const auto l = ui::computeSequencerLayout (kViewW, kViewH, 16, 8);
    std::vector<std::int8_t> grid (16 * 8, -1);
    grid[0] = 0;   // row 0, col 0

    std::map<std::uint32_t, std::uint16_t> extensions;
    extensions[0] = 4;   // extend to 4 declared steps (natural 2 is smaller)

    const auto bars = ui::computeCellBars (0, 0, 48000, 48000, 120.0,
                                           state::kNoteValue4n, l, grid, extensions);
    REQUIRE (bars.size() == 1);
    CHECK (bars[0].startColumn == 0);
    CHECK (bars[0].endColumn == 4);
}

TEST_CASE ("sequencer cell bars: anticipatory clamp at the next active column")
{
    const auto l = ui::computeSequencerLayout (kViewW, kViewH, 16, 8);
    std::vector<std::int8_t> grid (16 * 8, -1);
    // Row 0, col 0 with a 5-step natural length (5s slice at 120bpm).
    grid[0] = 0;
    grid[1 * 8 + 3] = 1;         // row 1, col 3 active => cuts the bar at col 3

    std::map<std::uint32_t, std::uint16_t> extensions;

    // Natural 5 steps would reach col 5, but col 3 is active => clamp to 3.
    const auto bars = ui::computeCellBars (0, 0, 240000, 48000, 120.0,
                                           state::kNoteValue4n, l, grid, extensions);
    REQUIRE (bars.size() == 1);
    CHECK (bars[0].endColumn == 3);   // clamped short of the active column
}

// ---------------------------------------------------------------------------
// Issue #2 scroll/pan viewport over the grid (SequencerViewport).
// ---------------------------------------------------------------------------

TEST_CASE ("sequencer viewport: content extents and cell sizes track zoom")
{
    const auto l = ui::computeSequencerLayout (kViewW, kViewH, 16, 16);

    // Zoom 1.0 = the base cell size; the grid exactly fills the viewport.
    ui::SequencerViewport fit;
    CHECK (ui::sequencerContentExtent (l, false, fit) == Catch::Approx (kViewW));
    CHECK (ui::sequencerContentExtent (l, true, fit) == Catch::Approx (kViewH));
    CHECK (ui::sequencerCellWidth (l, fit) == Catch::Approx (l.colWidth));
    CHECK (ui::sequencerCellHeight (l, fit) == Catch::Approx (l.rowHeight));
    CHECK (ui::sequencerMaxScroll (l, false, kViewW, fit) == Catch::Approx (0.0));
    CHECK (ui::sequencerMaxScroll (l, true, kViewH, fit) == Catch::Approx (0.0));

    // Zoomed 2x / 0.5x.
    ui::SequencerViewport z { 2.0, 0.5, 0.0, 0.0 };
    CHECK (ui::sequencerContentExtent (l, false, z) == Catch::Approx (2.0 * kViewW));
    CHECK (ui::sequencerContentExtent (l, true, z) == Catch::Approx (0.5 * kViewH));
    CHECK (ui::sequencerCellWidth (l, z) == Catch::Approx (2.0 * l.colWidth));
    CHECK (ui::sequencerCellHeight (l, z) == Catch::Approx (0.5 * l.rowHeight));

    // Y zoomed out so far the content fits: no Y scroll, origin pinned to 0.
    CHECK (ui::sequencerMaxScroll (l, true, kViewH, z) == Catch::Approx (0.0));
    CHECK (ui::sequencerOriginPx (l, true, kViewH, z) == Catch::Approx (0.0));

    // X scrolled: originPx = origin * maxScroll.
    const double maxX = ui::sequencerMaxScroll (l, false, kViewW, z);
    ui::SequencerViewport half { 2.0, 0.5, 0.5, 0.5 };
    CHECK (ui::sequencerOriginPx (l, false, kViewW, half) == Catch::Approx (0.5 * maxX));
}

TEST_CASE ("sequencer viewport: clamping keeps zoom and origin in range")
{
    auto vp = ui::clampSequencerViewport ({ 100.0, -3.0, 5.0, -0.2 });
    CHECK (vp.zoomX == Catch::Approx (ui::kMaxSequencerZoom));
    CHECK (vp.zoomY == Catch::Approx (ui::kMinSequencerZoom));
    CHECK (vp.originX == Catch::Approx (1.0));
    CHECK (vp.originY == Catch::Approx (0.0));

    CHECK (ui::clampSequencerZoom (0.0) == Catch::Approx (ui::kMinSequencerZoom));

    // newZoomFactor clamps the RESULT, so the returned factor keeps the
    // existing zoom below max or above min.
    CHECK (ui::newZoomFactor (4.0, 4.0) == Catch::Approx (ui::kMaxSequencerZoom / 4.0));
    CHECK (ui::newZoomFactor (1.0, 0.001) == Catch::Approx (ui::kMinSequencerZoom));
}

TEST_CASE ("sequencer viewport: zoom-out floor is the fill-the-view fit")
{
    // Few rows: everything fits at base zoom, so the fit-min is 1.0 on both
    // axes (columns always fill; rows fill because rowHeight = viewH/rows).
    // Zooming out cannot go below 1.0 -- that would leave dead space.
    {
        const auto l = ui::computeSequencerLayout (kViewW, kViewH, 8, 16);
        CHECK (ui::sequencerMinZoom (l, false, kViewW) == Catch::Approx (1.0));
        CHECK (ui::sequencerMinZoom (l, true, kViewH) == Catch::Approx (1.0));

        const auto out = ui::zoomSequencerViewport (
            l, kViewW, kViewH, ui::SequencerViewport {}, 0.5, 0.5, 0.001, 0.001);
        CHECK (out.zoomX == Catch::Approx (1.0));
        CHECK (out.zoomY == Catch::Approx (1.0));
    }

    // Many rows: base content overflows in Y, so Y's fit-min is < 1 (the zoom
    // at which every row shrinks to exactly fill the height); X still fills at
    // 1.0. Below the fit-min there would be empty space, so it is the floor.
    {
        const auto l = ui::computeSequencerLayout (kViewW, kViewH, 64, 32);
        const double fitY = ui::sequencerMinZoom (l, true, kViewH);
        CHECK (fitY < 1.0);
        CHECK (fitY > ui::kMinSequencerZoom);   // the floor moved above 0.25
        CHECK (fitY == Catch::Approx (kViewH / (ui::kMinSequencerRowH * 64.0)));
        CHECK (ui::sequencerMinZoom (l, false, kViewW) == Catch::Approx (1.0));

        // At the fit-min the content exactly fills the viewport (no dead space).
        const ui::SequencerViewport atFit { 1.0, fitY, 0.0, 0.0 };
        CHECK (ui::sequencerContentExtent (l, true, atFit) == Catch::Approx (kViewH));

        // Repeated wheel-out saturates at the fit-min, not kMinSequencerZoom.
        ui::SequencerViewport out;
        for (int i = 0; i < 40; ++i)
            out = ui::zoomSequencerViewport (l, kViewW, kViewH, out, 0.5, 0.5,
                                             1.0 / ui::kSequencerZoomPerNotch,
                                             1.0 / ui::kSequencerZoomPerNotch);
        CHECK (out.zoomY == Catch::Approx (fitY));
        CHECK (out.zoomX == Catch::Approx (1.0));
    }
}

TEST_CASE ("sequencer viewport: anchored zoom keeps the cursor content fixed")
{
    const auto l = ui::computeSequencerLayout (kViewW, kViewH, 16, 16);
    ui::SequencerViewport vp;

    // Zoom 2x both axes about the 25%-25% point. Before: content px 225
    // (X) under viewport px 225, content px 100 (Y) under viewport px 100.
    const auto out = ui::zoomSequencerViewport (
        l, kViewW, kViewH, vp, 0.25, 0.25, 2.0, 2.0);

    CHECK (out.zoomX == Catch::Approx (2.0));
    CHECK (out.zoomY == Catch::Approx (2.0));
    // Anchor: after zoom the SAME content pixel must sit under the cursor.
    const double cursorXpx = 0.25 * kViewW;
    const double cursorYpx = 0.25 * kViewH;
    const double cx = ui::sequencerOriginPx (l, false, kViewW, out) + cursorXpx;
    const double cy = ui::sequencerOriginPx (l, true, kViewH, out) + cursorYpx;
    CHECK (cx == Catch::Approx (2.0 * 0.25 * kViewW)); // 225 * 2
    CHECK (cy == Catch::Approx (2.0 * 0.25 * kViewH)); // 100 * 2

    // Tightly zoomed against a corner keeps the origin clamped to [0,1].
    const auto edge = ui::zoomSequencerViewport (
        l, kViewW, kViewH, vp, 1.0, 0.0, 1000.0, 1000.0);
    CHECK (edge.zoomX == Catch::Approx (ui::kMaxSequencerZoom));
    CHECK (edge.originX == Catch::Approx (1.0));
    CHECK (edge.originY == Catch::Approx (0.0));
}

TEST_CASE ("sequencer viewport: scrollbar-locked wheel moves one axis only")
{
    const auto l = ui::computeSequencerLayout (kViewW, kViewH, 64, 32);
    ui::SequencerViewport vp;

    // 64 rows scroll; 32 cols at 28.125px = exactly 900px, no X scroll.
    const auto out = ui::zoomSequencerViewport (
        l, kViewW, kViewH, vp, 0.5, 0.5, ui::kSequencerZoomPerNotch, 1.0);

    CHECK (out.zoomX == Catch::Approx (ui::kSequencerZoomPerNotch));
    CHECK (out.zoomY == Catch::Approx (1.0));
    // Origin in a non-scrolling axis is pinned at 0; Y centered stays 0.
    CHECK (out.originY == Catch::Approx (0.0));
}

TEST_CASE ("sequencer viewport: pan drags the content opposite to the hand")
{
    const auto l = ui::computeSequencerLayout (kViewW, kViewH, 64, 32);
    ui::SequencerViewport vp { 2.0, 2.0, 0.5, 0.5 };

    // Drag right (positive X) scrolls content left => origin drops by
    // deltaX / maxScroll. 64 rows at 14px * 2 = 1792px vs 400px view.
    const double maxX = ui::sequencerMaxScroll (l, false, kViewW, vp);
    const double maxY = ui::sequencerMaxScroll (l, true, kViewH, vp);

    const auto out = ui::panSequencerViewport (
        l, kViewW, kViewH, vp, 0.5 * maxX, -0.5 * maxY);
    CHECK (out.originX == Catch::Approx (0.0));   // dragged to the far right
    CHECK (out.originY == Catch::Approx (1.0));   // dragged up to the top

    // Dragging beyond the limit clamps.
    const auto clamped = ui::panSequencerViewport (l, kViewW, kViewH, vp, -maxX, 2.0 * maxY);
    CHECK (clamped.originX == Catch::Approx (1.0));
    CHECK (clamped.originY == Catch::Approx (0.0));

    // A fully-visible axis ignores panning on that axis.
    ui::SequencerViewport fit;
    const auto noScroll = ui::panSequencerViewport (
        ui::computeSequencerLayout (kViewW, kViewH, 16, 16), kViewW, kViewH,
        fit, -500.0, 500.0);
    CHECK (noScroll.originX == Catch::Approx (0.0));
    CHECK (noScroll.originY == Catch::Approx (0.0));
}

TEST_CASE ("sequencer viewport: row/column pixel placement with scroll")
{
    const auto l = ui::computeSequencerLayout (kViewW, kViewH, 64, 32);
    ui::SequencerViewport z { 1.0, 1.0, 0.25, 0.5 };
    const double ox = ui::sequencerOriginPx (l, false, kViewW, z);
    const double oy = ui::sequencerOriginPx (l, true, kViewH, z);

    const double colW = ui::sequencerCellWidth (l, z);
    const double rowH = ui::sequencerCellHeight (l, z);
    CHECK (ui::sequencerColumnX (l, z, ox, 0) == Catch::Approx (-ox));
    CHECK (ui::sequencerColumnX (l, z, ox, 5) == Catch::Approx (5.0 * colW - ox));
    // 64 rows at 14px = 896px content in a 400px window: originY 0.5 scrolls
    // by 0.5 * 496 = 248px, so row 0's bottom sits at 896 - 248 = 648px and
    // the row above it 14px higher.
    CHECK (ui::sequencerRowBottom (l, z, 0.0, oy, 0) == Catch::Approx (648.0));
    CHECK (ui::sequencerRowBottom (l, z, 0.0, oy, 7) == Catch::Approx (648.0 - 7.0 * rowH));
}

TEST_CASE ("sequencer viewport: zoom-aware hit-testing mirrors the geometry")
{
    const auto l = ui::computeSequencerLayout (kViewW, kViewH, 64, 32);
    ui::SequencerViewport vp { 2.0, 2.0, 0.5, 0.5 };
    const double ox = ui::sequencerOriginPx (l, false, kViewW, vp);
    const double oy = ui::sequencerOriginPx (l, true, kViewH, vp);

    const double colW = ui::sequencerCellWidth (l, vp);
    const double rowH = ui::sequencerCellHeight (l, vp);

    // The content pixel under each cell maps back to that cell. With zoom 2
    // and origin 0.5 the visible window falls on cols 8..23 and rows 25..38.
    for (int c : { 8, 16, 23 })
    {
        const double cxw = ui::sequencerColumnX (l, vp, ox, c) + colW * 0.5;
        CHECK (ui::sequencerColumnFromX (l, vp, cxw, 0.0, ox) == c);
    }
    for (int r : { 25, 32, 38 })
    {
        const double ryr = ui::sequencerRowBottom (l, vp, 0.0, oy, r) - rowH * 0.5;
        CHECK (ui::sequencerRowFromY (l, vp, ryr, 0.0, kViewH, oy) == r);
    }

    // Outside the grid (left / below row 0's band) => -1.
    CHECK (ui::sequencerColumnFromX (l, vp, -1.0, 0.0, ox) == -1);
    CHECK (ui::sequencerRowFromY (l, vp, kViewH - oy - rowH - 0.5, 0.0, kViewH, oy) == -1);

    // An unzoomed viewport the same as rowFromBottomY/columnFromX at scroll 0.
    const auto flat = ui::computeSequencerLayout (kViewW, kViewH, 16, 8);
    ui::SequencerViewport fit;
    CHECK (ui::sequencerRowFromY (flat, fit, kViewH - 0.5, 0.0, kViewH, 0.0) == 0);
    CHECK (ui::sequencerColumnFromX (flat, fit, 0.0, 0.0, 0.0) == 0);
}

TEST_CASE ("sequencer overlay scrollbar: knob tracks visible fraction + origin")
{
    const auto l = ui::computeSequencerLayout (kViewW, kViewH, 64, 32);

    // Vertical: 64 rows * 14px = 896px in a 400px viewport.
    ui::SequencerViewport vp;
    const auto vbar = ui::computeSequencerScrollBar (true, l, kViewH, vp);
    REQUIRE (vbar.scrollable);
    CHECK (vbar.visibleFraction == Catch::Approx (kViewH / 896.0));
    CHECK (vbar.trackStart == Catch::Approx (0.0));
    CHECK (vbar.trackEnd == Catch::Approx (kViewH));
    CHECK (vbar.knobStart == Catch::Approx (0.0));
    CHECK (vbar.knobEnd == Catch::Approx (kViewH / 896.0 * kViewH));

    // At origin 0.5 the knob sits mid-track.
    ui::SequencerViewport mid { 1.0, 1.0, 0.0, 0.5 };
    const auto vmid = ui::computeSequencerScrollBar (true, l, kViewH, mid);
    const double knobLen = vmid.knobEnd - vmid.knobStart;
    CHECK (vmid.knobStart == Catch::Approx (0.5 * (kViewH - knobLen)));

    // Fully scrolled: knob flush with the track end.
    ui::SequencerViewport end { 1.0, 1.0, 0.0, 1.0 };
    const auto vend = ui::computeSequencerScrollBar (true, l, kViewH, end);
    CHECK (vend.knobEnd == Catch::Approx (kViewH));
    // Visible fraction never leaves a min-sized knob even when the content is large.
    CHECK (vend.knobEnd - vend.knobStart >= ui::kSequencerScrollBarMinKnob);

    // Horizontal does not scroll at zoom 1 (32 cols * 28.125 = 900 = view).
    const auto hbar = ui::computeSequencerScrollBar (false, l, kViewW, vp);
    CHECK_FALSE (hbar.scrollable);

    // ... but does once zoomed in, and re-reads the visible fraction.
    ui::SequencerViewport zx { 2.0, 1.0, 0.0, 0.0 };
    const auto hzoom = ui::computeSequencerScrollBar (false, l, kViewW, zx);
    REQUIRE (hzoom.scrollable);
    CHECK (hzoom.visibleFraction == Catch::Approx (0.5));
}

TEST_CASE ("sequencer scroll zone: wheel axis lock and corner ownership")
{
    constexpr double kThick = 10.0;
    constexpr double kHit = 6.0;

    // Canvas interior.
    CHECK (ui::sequencerScrollZone (450.0, 200.0, kViewW, kViewH, kThick, kHit)
           == ui::SequencerScrollZone::canvas);
    // Bottom band (including hit extension) => horizontal bar.
    CHECK (ui::sequencerScrollZone (450.0, kViewH - 1.0, kViewW, kViewH, kThick, kHit)
           == ui::SequencerScrollZone::horizontalBar);
    CHECK (ui::sequencerScrollZone (450.0, kViewH - kThick - kHit + 0.5, kViewW, kViewH, kThick, kHit)
           == ui::SequencerScrollZone::horizontalBar);
    // Just above the band => canvas again.
    CHECK (ui::sequencerScrollZone (450.0, kViewH - kThick - kHit - 0.5, kViewW, kViewH, kThick, kHit)
           == ui::SequencerScrollZone::canvas);
    // Right band above the H bar => vertical bar.
    CHECK (ui::sequencerScrollZone (kViewW - 1.0, 200.0, kViewW, kViewH, kThick, kHit)
           == ui::SequencerScrollZone::verticalBar);
    CHECK (ui::sequencerScrollZone (kViewW - kThick - kHit + 0.5, 200.0, kViewW, kViewH, kThick, kHit)
           == ui::SequencerScrollZone::verticalBar);
    // Bottom-right corner belongs to the horizontal bar's track.
    CHECK (ui::sequencerScrollZone (kViewW - 1.0, kViewH - 1.0, kViewW, kViewH, kThick, kHit)
           == ui::SequencerScrollZone::horizontalBar);
}

TEST_CASE ("cell override menu: per-style param entries with Subdivide + Volume")
{
    using state::PlaybackStyle;
    using state::StyleParamId;
    using ui::CellOverrideMenuKind;

    // Forward exposes no style params; only the general pair.
    auto fwd = ui::cellOverrideMenuEntries (PlaybackStyle::forward);
    REQUIRE (fwd.size() == 2);
    CHECK (fwd[0].id == StyleParamId::subdivide);
    CHECK (fwd[0].kind == CellOverrideMenuKind::slider);   // stepped discrete -> slider
    CHECK (fwd[1].id == StyleParamId::volume);
    CHECK (fwd[1].kind == CellOverrideMenuKind::modeSubmenu);  // swept -> mode submenu first
    CHECK (fwd[1].modeId == StyleParamId::volumeMode);

    // Filter Down: Resonance (continuous -> slider) + Filter Type (discrete -> submenu).
    auto filter = ui::cellOverrideMenuEntries (PlaybackStyle::filterDown);
    REQUIRE (filter.size() == 4);
    CHECK (filter[0].id == StyleParamId::filterResonance);
    CHECK (filter[0].kind == CellOverrideMenuKind::slider);
    CHECK (filter[1].id == StyleParamId::filterType);
    CHECK (filter[1].kind == CellOverrideMenuKind::submenu);

    // Scratch: Rate (discrete -> submenu) + two curve dropdowns (submenu).
    auto scratch = ui::cellOverrideMenuEntries (PlaybackStyle::scratch);
    REQUIRE (scratch.size() == 5);
    CHECK (scratch[0].id == StyleParamId::scratchRate);
    CHECK (scratch[0].kind == CellOverrideMenuKind::submenu);
    CHECK (scratch[1].id == StyleParamId::scratchForwardCurve);
    CHECK (scratch[2].id == StyleParamId::scratchBackwardCurve);

    // Bitcrush swept values: SR Reduction (mode submenu) + Bit Depth (submenu).
    auto crush = ui::cellOverrideMenuEntries (PlaybackStyle::bitcrush);
    REQUIRE (crush.size() == 4);
    CHECK (crush[0].id == StyleParamId::srReduction);
    CHECK (crush[0].kind == CellOverrideMenuKind::modeSubmenu);
    CHECK (crush[0].modeId == StyleParamId::srReductionMode);
    CHECK (crush[1].id == StyleParamId::bitDepth);
    CHECK (crush[1].kind == CellOverrideMenuKind::modeSubmenu);
    CHECK (crush[1].modeId == StyleParamId::bitDepthMode);

    // Every menu ends with Subdivide + Volume regardless of style.
    for (const auto style : { PlaybackStyle::pingPong, PlaybackStyle::tapeStop,
                              PlaybackStyle::stretch, PlaybackStyle::filterUp,
                              PlaybackStyle::flanger })
    {
        auto entries = ui::cellOverrideMenuEntries (style);
        REQUIRE (entries.size() >= 2);
        const std::size_t last = entries.size() - 2;
        CHECK (entries[last].id == StyleParamId::subdivide);
        CHECK (entries[last + 1].id == StyleParamId::volume);
        CHECK (entries[last + 1].kind == CellOverrideMenuKind::modeSubmenu);
        CHECK (entries[last + 1].modeId == StyleParamId::volumeMode);
    }
}
