// Nedit -- State layer.
//
// The complete serializable plugin state. Engine and UI both operate on
// this model; nothing here depends on any SDK or framework.
//
// NOT in this struct (by design):
//   * derived state (slice boundaries, waveform peaks) -- recomputed
//   * runtime state (scheduler positions, current pick, DSP scratch
//     buffers, MIDI-learn arming, pending recalls) -- engine-owned,
//     never serialized

#pragma once

#include "ControlState.h"
#include "GenerateState.h"
#include "PerformanceState.h"
#include "RenderState.h"
#include "SampleState.h"
#include "SequencerState.h"
#include "UiState.h"
#include "Types.h"

namespace nedit::state {

struct PluginState
{
    TriggerMode triggerMode = TriggerMode::sliceLength;

    SampleState sample;
    RenderState render;
    GenerateState generate;
    SequencerState sequencer;
    PerformanceState performance;
    ControlState control;
    UiState ui;

    // Clamp everything to documented ranges. Called automatically after
    // deserialization; safe to call any time.
    void sanitize() noexcept
    {
        // triggerMode's underlying type is unsigned; only the upper bound
        // can be violated by hostile input.
        if (static_cast<int> (triggerMode) >= kNumTriggerModes)
            triggerMode = TriggerMode::sliceLength;

        sample.sanitize();
        render.sanitize();
        generate.sanitize();
        sequencer.sanitize();
        performance.sanitize (sample.sampleLengthFrames);
        control.sanitize();
        ui.sanitize();
    }

    bool operator== (const PluginState&) const = default;
};

} // namespace nedit::state
