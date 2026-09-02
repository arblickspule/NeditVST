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
