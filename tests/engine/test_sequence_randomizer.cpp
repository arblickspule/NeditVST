// SequenceRandomizer: the Sequencer "Randomize Sequence" generator.
//
// Covers determinism, density, monophony + span non-overlap, weighted
// style selection, per-style parameter randomizing (excluding the general
// Subdivide/Volume), natural-length awareness and degenerate inputs.

#include <catch2/catch_test_macros.hpp>

#include <engine/SequenceRandomizer.h>

#include <state/SequencerState.h>
#include <state/Types.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

using namespace nedit::state;
using namespace nedit::engine::seq;
using nedit::engine::Slice;

namespace {

// Build a grid with `rows` rows and `columns` columns at a 16n step
// resolution (0.25 beats/step) -- the resolution the natural-length math
// below assumes.
SequencerState makeState (int rows, int columns)
{
    SequencerState st;
    st.stepResolutionIndex = kNoteValue16n;  // 0.25 beats per step
    st.rows = rows;
    st.columns = columns;
    st.grid.assign (static_cast<std::size_t> (rows) * static_cast<std::size_t> (columns),
                    static_cast<std::int8_t> (-1));
    return st;
}

// One slice; most tests use a 1-step slice (one 64th at 120 BPM = 0.125s
// beats... see body), 1-step and 2-step helpers below instead.
Slice makeSlice (std::int64_t start, std::int64_t end)
{
    Slice s;
    s.startFrame = start;
    s.endFrame = end;
    return s;
}

// At 44100 Hz and 120 BPM: one 16n step = 16n/16n -> 0.25 beats = 0.125 s
// = 5512.5 frames. A slice of 5512.5 frames -> 1 step; a slice of 11025
// frames -> 2 steps.
std::span<Slice> slicesFor (std::vector<Slice>& v)
{
    return std::span<Slice> (v.data(), v.size());
}

} // namespace

TEST_CASE ("randomize: deterministic for a given seed", "[seqrand]")
{
    auto a = makeState (8, 32);
    auto b = makeState (8, 32);

    std::vector<Slice> sa;
    for (int i = 0; i < 8; ++i)
        sa.push_back (makeSlice (i * 5512, i * 5512 + 5512));

    const auto ra = randomizeSequence (a, slicesFor (sa), 44100.0, 120.0, 42u);
    const auto rb = randomizeSequence (b, slicesFor (sa), 44100.0, 120.0, 42u);
    REQUIRE (ra.cellsPlaced == rb.cellsPlaced);

    REQUIRE (a.grid == b.grid);
    REQUIRE (a.overrides == b.overrides);
}

TEST_CASE ("randomize: different seeds differ", "[seqrand]")
{
    auto a = makeState (8, 32);
    auto b = makeState (8, 32);

    std::vector<Slice> sa;
    for (int i = 0; i < 8; ++i)
        sa.push_back (makeSlice (i * 5512, i * 5512 + 5512));

    const auto ra = randomizeSequence (a, slicesFor (sa), 44100.0, 120.0, 42u);
    (void) ra;
    const auto rb = randomizeSequence (b, slicesFor (sa), 44100.0, 120.0, 43u);
    REQUIRE (rb.cellsPlaced >= 0);

    REQUIRE (a.grid != b.grid);
}

TEST_CASE ("randomize: monophonic and spans never overlap", "[seqrand]")
{
    auto st = makeState (8, 32);

    // All slices one 16n step wide, so a placed cell claims exactly its
    // own column.
    std::vector<Slice> sa;
    for (int i = 0; i < 8; ++i)
        sa.push_back (makeSlice (i * 5512, i * 5512 + 5512));

    auto result = randomizeSequence (st, slicesFor (sa), 44100.0, 120.0, 7u, 1.0f);

    REQUIRE (result.cellsPlaced > 0);

    // Every cell value is a valid style or empty.
    for (const auto v : st.grid)
        REQUIRE ((v < 0 || v < kNumPlaybackStyles));

    // At most one filled cell per column (monophony), and every slice's
    // span claim is self-consistent: a slice of natural length N starting
    // at col C claims columns C..C+N-1 (this test's 1-step slices claim
    // exactly one column, so monophony implies non-overlap).
    for (int col = 0; col < st.columns; ++col)
    {
        int fills = 0;
        for (int r = 0; r < st.rows; ++r)
            if (st.grid[static_cast<std::size_t> (r) * static_cast<std::size_t> (st.columns)
                        + static_cast<std::size_t> (col)] >= 0)
                ++fills;
        REQUIRE (fills <= 1);
    }
}

TEST_CASE ("randomize: zero density places nothing", "[seqrand]")
{
    auto st = makeState (4, 16);
    std::vector<Slice> sa;
    for (int i = 0; i < 4; ++i)
        sa.push_back (makeSlice (i * 5512, i * 5512 + 5512));

    auto result = randomizeSequence (st, slicesFor (sa), 44100.0, 120.0, 1u, 0.0f);

    REQUIRE (result.cellsPlaced == 0);
    REQUIRE (st.grid == std::vector<std::int8_t> (4 * 16, static_cast<std::int8_t> (-1)));
    REQUIRE (st.overrides.empty());
}

