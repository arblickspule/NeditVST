// Nedit -- UI layer.
//
// Pure per-mode enabled/grey mapping for the Generate timing option menus
// (RESET EVERY rides Slice Length; CLOCK REFERENCE + the Tape Stop / Filter
// sweep scopes ride Clock). Framework-free so the mode->grey contract is
// unit-testable without a widget tree.

#pragma once

#include "state/Types.h"

namespace nedit::ui {

struct TimingGreyState
{
    bool resetBarsGreyed = false;
    bool clockRefGreyed = false;
    bool tapeScopeGreyed = false;
    bool filterScopeGreyed = false;
};

[[nodiscard]] inline TimingGreyState timingGreyState (state::TriggerMode mode) noexcept
{
    const bool sl = (mode == state::TriggerMode::sliceLength);
    TimingGreyState g;
    g.resetBarsGreyed = ! sl;
    g.clockRefGreyed = sl;
    g.tapeScopeGreyed = sl;
    g.filterScopeGreyed = sl;
    return g;
}

} // namespace nedit::ui
