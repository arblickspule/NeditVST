#pragma once

// MIDI note routing by trigger mode -- the single place that maps host
// MIDI events onto the scheduler's per-mode entry points. Pure and
// synchronous: it runs on the audio thread inside process(), exactly where
// the original routed its note handling.
//
// Mode semantics (mirroring the original):
// - Performance:  note-on recalls a bank snapshot; note-offs carry no
//   meaning (loop/silence are handled inside the scheduler).
// - Control:      note-ons trigger, note-offs release in gate mode.
// - Sequenced:    note-ons recall pattern-bank slots; note-offs ignored.
// - Slice Length / Clock: no MIDI semantics.
//
// `numAvailableSlices` is the caller's capped slice count
// (min(kMaxControlSlices?, derived slices)); the dispatcher does not know
// about the derived slice list.

#include "engine/Scheduler.h"
#include "state/Types.h"

#include <algorithm>
#include <cstdint>

namespace nedit::engine
{

[[nodiscard]] inline float velocityFromMidiByte (std::uint8_t byte) noexcept
{
    return static_cast<float> (byte) / 127.0f;
}

inline void routeMidiNote (VoiceScheduler& scheduler,
                           const state::PluginState& state,
                           int noteNumber,
                           float velocity01,
                           bool noteOn,
                           bool hostTransportPlaying,
                           int numAvailableSlices)
{
    const float clampedVelocity = std::clamp (velocity01, 0.0f, 1.0f);

    switch (state.triggerMode)
    {
        case state::TriggerMode::performance:
            if (noteOn)
                scheduler.requestPerformanceRecall (state, noteNumber,
                                                    hostTransportPlaying);
            break;

        case state::TriggerMode::control:
            if (noteOn)
                scheduler.controlNoteOn (noteNumber, clampedVelocity,
                                         state.control.baseNote,
                                         numAvailableSlices);
            else
                scheduler.controlNoteOff (noteNumber, state.control.gateMode);
            break;

        case state::TriggerMode::sequenced:
            if (noteOn)
                scheduler.requestPatternSwitch (noteNumber);
            break;

        default:
            break;
    }
}

} // namespace nedit::engine
