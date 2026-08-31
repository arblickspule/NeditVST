// Nedit -- Engine layer.
//
// The Sequencer "Randomize Sequence" generator. Pure function of a
// SequencerState + the derived slice list; mutates the working grid in
// place. Deliberately NOT on the audio thread -- Randomize is a UI-triggered
// structural edit, like building slices or editing trim, so it is run under
// the editor's own state ownership and published afterwards.
//
// Faithful to the original's design while fixing its weak spots (see below):
//
//   * clear-then-rebuild, monophonic grid (one filled row per column)
//   * natural length per slice = its source duration quantized to the step
//     resolution at the source tempo -- bars never fragment a longer slice
//   * fair round-robin placement: repeated passes, each pass offers every
//     row exactly one placement attempt in a freshly reshuffled order, so
//     no row (or grid position) is systematically favoured
//   * style drawn from the sequencer's OWN weight table (SequencerState::
//     randomizeStyleWeights, decoupled from Generate's), so lowering a
//     style's weight also makes Randomize reach for it less
//   * per-style "randomize parameters" opt-in rolls an independent value
//     for every parameter that style owns, minus the general Subdivide and
//     Volume
//
// Improvements over the original (licence to adapt):
//   * seeded, deterministic RNG -- the original held an unseedable member
//     Random, which made the output untestable. A caller-chosen seed makes
//     grids reproducible.
//   * configurable density: placementProbability (default 0.5) replaces the
//     original's hard-coded 0.35 per-attempt roll.
//   * spacing-aware span scan: instead of "first free span from a random
//     start" (which clusters bars into the first gap it finds), each row's
//     attempt evaluates every free span and picks the one that maximizes
//     distance to the nearest already-placed cell -- placements spread
//     across the grid instead of clumping.
//   * safe all-zero weights: if every weight is 0 the draw falls back to
//     Forward instead of failing to place anything.
//   * returns placement statistics so callers (UI) can report and tests can
//     assert.

#pragma once

#include <state/SequencerState.h>

#include "Slice.h"

#include <cstdint>
#include <span>

namespace nedit::engine::seq {

// Outcome of a Randomize call.
struct RandomizeResult
{
    int cellsPlaced = 0;  // how many cells ended the run filled
    int passes = 0;       // round-robin passes actually run
};

// Derived working-grid dimensions.
struct SequencerDims
{
    int rows = 0;
    int columns = 0;

    bool operator== (const SequencerDims&) const = default;
};

// Derive the working grid's dimensions from the sample + pattern settings:
//   rows    = min(sliceCount, kMaxSequencerRows)  -- one row per slice
//   columns = stepsPerBar(stepResolutionIndex) * patternLengthBars,
//             capped at kMaxSequencerColumns
// where stepsPerBar = round(4 beats / step-note-value-beats). Pure so the
// dimension contract is unit-testable without the shell.
[[nodiscard]] SequencerDims computeSequencerDims (int sliceCount,
                                                  int stepResolutionIndex,
                                                  int patternLengthBarsIndex) noexcept;

// Set state.rows/columns to `dims`; when they DIFFER from the current
// dimensions the working grid (cells + overrides + extensions) is reset via
// clearGrid -- the documented "grid resets when its own dimensions change"
// contract (SequencerState.h). Returns whether the dimensions changed.
// The pattern bank is untouched.
bool resizeGrid (state::SequencerState& state, SequencerDims dims) noexcept;

// Default per-attempt placement density. 0.5 is denser (and more useful
// out of the box) than the original's 0.35, but every call can override.
inline constexpr float kDefaultPlacementProbability = 0.5f;

// Wipe the working grid and every per-cell override/extension, leaving the
// dimensions (rows/columns) and the fallback params untouched. Shared by
// "Clear Sequence" and the Randomize entry (which clears before rebuilding).
// Patterns already stored in the bank are untouched.
void clearGrid (state::SequencerState& state) noexcept;

// Populates state.grid (monophonic) from the derived slices.
//
//   state  : in / out. rows/columns/stepResolutionIndex are read first;
//            grid + overrides + extensions are cleared and rebuilt.
//   slices : the derived slice list; only the first state.rows are used
//            (min(state.rows, slices.size())).
//   sampleSampleRate, originalBpm : source-tempo inputs for natural length.
//   seed   : RNG seed (deterministic).
//   placementProbability : (0..1] per-attempt placement chance.
[[nodiscard]] RandomizeResult randomizeSequence (
    state::SequencerState& st,
    std::span<const Slice> slices,
    double sampleSampleRate,
    double originalBpm,
    std::uint32_t seed,
    float placementProbability = kDefaultPlacementProbability) noexcept;

} // namespace nedit::engine::seq