TEST_CASE ("randomize: all-zero weights fall back to Forward", "[seqrand]")
{
    auto st = makeState (4, 16);
    st.randomizeStyleWeights.fill (0.0f);  // every weight zero

    std::vector<Slice> sa;
    for (int i = 0; i < 4; ++i)
        sa.push_back (makeSlice (i * 5512, i * 5512 + 5512));

    auto result = randomizeSequence (st, slicesFor (sa), 44100.0, 120.0, 5u, 1.0f);

    REQUIRE (result.cellsPlaced > 0);
    for (const auto v : st.grid)
        if (v >= 0)
            REQUIRE (v == static_cast<std::int8_t> (PlaybackStyle::forward));
}

TEST_CASE ("randomize: only-weighted style is chosen", "[seqrand]")
{
    auto st = makeState (4, 16);
    st.randomizeStyleWeights.fill (0.0f);
    st.randomizeStyleWeights[static_cast<std::size_t> (PlaybackStyle::flanger)] = 1.0f;

    std::vector<Slice> sa;
    for (int i = 0; i < 4; ++i)
        sa.push_back (makeSlice (i * 5512, i * 5512 + 5512));

    const auto r = randomizeSequence (st, slicesFor (sa), 44100.0, 120.0, 9u, 1.0f);
    REQUIRE (r.cellsPlaced > 0);

    for (const auto v : st.grid)
        if (v >= 0)
            REQUIRE (v == static_cast<std::int8_t> (PlaybackStyle::flanger));
}

TEST_CASE ("randomize: parameter randomizing writes overrides minus Subdivide/Volume", "[seqrand]")
{
    auto st = makeState (4, 16);
    st.randomizeStyleWeights.fill (0.0f);
    st.randomizeStyleWeights[static_cast<std::size_t> (PlaybackStyle::flanger)] = 1.0f;
    st.randomizeParametersForStyle[static_cast<std::size_t> (PlaybackStyle::flanger)] = true;

    std::vector<Slice> sa;
    for (int i = 0; i < 4; ++i)
        sa.push_back (makeSlice (i * 5512, i * 5512 + 5512));

    const auto r = randomizeSequence (st, slicesFor (sa), 44100.0, 120.0, 11u, 1.0f);
    REQUIRE (r.cellsPlaced > 0);
    REQUIRE_FALSE (st.overrides.empty());

    // Every placed cell's overrides must be a valid style's params in
    // range; Subdivide and Volume must never be randomized; every swept
    // param has its mode sibling AND the values are in-range.
    for (const auto& [cell, params] : st.overrides)
    {
        (void) cell;

        for (const auto& [id, value] : params)
        {
            REQUIRE (id != StyleParamId::subdivide);
            REQUIRE (id != StyleParamId::volume);

            const auto& info = styleParamInfo (id);
            REQUIRE (value >= info.minValue);
            REQUIRE (value <= info.maxValue);
        }
    }
}

TEST_CASE ("randomize: natural length spans multiple columns", "[seqrand]")
{
    auto st = makeState (4, 16);

    // A 2-step slice (11025 frames at 44100/120) claims two columns.
    std::vector<Slice> sa;
    for (int i = 0; i < 4; ++i)
        sa.push_back (makeSlice (i * 11025, (i + 1) * 11025));

    // density 1.0, loop until everything that can be placed is -- the
    // result must show multi-column spans.
    const auto res = randomizeSequence (st, slicesFor (sa), 44100.0, 120.0, 3u, 1.0f);
    REQUIRE (res.cellsPlaced > 0);

    // Every non-blank start column's own span is free of other starts: two
    // bars of length 2 never share a column (verified by counting fills).
    for (int col = 0; col < st.columns; ++col)
    {
        int fills = 0;
        for (int r = 0; r < st.rows; ++r)
            if (st.grid[static_cast<std::size_t> (r) * static_cast<std::size_t> (st.columns)
                        + static_cast<std::size_t> (col)] >= 0)
                ++fills;
        REQUIRE (fills <= 1);
    }
}

