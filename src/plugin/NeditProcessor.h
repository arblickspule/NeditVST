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

#include "SampleManager.h"
#include "engine/MidiDispatch.h"
#include "engine/Scheduler.h"
#include "engine/SnapshotProvider.h"
#include "plugin/ParameterSurface.h"
#include "state/PluginState.h"

#include "public.sdk/source/vst/vstsinglecomponenteffect.h"

#include <array>
#include <string>
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

    //--- UI-side entry points (called from the editor, UI thread) ----------
    // Decodes + analyzes the file and swaps it into the audio slot on
    // success. Returns false (state untouched) on failure.
    bool requestSampleLoad (const std::string& path);

    // Editor read access. The editor is a stateless renderer over these.
    [[nodiscard]] std::shared_ptr<const LoadedSample> acquireLoadedSample() const noexcept
    {
        return sampleManager_.acquire();
    }
    [[nodiscard]] const state::PluginState& uiStateView() const noexcept { return uiState_; }

    // Waveform zoom/pan writes (UiState owns view state; pitfall #6).
    void setVisibleWindow (double startNorm, double endNorm);

    // Audition toggle — gates whether the scheduler produces audio.
    void setAuditionEnabled (bool enabled);

    // Sample presence query (editor uses this to disable the audition
    // button when nothing is loaded).
    [[nodiscard]] bool hasSample() const noexcept
    {
        return sampleManager_.acquire() != nullptr;
    }

    //--- IEditController: editor creation -----------------------------------
    Steinberg::IPlugView* PLUGIN_API createView (Steinberg::FIDString name) override;

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
    [[nodiscard]] int debugSliceCount() const noexcept
    {
        const auto loaded = sampleManager_.acquire();
        return loaded != nullptr ? static_cast<int> (loaded->slices.size()) : 0;
    }

private:
    void registerParameters();
    void syncParameterObjectsFromState();
    void renderAudition (float* const* outAdd, int numOutChannels,
                         int numSamples, double hostSampleRate);

    // Controller-side authoritative live state.
    state::PluginState uiState_;

    // Audio-thread plumbing.
    engine::SnapshotProvider provider_;
    engine::VoiceScheduler scheduler_;
    engine::BlockContext ctx_ {};
    SampleManager sampleManager_;

    static constexpr int kMaxSourceChannels = 16;
    std::array<const float*, kMaxSourceChannels> sourceChannelPointers_ {};
    state::PluginState automationScratch_;
    double lastBlockEndPpq_ = 0.0;
    double auditionPosition_ = 0.0;  // read cursor for raw audition loop

    NeditProcessor (const NeditProcessor&) = delete;
    NeditProcessor& operator= (const NeditProcessor&) = delete;
};

} // namespace nedit::plugin
