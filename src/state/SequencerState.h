// Nedit -- State layer.
//
// Sequenced mode: the monophonic step grid, per-step parameter overrides
// and length extensions, the randomizer's own settings, and the 128-slot
// MIDI-recallable pattern bank.
//
// Fixes over the original:
//   * per-step overrides are keyed by StyleParamId (the original used
//     parameter NAME strings)
//   * the randomizer owns its own style-weight table instead of reading
//     Generate's
//   * fallbackParams: steps without an override read these, owned by the
//     sequencer (the original silently read the shared globals)
//   * the pattern bank IS serialized (the original never persisted it)
//   * loop-length edits must NOT reset the grid (original stale coupling);
//     the grid resets only when its own dimensions change
//     (stepResolutionIndex / patternLengthBarsIndex) or the slice list is
//     rebuilt -- enforced by the engine, documented here.

#pragma once

#include "StyleParameters.h"
#include "Types.h"

#include <array>
#include <cstdint>
#include <map>
#include <vector>

namespace nedit::state {

// One saved pattern. Dimensions are captured with the grid because they
// define the flat-index stride.
struct SequencerPattern
{
    bool populated = false;
    int rows = 0;
    int columns = 0;
    int stepResolutionIndex = kNoteValue16n;
    int patternLengthBarsIndex = kDefaultPatternLengthBarsIndex;

    // Flat row-major grid, rows * columns entries. -1 = empty, otherwise a
    // playback style ordinal (0..8). Monophonic: at most one filled row
    // per column (enforced at write time by the state mutators/engine).
    std::vector<std::int8_t> grid;

    // Sparse per-step parameter overrides: flat cell index -> (param -> value).
    std::map<std::uint32_t, std::map<StyleParamId, float>> overrides;

    // Sparse per-step length extension in steps (absent = natural length).
    std::map<std::uint32_t, std::uint16_t> extensions;

    bool operator== (const SequencerPattern&) const = default;
};

struct SequencerState
{
    // --- grid dimensions -----------------------------------------------------
    int stepResolutionIndex = kNoteValue16n;                    // column stride
    int patternLengthBarsIndex = kDefaultPatternLengthBarsIndex;

    // --- the working pattern ---------------------------------------------------
    // rows = min(kMaxSequencerRows, sliceCount); columns derive from
    // resolution x pattern length, capped at kMaxSequencerColumns. Stored
    // explicitly so the state is self-describing.
    int rows = 0;
    int columns = 0;
    std::vector<std::int8_t> grid;                              // rows * columns, -1 empty
    std::map<std::uint32_t, std::map<StyleParamId, float>> overrides;
    std::map<std::uint32_t, std::uint16_t> extensions;

    // Steps without an override read these (sequencer-owned fallback).
    StyleParameters fallbackParams;

    // --- randomizer -------------------------------------------------------------
    // The sequencer randomizer's OWN style weights (decoupled from Generate).
    std::array<float, kNumPlaybackStyles> randomizeStyleWeights { { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                                                    0.0f, 0.0f, 0.0f, 0.0f } };

    // Per-style opt-in: Randomize also rolls that style's parameter values.
    std::array<bool, kNumPlaybackStyles> randomizeParametersForStyle {};

    // --- UI-adjacent but model-owned ---------------------------------------------
    int selectedDrawingStyle = 0;  // style ordinal the palette paints with

    // --- pattern bank ---------------------------------------------------------------
    std::array<SequencerPattern, kNumMidiNotes> patternBank {};
    PatternSwitchTiming patternSwitchTiming = PatternSwitchTiming::immediate;
    int patternSwitchIntervalIndex = kNoteValue1n;

    [[nodiscard]] std::uint32_t cellIndex (int row, int column) const noexcept
    {
        return static_cast<std::uint32_t> (row) * static_cast<std::uint32_t> (columns)
             + static_cast<std::uint32_t> (column);
    }

    void sanitize() noexcept
    {
        if (! isValidNoteValueIndex (stepResolutionIndex))
            stepResolutionIndex = kNoteValue16n;

        patternLengthBarsIndex = clampValue (patternLengthBarsIndex, 0,
                                             static_cast<int> (kPatternLengthBarsValues.size()) - 1);

        rows = clampValue (rows, 0, kMaxSequencerRows);
        columns = clampValue (columns, 0, kMaxSequencerColumns);

        sanitizeGrid (grid, overrides, extensions, rows, columns);

        fallbackParams.sanitize();

        for (auto& w : randomizeStyleWeights)
            w = clampValue (w, 0.0f, 1.0f);

        selectedDrawingStyle = clampValue (selectedDrawingStyle, 0, kNumPlaybackStyles - 1);

        for (auto& pattern : patternBank)
        {
            if (! pattern.populated)
            {
                pattern = SequencerPattern {};
                continue;
            }

            if (! isValidNoteValueIndex (pattern.stepResolutionIndex))
                pattern.stepResolutionIndex = kNoteValue16n;

            pattern.patternLengthBarsIndex =
                clampValue (pattern.patternLengthBarsIndex, 0,
                            static_cast<int> (kPatternLengthBarsValues.size()) - 1);

            pattern.rows = clampValue (pattern.rows, 0, kMaxSequencerRows);
            pattern.columns = clampValue (pattern.columns, 0, kMaxSequencerColumns);

            sanitizeGrid (pattern.grid, pattern.overrides, pattern.extensions,
                          pattern.rows, pattern.columns);
        }

        if (! isValidNoteValueIndex (patternSwitchIntervalIndex))
            patternSwitchIntervalIndex = kNoteValue1n;
    }

    bool operator== (const SequencerState&) const = default;

private:
    static void sanitizeGrid (std::vector<std::int8_t>& gridData,
                              std::map<std::uint32_t, std::map<StyleParamId, float>>& overrideData,
                              std::map<std::uint32_t, std::uint16_t>& extensionData,
                              int rowCount, int columnCount) noexcept
    {
        const auto expected = static_cast<std::size_t> (rowCount)
                            * static_cast<std::size_t> (columnCount);
        gridData.resize (expected, static_cast<std::int8_t> (-1));

        for (auto& cell : gridData)
            if (cell < -1 || cell >= kNumPlaybackStyles)
                cell = -1;

        // Drop overrides/extensions pointing outside the grid.
        std::erase_if (overrideData, [expected] (const auto& entry)
                       { return entry.first >= expected; });
        std::erase_if (extensionData, [expected] (const auto& entry)
                       { return entry.first >= expected; });

        // Clamp override values to their parameter's range.
        for (auto& [cell, params] : overrideData)
        {
            (void) cell;

            for (auto& [id, value] : params)
            {
                const auto& info = styleParamInfo (id);
                value = clampValue (value, info.minValue, info.maxValue);
            }
        }
    }
};

} // namespace nedit::state