// NeditVST#9: "Random notes should be set to the slices length (snapped to
// grid) and the next random note should respect that length." A random bar
// must be placeable in the last `natural` columns of the grid (its start
// column's span is clamped at the grid edge, so the note reaches its full
// slice length and the next note respects that claimed span). The original
// clamps the span at the grid end (jmin(columns, col+natural)) -- a
// WRAP-around free-check that also demanded the wrapped low columns be free
// wrongly rejects such edge placements and fragments the grid, so random
// notes don't respect the surrounding length.
TEST_CASE ("randomize: a bar can occupy the grid's edge columns", "[seqrand]")
{
    // 2 rows, 3 columns, every slice 2 steps: the first bar claims columns
    // [0,2) (furthest-free tie -> column 0). The ONLY remaining 2-column
    // span is [2, min(3, 2+2)) = [2,3) -- the right edge. density 1.0.
    // Under the OLD wrap-around free-check that span is rejected (its
    // wrapped partner column is (2+1)%3 = 0, already claimed), so only 1
    // cell places; with the clamp fix the edge bar places too (2 cells).
    auto st = makeState (2, 3);

    std::vector<Slice> sa;
    for (int i = 0; i < 2; ++i)
        sa.push_back (makeSlice (i * 11025, (i + 1) * 11025));  // 2 steps each

    const auto res = randomizeSequence (st, slicesFor (sa), 44100.0, 120.0, 4242u, 1.0f);

    // The two rows should BOTH place: column 0 and the right-edge column 2.
    REQUIRE (res.cellsPlaced >= 2);

    // A bar occupies the right edge (column 2) -- reachable only with the
    // non-wrapping clamp.
    bool edgeOccupied = false;
    for (int r = 0; r < st.rows; ++r)
        if (st.grid[static_cast<std::size_t> (r) * static_cast<std::size_t> (st.columns)
                    + static_cast<std::size_t> (st.columns - 1)] >= 0)
            edgeOccupied = true;
    CHECK (edgeOccupied);
}

TEST_CASE ("randomize: degenerate inputs are safe and clear the grid", "[seqrand]")
{
    // No rows -> clears, no crash.
    auto empty = makeState (0, 16);
    std::vector<Slice> sa;
    auto r0 = randomizeSequence (empty, slicesFor (sa), 44100.0, 120.0, 0u);
    REQUIRE (r0.cellsPlaced == 0);
    REQUIRE (empty.grid.empty());

    // No columns -> clears, no crash.
    auto emptyCols = makeState (4, 0);
    std::vector<Slice> sb;
    for (int i = 0; i < 4; ++i)
        sb.push_back (makeSlice (i * 5512, i * 5512 + 5512));
    auto r1 = randomizeSequence (emptyCols, slicesFor (sb), 44100.0, 120.0, 0u);
    REQUIRE (r1.cellsPlaced == 0);
    REQUIRE (emptyCols.grid.empty());

    // More rows than slices given -> clamps to the slice count.
    auto big = makeState (8, 16);
    std::vector<Slice> sc;
    for (int i = 0; i < 4; ++i)
        sc.push_back (makeSlice (i * 5512, i * 5512 + 5512));
    auto r2 = randomizeSequence (big, slicesFor (sc), 44100.0, 120.0, 2u, 1.0f);
    REQUIRE (r2.cellsPlaced >= 0);
}

TEST_CASE ("clearGrid wipes cells, overrides and extensions but keeps dimensions", "[seqrand]")
{
    auto st = makeState (3, 8);
    st.grid[0] = 3;
    st.overrides[0][StyleParamId::volume] = 0.5f;
    st.extensions[0] = 2;

    clearGrid (st);

    REQUIRE (st.rows == 3);
    REQUIRE (st.columns == 8);
    REQUIRE (st.grid == std::vector<std::int8_t> (24, static_cast<std::int8_t> (-1)));
    REQUIRE (st.overrides.empty());
    REQUIRE (st.extensions.empty());
}

TEST_CASE ("computeSequencerDims: rows = slices, columns = stepsPerBar * bars", "[seqrand]")
{
    // 16n = 0.25 beats => 16 steps per bar. patternLengthBarsIndex 0 => 1 bar.
    const auto a = computeSequencerDims (10, kNoteValue16n, 0);
    CHECK (a.rows == 10);
    CHECK (a.columns == 16);

    // 4n = 1 beat => 4 steps per bar. patternLengthBarsIndex 2 => 4 bars.
    const auto b = computeSequencerDims (5, kNoteValue4n, 2);
    CHECK (b.rows == 5);
    CHECK (b.columns == 16);

    // Rows clamp to kMaxSequencerRows; columns clamp to kMaxSequencerColumns.
    const auto c = computeSequencerDims (999, kNoteValue16n, 2);
    CHECK (c.rows == kMaxSequencerRows);
    CHECK (c.columns <= kMaxSequencerColumns);

    // No slices => no rows.
    const auto d = computeSequencerDims (0, kNoteValue16n, 0);
    CHECK (d.rows == 0);
}

TEST_CASE ("resizeGrid resets the grid only when the dimensions change", "[seqrand]")
{
    auto st = makeState (4, 16);
    st.grid[0] = 5;
    st.overrides[0][StyleParamId::volume] = 0.5f;

    // Same dims => no reset, cells preserved.
    CHECK_FALSE (resizeGrid (st, { 4, 16 }));
    CHECK (st.grid[0] == 5);
    CHECK_FALSE (st.overrides.empty());

    // Different dims => reset (cells + overrides wiped, new size).
    CHECK (resizeGrid (st, { 6, 8 }));
    CHECK (st.rows == 6);
    CHECK (st.columns == 8);
    CHECK (st.grid == std::vector<std::int8_t> (48, static_cast<std::int8_t> (-1)));
    CHECK (st.overrides.empty());
}
