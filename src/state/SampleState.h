// Nedit -- State layer.
//
// Sample document state: which file is loaded, the global trim, and the
// slicing *inputs* (detection sensitivity, grid quantize, manual and
// excluded points). The slice boundaries themselves are DERIVED state --
// the engine recomputes them deterministically from this struct plus the
// audio -- and are never serialized.
//
// Fixes over the original:
//   * full sample path is stored (the original kept only the file name,
//     making restore impossible)
//   * the trim here is the ONLY global trim; Performance-mode segments own
//     their trim in PerformanceSnapshot (the original repointed these very
//     fields at the focused performance slot, corrupting tempo derivation
//     and requiring a duplicate "tempoTrim" pair as a workaround)

#pragma once

#include "Types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace nedit::state {

// A user-placed (manual) or user-removed (excluded) slice boundary.
// Positions are in source-sample frames. Ids are stable within one
// document lifetime so undo and UI can track points across edits.
struct SamplePoint
{
    std::int32_t id = 0;
    std::int64_t position = 0;

    bool operator== (const SamplePoint&) const = default;
};

struct SampleState
{
    // --- sample reference -------------------------------------------------
    std::string samplePath;          // full path; empty = no sample loaded
    std::uint64_t sampleContentHash = 0;  // optional integrity check on restore
    std::int64_t sampleLengthFrames = 0;  // as last loaded (validation aid)
    double sampleSampleRate = 0.0;        // source file rate as last loaded

    // --- global trim (source-sample frames) --------------------------------
    // 0 <= trimStart < trimEnd <= sampleLengthFrames, minimum gap 64 frames.
    std::int64_t trimStartFrame = 0;
    std::int64_t trimEndFrame = 0;

    // --- transient detection inputs ----------------------------------------
    float sensitivity = 0.5f;             // 0..1
    bool quantizeTransients = false;      // snap auto onsets (never manual) to grid
    int quantizeGridIndex = kNoteValue4n; // note-value palette index

    // --- user slice edits ---------------------------------------------------
    std::vector<SamplePoint> manualPoints;    // user-added boundaries
    std::vector<SamplePoint> excludedPoints;  // suppressed auto-detected onsets
    std::int32_t nextManualPointId = 1;
    std::int32_t nextExcludedPointId = 1;

    // --- tempo --------------------------------------------------------------
    int loopLengthBars = 1;               // bars the trimmed span represents (4/4)
    bool manualBpmOverrideEnabled = false;
    double manualBpmOverrideValue = 120.0;

    static constexpr std::int64_t kMinTrimGapFrames = 64;

    [[nodiscard]] bool hasSample() const noexcept { return ! samplePath.empty(); }

    void sanitize() noexcept
    {
        sensitivity = clampValue (sensitivity, 0.0f, 1.0f);

        if (! isValidNoteValueIndex (quantizeGridIndex))
            quantizeGridIndex = kNoteValue4n;

        if (loopLengthBars < 1)
            loopLengthBars = 1;

        if (manualBpmOverrideValue < 1.0)
            manualBpmOverrideValue = 1.0;

        if (sampleLengthFrames < 0)
            sampleLengthFrames = 0;

        if (sampleSampleRate < 0.0)
            sampleSampleRate = 0.0;

        // Trim: clamp into the sample, keep ordering and the minimum gap.
        if (sampleLengthFrames > 0)
        {
            trimStartFrame = clampValue<std::int64_t> (trimStartFrame, 0, sampleLengthFrames);
            trimEndFrame = clampValue<std::int64_t> (trimEndFrame, 0, sampleLengthFrames);

            if (trimEndFrame - trimStartFrame < kMinTrimGapFrames)
            {
                trimEndFrame = trimStartFrame + kMinTrimGapFrames;

                if (trimEndFrame > sampleLengthFrames)
                {
                    trimEndFrame = sampleLengthFrames;
                    trimStartFrame = clampValue<std::int64_t> (trimEndFrame - kMinTrimGapFrames,
                                                               0, sampleLengthFrames);
                }
            }
        }
        else
        {
            trimStartFrame = 0;
            trimEndFrame = 0;
        }

        if (nextManualPointId < 1)
            nextManualPointId = 1;

        if (nextExcludedPointId < 1)
            nextExcludedPointId = 1;
    }

    bool operator== (const SampleState&) const = default;
};

} // namespace nedit::state
