// Nedit -- State layer.
//
// Per-instance UI state that should survive editor close/reopen and
// session reload. Kept in the model deliberately: the original stored
// zoom/pan in the editor and re-zeroed it on every reopen, which caused a
// real SIGSEGV (docs/bugfix-editor-reopen-sigsegv.md in the original
// repo). Views must always initialize themselves FROM this state.

#pragma once

#include "Types.h"

namespace nedit::state {

enum class UiTab : std::uint8_t
{
    generate = 0,
    sequence,
    control,
    perform
};

// ---------------------------------------------------------------------------
// Tab <-> TriggerMode mapping (the tab and the scheduler-facing mode move in
// lockstep). TriggerMode is FLAT -- the two Generate sub-modes ARE the
// sliceLength/clock entries -- so the only many-to-one is the Generate tab,
// which resolves through generateMode. These are the single source of truth
// for both coupling directions (tab drives mode, mode drives tab); the
// processor setters and the parameter-surface fold both go through them.
[[nodiscard]] constexpr UiTab tabForTriggerMode (TriggerMode mode) noexcept
{
    switch (mode)
    {
        case TriggerMode::sliceLength:
        case TriggerMode::clock:       return UiTab::generate;
        case TriggerMode::sequenced:   return UiTab::sequence;
        case TriggerMode::performance: return UiTab::perform;
        case TriggerMode::control:     return UiTab::control;
    }
    return UiTab::generate;
}

[[nodiscard]] constexpr TriggerMode triggerModeForTab (UiTab tab,
                                                       TriggerMode generateMode) noexcept
{
    switch (tab)
    {
        case UiTab::generate:
            // The Generate tab hosts two modes; keep whichever sub-mode the
            // ribbon last chose (defaulting to sliceLength for anything odd).
            return generateMode == TriggerMode::clock ? TriggerMode::clock
                                                      : TriggerMode::sliceLength;
        case UiTab::sequence: return TriggerMode::sequenced;
        case UiTab::control:  return TriggerMode::control;
        case UiTab::perform:  return TriggerMode::performance;
    }
    return TriggerMode::sliceLength;
}

struct UiState
{
    UiTab activeTab = UiTab::generate;

    // Waveform zoom/pan, normalized to the sample length (0..1) so the
    // values stay meaningful independent of which sample is loaded.
    double visibleStartNorm = 0.0;
    double visibleEndNorm = 1.0;

    // Audition toggle: when true the scheduler produces audio.
    // Disabled by the editor when no sample is loaded.
    bool auditionEnabled = false;

    void sanitize() noexcept
    {
        // activeTab's underlying type is unsigned; only the upper bound
        // can be violated by hostile input.
        if (static_cast<int> (activeTab) > 3)
            activeTab = UiTab::generate;

        visibleStartNorm = clampValue (visibleStartNorm, 0.0, 1.0);
        visibleEndNorm = clampValue (visibleEndNorm, 0.0, 1.0);

        if (visibleEndNorm <= visibleStartNorm)
        {
            visibleStartNorm = 0.0;
            visibleEndNorm = 1.0;
        }
    }

    bool operator== (const UiState&) const = default;
};

} // namespace nedit::state
