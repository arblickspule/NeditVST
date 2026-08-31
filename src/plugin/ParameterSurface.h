#pragma once

// The DAW-visible automation surface: which PluginState fields exist as
// automatable VST3 parameters, and how normalized [0..1] values map onto
// them. Pure state code -- no SDK includes -- so it unit-tests offline.
//
// Surface decisions (Phase 3 v1, deliberate and documented):
// - Style parameters 0..20 target GenerateState.styleParams (the Generate
//   tab panel). The Control/Sequencer-fallback/Performance scopes stay
//   UI-edited state, not automation targets -- automating four parallel
//   copies of the same vocabulary invites exactly the silent-sharing bug
//   the rewrite killed. Revisit if a use case appears.
// - Everything else exposed: trigger mode, manual tempo (+enable), loop
//   length bars, control base note/gate mode, performance quantize recall
//   (+interval). Sequencer grids are structural, not automation material.
//
// Parameter IDs are part of the persisted-session contract (hosts store
// them in chunks/automations): NEVER renumber, only append.

#include "state/PluginState.h"
#include "state/StyleParameters.h"
#include "state/Types.h"

#include <cmath>
#include <cstdint>

namespace nedit::plugin {

enum : std::uint32_t
{
    // Style parameters occupy 0 .. 20 (matches StyleParamId).
    kLastStyleParamId = static_cast<std::uint32_t> (state::kNumStyleParams) - 1,

