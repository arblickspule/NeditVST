#include "SequenceRandomizer.h"

#include <state/Types.h>

#include <algorithm>
#include <cmath>
#include <random>

namespace nedit::engine::seq {

namespace {

// Beat value of a (possibly unsanitized) note-value index; 0.0 if bogus.
[[nodiscard]] double noteValueBeats (int index) noexcept
{
    return state::isValidNoteValueIndex (index)
        ? state::kNoteValues[static_cast<std::size_t> (index)].beats
        : 0.0;
}

// Uniform [0,1) in double.
[[nodiscard]] double uniform01 (std::mt19937& rng) noexcept
{
    std::uniform_real_distribution<double> dist (0.0, 1.0);
    return dist (rng);
}

[[nodiscard]] int uniformInt (std::mt19937& rng, int n) noexcept
{
    if (n <= 0)
        return 0;
    std::uniform_int_distribution<int> dist (0, n - 1);
    return dist (rng);
}

// Weighted index draw over `count` weights. Falls back to a uniform draw
// when every weight is <= 0 (so an all-zero Randomize weight table still
// places cells -- it just can't pick a style).
[[nodiscard]] int pickWeightedIndex (std::mt19937& rng,
                                     std::span<const float> weights,
                                     int fallbackIndex) noexcept
{
    if (weights.empty())
        return fallbackIndex;

    double total = 0.0;
    for (const float w : weights)
        total += std::max (0.0, static_cast<double> (w));

    if (total <= 0.0)
        return fallbackIndex;

    const double target = uniform01 (rng) * total;
    double cumulative = 0.0;

    for (std::size_t i = 0; i < weights.size(); ++i)
    {
        cumulative += std::max (0.0, static_cast<double> (weights[i]));
        if (target <= cumulative)
            return static_cast<int> (i);
    }

    return static_cast<int> (weights.size()) - 1;
}

// Distance (in columns, wrapping) from the end of `start + naturalSteps`
// to the nearest occupied span. base = the column just before `start` used
// only to seed the nearest-cell distance.
[[nodiscard]] int nearestOccupiedDistance (const std::vector<bool>& occupied,
                                           int start, int length,
                                           int columns) noexcept
{
    // Scan outward symmetrically around the candidate's box, measuring the
    // smallest number of steps from any occupied cell to the box (through
    // emptiness). A larger result = the placement sits further from its
    // neighbours = spreads the grid.
    int best = columns + 1;

    for (int c = 0; c < columns; ++c)
    {
        if (! occupied[static_cast<std::size_t> (c)])
            continue;

        // Distance from this occupied column to the candidate box.
        int d = columns;
        for (int k = 0; k < length; ++k)
        {
            const int boxCol = (start + k) % columns;
            int delta = std::abs (c - boxCol);
            delta = std::min (delta, columns - delta);  // wrap
            d = std::min (d, delta);
        }
        best = std::min (best, d);
    }

    return best;
}

} // namespace

void clearGrid (state::SequencerState& state) noexcept
{
    state.grid.assign (static_cast<std::size_t> (state.rows)
                           * static_cast<std::size_t> (state.columns),
                       static_cast<std::int8_t> (-1));
    state.overrides.clear();
    state.extensions.clear();
}

SequencerDims computeSequencerDims (int sliceCount, int stepResolutionIndex,
                                    int patternLengthBarsIndex) noexcept
{
    SequencerDims dims;
    dims.rows = std::clamp (sliceCount, 0, state::kMaxSequencerRows);

    const double stepBeats = noteValueBeats (stepResolutionIndex);
    // 4 beats to a bar; steps per bar = that divided by the step value.
    int stepsPerBar = stepBeats > 0.0
                          ? static_cast<int> (std::lround (4.0 / stepBeats))
                          : 16;
    stepsPerBar = std::max (1, stepsPerBar);

    int bars = 1;
    if (patternLengthBarsIndex >= 0
        && patternLengthBarsIndex < static_cast<int> (state::kPatternLengthBarsValues.size()))
        bars = state::kPatternLengthBarsValues[static_cast<std::size_t> (patternLengthBarsIndex)];

    dims.columns = std::clamp (stepsPerBar * bars, 0, state::kMaxSequencerColumns);
    return dims;
}

bool resizeGrid (state::SequencerState& state, SequencerDims dims) noexcept
{
    if (dims.rows == state.rows && dims.columns == state.columns)
        return false;
    state.rows = dims.rows;
    state.columns = dims.columns;
    clearGrid (state);   // documented reset-on-dimension-change contract
    return true;
}

RandomizeResult randomizeSequence (state::SequencerState& st,
                                   std::span<const Slice> slices,
                                   double sampleSampleRate,
                                   double originalBpm,
                                   std::uint32_t seed,
                                   float placementProbability) noexcept
{
    RandomizeResult result;

    // Rows available = the smaller of the grid's declared rows and the
    // real slice count (the grid may reference slices that no longer exist
    // after a rebuild). Columns come straight from state.
    const int rows = std::min (st.rows, static_cast<int> (slices.size()));
    const int columns = st.columns;

    if (rows <= 0 || columns <= 0)
    {
        clearGrid (st);
        return result;
    }

    const double stepBeats = noteValueBeats (st.stepResolutionIndex);
    const double density = std::clamp (static_cast<double> (placementProbability),
                                       0.0, 1.0);

    // Natural length per slice in steps: slice duration at the source
    // tempo, quantized to the step resolution, floored at 1 (a very short
    // slice still occupies its starting step so it's audible).
    std::vector<int> naturalSteps (static_cast<std::size_t> (rows), 1);

    for (int row = 0; row < rows; ++row)
    {
        const auto& slice = slices[static_cast<std::size_t> (row)];
        const std::int64_t sliceLength = slice.lengthFrames();

        if (sliceLength > 0 && sampleSampleRate > 0.0 && stepBeats > 0.0
            && originalBpm > 0.0)
        {
            const double sliceSeconds = static_cast<double> (sliceLength) / sampleSampleRate;
            const double naturalBeats = sliceSeconds * (originalBpm / 60.0);
            naturalSteps[static_cast<std::size_t> (row)] =
                std::max (1, static_cast<int> (std::lround (naturalBeats / stepBeats)));
        }
    }

    std::mt19937 rng (seed);

    // Which columns are claimed by an already-placed bar's FULL span
    // (prevents a longer bar being fragmented by another hit landing
    // mid-span). Also the per-cell value won't be written twice.
    std::vector<bool> columnOccupied (static_cast<std::size_t> (columns), false);

    // Wipe the working grid + all per-cell data before rebuilding, exactly
    // like the original's clearSequence() (Clear Sequence and Randomize
    // share the same wipe).
    clearGrid (st);

    // Row order, reshuffled per pass (Fisher-Yates).
    std::vector<int> rowOrder (static_cast<std::size_t> (rows));
    for (int i = 0; i < rows; ++i)
        rowOrder[static_cast<std::size_t> (i)] = i;

    constexpr int kMaxPasses = 4096;

    for (int pass = 0; pass < kMaxPasses; ++pass)
    {
        for (int i = rows - 1; i > 0; --i)
        {
            const int j = uniformInt (rng, i + 1);
            std::swap (rowOrder[static_cast<std::size_t> (i)],
                       rowOrder[static_cast<std::size_t> (j)]);
        }

        bool placedAnythingThisPass = false;

        for (const int row : rowOrder)
        {
            const int natural = naturalSteps[static_cast<std::size_t> (row)];

            // Collect every free span of this row's natural length, then
            // choose the one that sits furthest from the already-placed
            // bars (spacing-aware). A placed bar can never overlap an
            // occupied column, and a placement that no longer fits anywhere
            // simply skips this attempt -- it may get a chance next pass.
            int bestStart = -1;
            int bestDistance = -1;

            for (int col = 0; col < columns; ++col)
            {
                // A bar's span NEVER wraps past the grid's end back to column
                // 0 -- the span is clamped at `columns` (NeditVST#9: the
                // original clamps too, jmin(columns, col + naturalSteps)).
                // Wrapping would make the claimed length extend past the
                // on-grid bar and let a later note land inside that wrapped
                // region, so the next random note wouldn't respect the
                // previous bar's length.
                const int spanEnd = std::min (columns, col + natural);

                bool free = true;
                for (int c = col; c < spanEnd; ++c)
                {
                    if (columnOccupied[static_cast<std::size_t> (c)])
                    {
                        free = false;
                        break;
                    }
                }

                if (! free)
                    continue;

                const int distance =
                    nearestOccupiedDistance (columnOccupied, col, natural, columns);
                if (distance > bestDistance)
                {
                    bestDistance = distance;
                    bestStart = col;
                }
            }

            if (bestStart < 0)
                continue;  // no free span for this row right now -- try next pass

            if (uniform01 (rng) >= density)
                continue;  // this attempt lost its density roll

            // Style from the sequencer's own weighted table; Forward on an
            // all-zero table (so Randomize never fails to place).
            const auto& weights = st.randomizeStyleWeights;
            const int styleOrdinal = pickWeightedIndex (
                rng, weights, static_cast<int> (state::PlaybackStyle::forward));

            const auto cell = static_cast<std::uint32_t> (
                static_cast<std::size_t> (row) * static_cast<std::size_t> (columns)
              + static_cast<std::size_t> (bestStart));
            st.grid[static_cast<std::size_t> (cell)] = static_cast<std::int8_t> (styleOrdinal);

            // Per-style "randomize parameters" opt-in: roll an independent
            // value for every parameter this style owns except the general
            // Subdivide and Volume (which apply to every style and whose
            // randomizing the original deliberately excluded too).
            if (styleOrdinal >= 0
                && styleOrdinal < state::kNumPlaybackStyles
                && st.randomizeParametersForStyle[static_cast<std::size_t> (styleOrdinal)])
            {
                const auto applicable = state::applicableStyleParams (
                    static_cast<state::PlaybackStyle> (styleOrdinal));

                for (int i = 0; i < applicable.count; ++i)
                {
                    const auto id = applicable.ids[static_cast<std::size_t> (i)];
                    if (id == state::StyleParamId::subdivide
                        || id == state::StyleParamId::volume)
                        continue;

                    const auto& info = state::styleParamInfo (id);

                    float value;
                    if (info.discrete)
                        value = static_cast<float> (
                            uniformInt (rng, std::max (1, info.numOptions)));
                    else
                        value = static_cast<float> (
                            info.minValue
                            + uniform01 (rng) * (info.maxValue - info.minValue));

                    st.overrides[cell][id] = value;

                    if (info.swept)
                    {
                        const auto modeId = static_cast<state::StyleParamId> (
                            static_cast<int> (id) + 1);
                        const auto& modeInfo = state::styleParamInfo (modeId);
                        st.overrides[cell][modeId] = static_cast<float> (
                            uniformInt (rng, std::max (1, modeInfo.numOptions)));
                    }
                }
            }

            // Claim this bar's whole span so nothing lands inside it. The
            // span is clamped at the grid's end (no wrap-around), matching
            // the free-check above -- a bar near the right edge occupies
            // [bestStart, columns) and no later note wraps in from column 0.
            const int claimEnd = std::min (columns, bestStart + natural);
            for (int c = bestStart; c < claimEnd; ++c)
                columnOccupied[static_cast<std::size_t> (c)] = true;

            ++result.cellsPlaced;
            placedAnythingThisPass = true;
        }

        if (! placedAnythingThisPass)
            break;

        ++result.passes;
    }

    return result;
}

} // namespace nedit::engine::seq
