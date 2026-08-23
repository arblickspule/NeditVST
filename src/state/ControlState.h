// Nedit -- State layer.
//
// Control mode: Simpler-style chromatic slice triggering with keyswitch
// style selection. Keyswitch mapping is arithmetic (baseNote - 1 -
// styleIndex), so nothing beyond the base note is stored for it.
//
// Fix over the original: Control owns its own StyleParameters copy
// instead of silently reading the shared globals.

#pragma once

#include "StyleParameters.h"
#include "Types.h"

namespace nedit::state {

struct ControlState
{
    int baseNote = kDefaultControlBaseNote;  // slices ascend chromatically, capped at 32
    bool gateMode = false;                   // trigger vs gate
    int activeStyle = 0;                     // last keyswitch-selected style ordinal

    // Control mode's own style parameter values.
    StyleParameters styleParams;

    void sanitize() noexcept
    {
        baseNote = clampValue (baseNote, 0, kNumMidiNotes - 1);
        activeStyle = clampValue (activeStyle, 0, kNumPlaybackStyles - 1);
        styleParams.sanitize();
    }

    bool operator== (const ControlState&) const = default;
};

} // namespace nedit::state
