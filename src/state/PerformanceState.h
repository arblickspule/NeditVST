// Nedit -- State layer.
//
// Performance mode: 128 note-indexed snapshots (segment trim + style +
// parameters + loop/sync flags), a live working state for the focused
// slot, and the trim-snap / quantized-recall settings.
//
// Fixes over the original:
//   * every snapshot -- INCLUDING the working state -- owns its trim.
//     The original repointed the global trim atomics at the focused
//     slot's segment (the most dangerous aliasing in that codebase,
//     which corrupted tempo derivation and required a duplicate
//     "tempoTrim" pair as a workaround). Here the global trim in
//     SampleState is never touched by Performance mode.
//   * the bank is serialized (the original never persisted it)

#pragma once

#include "StyleParameters.h"
#include "Types.h"

#include <array>

namespace nedit::state {

struct PerformanceSnapshot
{
    bool populated = false;

    // Segment trim in source-sample frames -- OWNED by the snapshot.
    std::int64_t trimStartFrame = 0;
    std::int64_t trimEndFrame = 0;

    int style = 0;  // playback style ordinal 0..8
    StyleParameters params;

    bool loop = false;  // loop vs one-shot
    bool sync = true;   // false = native-rate playback, ignoring pitch mode

    void sanitize (std::int64_t sampleLengthFrames) noexcept
    {
        style = clampValue (style, 0, kNumPlaybackStyles - 1);
        params.sanitize();

        if (sampleLengthFrames > 0)
        {
            trimStartFrame = clampValue<std::int64_t> (trimStartFrame, 0, sampleLengthFrames);
            trimEndFrame = clampValue<std::int64_t> (trimEndFrame, trimStartFrame, sampleLengthFrames);
        }
        else
        {
            trimStartFrame = 0;
            trimEndFrame = 0;
        }
    }

    bool operator== (const PerformanceSnapshot&) const = default;
};

struct PerformanceState
{
    // Per-MIDI-note snapshots.
    std::array<PerformanceSnapshot, kNumMidiNotes> bank {};

    // The live-editable state for the focused slot. Auto-saved into the
    // bank when focus moves. Owns its trim (see header comment).
    PerformanceSnapshot workingState;

    // Editing focus (-1 = none). Set only by on-screen keyboard clicks;
    // deliberately persists across mode changes.
    int focusedSlot = -1;

    // Trim-handle snapping while editing performance segments.
    TrimSnapMode trimSnapMode = TrimSnapMode::transients;
    int trimGridIndex = kNoteValue4n;

    // Deferred (quantized) snapshot recall.
    bool quantizeRecallEnabled = false;
    int quantizeRecallIntervalIndex = kNoteValue4n;

    void sanitize (std::int64_t sampleLengthFrames) noexcept
    {
        for (auto& snapshot : bank)
        {
            if (! snapshot.populated)
            {
                snapshot = PerformanceSnapshot {};
                continue;
            }

            snapshot.sanitize (sampleLengthFrames);
        }

        workingState.sanitize (sampleLengthFrames);

        focusedSlot = clampValue (focusedSlot, -1, kNumMidiNotes - 1);

        if (! isValidNoteValueIndex (trimGridIndex))
            trimGridIndex = kNoteValue4n;

        if (! isValidNoteValueIndex (quantizeRecallIntervalIndex))
            quantizeRecallIntervalIndex = kNoteValue4n;
    }

    bool operator== (const PerformanceState&) const = default;
};

} // namespace nedit::state
