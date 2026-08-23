#pragma once

// The VST3 shell glue: one class serving as IComponent + IEditController
// + IAudioProcessor (Steinberg's SingleComponentEffect), wiring the host
// onto the Phase 1 state and the Phase 2 engine:
//
//   host automation  -> ParameterSurface -> working state copy per block
//   UI edits         -> publish() on SnapshotProvider
//   audio thread     -> acquire() snapshot + VoiceScheduler + MidiDispatch
//   getState/setState-> nedit::state::serialize / deserialize
//
// The controller side owns `uiState_` as the authoritative live copy; the
// audio thread never mutates it -- it reads immutable snapshots, with
// parameter changes folded into a reusable scratch copy (steady-state
// allocation-free).

#include "engine/MidiDispatch.h"
#include "engine/Scheduler.h"
#include "engine/SnapshotProvider.h"
#include "plugin/ParameterSurface.h"
#include "state/PluginState.h"

#include "public.sdk/source/vst/vstsinglecomponenteffect.h"

#include <vector>

namespace nedit::plugin {

class NeditProcessor : public Steinberg::Vst::SingleComponentEffect
{
public:
    NeditProcessor();

    static Steinberg::FUnknown* PLUGIN_API createInstance (void*)
    {
        return static_cast<Steinberg::Vst::IAudioProcessor*> (new NeditProcessor);
    }

    //--- IPluginBase -------------------------------------------------------
    Steinberg::tresult PLUGIN_API initialize (Steinberg::FUnknown* context) override;
    Steinberg::tresult PLUGIN_API terminate() override;

    //--- IComponent --------------------------------------------------------
    Steinberg::tresult PLUGIN_API setActive (Steinberg::TBool state) override;
    Steinberg::tresult PLUGIN_API getState (Steinberg::IBStream* stream) override;
    Steinberg::tresult PLUGIN_API setState (Steinberg::IBStream* stream) override;

    //--- IAudioProcessor ---------------------------------------------------
    Steinberg::tresult PLUGIN_API setProcessing (Steinberg::TBool state) override;
    Steinberg::tresult PLUGIN_API process (Steinberg::Vst::ProcessData& data) override;

    //--- IEditController ----------------------------------------------------
    Steinberg::tresult PLUGIN_API setParamNormalized (
        Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue normNormalized) override;

    // Test/diagnostic hooks -- not part of any VST3 interface.
    [[nodiscard]] const state::PluginState& debugUiState() const noexcept { return uiState_; }
    [[nodiscard]] const engine::VoiceScheduler& debugScheduler() const noexcept { return scheduler_; }

private:
    void registerParameters();
    void syncParameterObjectsFromState();
    [[nodiscard]] int numAvailableSlices (const state::PluginState& s) const noexcept;

    // Controller-side authoritative live state.
    state::PluginState uiState_;

    // Audio-thread plumbing.
    engine::SnapshotProvider provider_;
    engine::VoiceScheduler scheduler_;
    engine::BlockContext ctx_ {};
    std::vector<engine::Slice> slices_;   // empty until sample analysis exists (Phase 4)
    state::PluginState automationScratch_;
    double lastBlockEndPpq_ = 0.0;

    NeditProcessor (const NeditProcessor&) = delete;
    NeditProcessor& operator= (const NeditProcessor&) = delete;
};

} // namespace nedit::plugin