    kParamTriggerMode = 100,
    kParamManualTempoEnabled = 101,
    kParamManualTempoBpm = 102,
    kParamLoopLengthBars = 103,
    kParamControlBaseNote = 104,
    kParamControlGateMode = 105,
    kParamQuantizeRecallEnabled = 106,
    kParamQuantizeRecallInterval = 107,
};

[[nodiscard]] inline bool isValidParamId (std::uint32_t id) noexcept
{
    return id <= kLastStyleParamId
        || (id >= kParamTriggerMode && id <= kParamQuantizeRecallInterval);
}

// VST3 step count: 0 = continuous slider, N = N+1 discrete positions.
[[nodiscard]] inline int stepCountFor (std::uint32_t id) noexcept
{
    if (id <= kLastStyleParamId)
    {
        const auto& info = state::styleParamInfo (static_cast<state::StyleParamId> (id));
        return info.discrete ? info.numOptions - 1 : 0;
    }

    switch (id)
    {
        case kParamTriggerMode:             return static_cast<int> (state::kNumTriggerModes) - 1;
        case kParamManualTempoEnabled:      return 1;
        case kParamControlGateMode:         return 1;
        case kParamQuantizeRecallEnabled:   return 1;
        case kParamQuantizeRecallInterval:  return static_cast<int> (state::kNumNoteValues) - 1;
        default:                            return 0;   // continuous
    }
}

[[nodiscard]] inline const char* titleFor (std::uint32_t id) noexcept
{
    if (id <= kLastStyleParamId)
        return state::styleParamInfo (static_cast<state::StyleParamId> (id)).name;

    switch (id)
    {
        case kParamTriggerMode:            return "Trigger Mode";
        case kParamManualTempoEnabled:     return "Manual Tempo";
        case kParamManualTempoBpm:         return "Tempo BPM";
        case kParamLoopLengthBars:         return "Loop Length Bars";
        case kParamControlBaseNote:        return "Base Note";
        case kParamControlGateMode:        return "Gate Mode";
        case kParamQuantizeRecallEnabled:  return "Quantize Recall";
        case kParamQuantizeRecallInterval: return "Recall Interval";
        default:                           return nullptr;
    }
}

[[nodiscard]] constexpr float clamp01 (float v) noexcept
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

[[nodiscard]] inline float toNormalized (const state::PluginState& s,
                                         std::uint32_t id) noexcept
{
    if (id <= kLastStyleParamId)
    {
        const auto paramId = static_cast<state::StyleParamId> (id);
        const auto& info = state::styleParamInfo (paramId);
        const float value = s.generate.styleParams.get (paramId);

        if (info.discrete)
            return clamp01 (std::round (value) / static_cast<float> (info.numOptions - 1));

        const float span = info.maxValue - info.minValue;
        return clamp01 ((value - info.minValue) / span);
    }

    switch (id)
    {
        case kParamTriggerMode:
            return clamp01 (static_cast<float> (s.triggerMode)
                            / static_cast<float> (state::kNumTriggerModes - 1));
        case kParamManualTempoEnabled:
            return s.sample.manualBpmOverrideEnabled ? 1.0f : 0.0f;
        case kParamManualTempoBpm:
        {
            constexpr float kMinBpm = 30.0f;
            constexpr float kMaxBpm = 300.0f;
            const float bpm = static_cast<float> (s.sample.manualBpmOverrideValue);
            return clamp01 ((bpm - kMinBpm) / (kMaxBpm - kMinBpm));
        }
        case kParamLoopLengthBars:
        {
            constexpr int kMaxBars = 16;
            const int bars = s.sample.loopLengthBars < 1 ? 1 : s.sample.loopLengthBars;
            return clamp01 (static_cast<float> (bars - 1) / static_cast<float> (kMaxBars - 1));
        }
        case kParamControlBaseNote:
            return clamp01 (static_cast<float> (s.control.baseNote)
                            / static_cast<float> (state::kNumMidiNotes - 1));
        case kParamControlGateMode:
            return s.control.gateMode ? 1.0f : 0.0f;
        case kParamQuantizeRecallEnabled:
            return s.performance.quantizeRecallEnabled ? 1.0f : 0.0f;
        case kParamQuantizeRecallInterval:
            return clamp01 (static_cast<float> (s.performance.quantizeRecallIntervalIndex)
                            / static_cast<float> (state::kNumNoteValues - 1));
        default:
            return 0.0f;
    }
}

// Writes through to the given state, clamped/routed exactly like the UI
// mutators would (discrete values land rounded -- StyleParameters::set
// owns the final clamping for style params).
inline void applyNormalized (state::PluginState& s, std::uint32_t id,
                             float normalized) noexcept
{
    normalized = clamp01 (normalized);

    if (id <= kLastStyleParamId)
    {
        const auto paramId = static_cast<state::StyleParamId> (id);
        const auto& info = state::styleParamInfo (paramId);

        float value;

        if (info.discrete)
            value = static_cast<float> (
                static_cast<int> (std::lround (normalized * static_cast<float> (info.numOptions - 1))));
        else
            value = info.minValue
                  + normalized * (info.maxValue - info.minValue);

        s.generate.styleParams.set (paramId, value);
        return;
    }

    switch (id)
    {
        case kParamTriggerMode:
        {
            const auto idx = static_cast<int> (
                std::lround (normalized * static_cast<float> (state::kNumTriggerModes - 1)));
            const auto tm = static_cast<state::TriggerMode> (
                idx < 0 ? 0 : (idx >= state::kNumTriggerModes ? state::kNumTriggerModes - 1 : idx));
            s.triggerMode = tm;
            // The two Generate sub-modes share the sliceLength/clock entries
            // of the top-level trigger mode (mirror of
            // NeditProcessor::setGenerateMode), so the top-bar menu and the
            // Generate ribbon keep all their sync'd views of the same mode.
            if (tm == state::TriggerMode::sliceLength || tm == state::TriggerMode::clock)
                s.generate.generateMode = tm;
            // Tab and mode move in lockstep: a host automating the trigger
            // mode moves the visible tab (the editor's syncTabBar picks this
            // up from the published state). Generate's two sub-modes both map
            // to the Generate tab, so this never fights the ribbon.
            s.ui.activeTab = state::tabForTriggerMode (tm);
            break;
        }
        case kParamManualTempoEnabled:
            s.sample.manualBpmOverrideEnabled = normalized >= 0.5f;
            break;
        case kParamManualTempoBpm:
        {
            constexpr float kMinBpm = 30.0f;
            constexpr float kMaxBpm = 300.0f;
            s.sample.manualBpmOverrideValue = static_cast<double> (
                kMinBpm + normalized * (kMaxBpm - kMinBpm));
            break;
        }
        case kParamLoopLengthBars:
        {
            constexpr int kMaxBars = 16;
            auto bars = static_cast<int> (
                std::lround (1.0f + normalized * static_cast<float> (kMaxBars - 1)));
            if (bars < 1)
                bars = 1;
            if (bars > kMaxBars)
                bars = kMaxBars;
            s.sample.loopLengthBars = bars;   // engine owns grid non-reset (AGENTS pitfall #7)
            break;
        }
        case kParamControlBaseNote:
        {
            auto note = static_cast<int> (
                std::lround (normalized * static_cast<float> (state::kNumMidiNotes - 1)));
            if (note < 0)
                note = 0;
            if (note > state::kNumMidiNotes - 1)
                note = state::kNumMidiNotes - 1;
            s.control.baseNote = note;
            break;
        }
        case kParamControlGateMode:
            s.control.gateMode = normalized >= 0.5f;
            break;
        case kParamQuantizeRecallEnabled:
            s.performance.quantizeRecallEnabled = normalized >= 0.5f;
            break;
        case kParamQuantizeRecallInterval:
        {
            auto idx = static_cast<int> (
                std::lround (normalized * static_cast<float> (state::kNumNoteValues - 1)));
            if (idx < 0)
                idx = 0;
            if (idx > state::kNumNoteValues - 1)
                idx = state::kNumNoteValues - 1;
            s.performance.quantizeRecallIntervalIndex = idx;
            break;
        }
        default:
            break;
    }
}

[[nodiscard]] inline float defaultNormalized (std::uint32_t id) noexcept
{
    return toNormalized (state::PluginState {}, id);
}

} // namespace nedit::plugin
