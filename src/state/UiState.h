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

struct UiState
{
    UiTab activeTab = UiTab::generate;

    // Waveform zoom/pan, normalized to the sample length (0..1) so the
    // values stay meaningful independent of which sample is loaded.
    double visibleStartNorm = 0.0;
    double visibleEndNorm = 1.0;

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
