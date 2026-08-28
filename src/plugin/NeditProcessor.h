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

// Clip a derived slice list to the CURRENT trim, remapping per-slice weights
// in lockstep so `outSlices`/`outWeights` stay parallel to the input. The
// view keeps a SOFT trim (slices outside are hidden, never rebuilt), and the
// audio thread must apply the same rule to what it PLAYS -- this is the
// engine-side mirror of that: wholly-outside slices are dropped, a slice
// straddling a trim handle is cut to the trim, and weights are copied
// positionally. Pure + allocation-free on pre-reserved outputs.
void clipSlicesToTrim (const std::vector<engine::Slice>& slices,
                       std::int64_t trimStart, std::int64_t trimEnd,
                       const std::vector<float>& sliceWeights,
                       std::vector<engine::Slice>& outSlices,
                       std::vector<float>& outWeights);

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

    // Performance-page tab (UiState.activeTab). Publish only; the panel
    // below the tab bar re-renders from state.
    void setActiveTab (state::UiTab tab);

    // Per-style draw weight (GenerateState.styleWeights[i]). Publish only;
    // clamps to [0,1], ignores out-of-range style indices.
    void setStyleWeight (int styleIndex, float weight);

    // Trim: direct frame-based write from the waveform view's trim handles.
    void setTrimFrames (std::int64_t startFrame, std::int64_t endFrame);

    // Per-slice probability weight (UI concept: 0 = never picked,
    // 1 = full weight). Stored in generate.sliceWeights.
    [[nodiscard]] float getSliceProbability (int sliceIndex) const;
    void setSliceProbability (int sliceIndex, float weight);

    // Manual slice markers (double-click on the waveform). addManualPoint
    // clamps to the trim and, when snap is true, snaps to the nearest
    // transient (raw derivative peak within ~50ms -- the original's
    // findNearestPeak, NOT a re-detect). moveManualPoint repositions an
    // existing marker (used for click-drag; snap applied on release). All
    // three rebuild the slice list from the retained analysis and preserve
    // painted probabilities by mapping weights across the rebuild by slice
    // start-frame. Returns the new point's id (addManualPoint) / true if a
    // point was removed.
    std::int32_t addManualPoint (std::int64_t frame, bool snap);
    void moveManualPoint (std::int32_t id, std::int64_t frame, bool snap);
    bool removeManualPoint (std::int32_t id);
    // Suppress the auto-detected onset nearest to `frame` (double-click on an
    // auto marker). Re-runs detection at the CURRENT sensitivity+holdoff to
    // find the nearest raw onset != trim start -- the same approach as the
    // original's excludeNearestAutoPoint -- and records it in
    // sample.excludedPoints, then rebuilds weights-preserving. The trim-start
    // boundary is never excludable. Returns false when nothing was found
    // (or it was already excluded), leaving no rebuild behind.
    bool excludeNearestAutoPoint (std::int64_t frame);

    // Detection sensitivity (toolbar slider): stores sample.sensitivity and
    // re-runs detection + slicing at the current trim/holdoff, preserving
    // painted probabilities. Range [0,1]; no-op without a sample.
    void setSensitivity (float value);

    // Grid-quantize auto onsets (toolbar toggle): stores
    // sample.quantizeTransients and re-derives the slice list from the
    // retained analysis, preserving painted probabilities. Manual points
    // are never quantized (see SampleState.quantizeTransients). No-op
    // without a sample.
    void setQuantizeTransients (bool on);

    // Grid note-value for the transient quantize (toolbar dropdown): stores
    // sample.quantizeGridIndex (a kNoteValues palette index) and re-runs
    // slicing at the current trim/holdoff, preserving painted probabilities.
    // No-op without a sample (the grid only matters once quantize is on).
    void setQuantizeGrid (int gridIndex);

    // Per-pick declick fades (toolbar sliders): stores render.fadeInMs /
    // render.fadeOutMs and republishes. Range [0, 10] ms; the engine clamps
    // each fade to half the pick length.
    void setFadeInMs (float ms);
    void setFadeOutMs (float ms);

    // Repitch vs granular time-stretch (toolbar toggle): stores
    // render.pitchMode and republishes. Rendering-only -- no slice rebuild;
    // NEW picks get the new mode.
    void setPitchMode (state::PitchMode mode);

    // Time-Stretch granular character (toolbar sliders, enabled only while
    // pitchMode == timeStretch): stores and republishes. Rendering-only;
    // NEW picks get the new values.
    void setGrainSizeMs (float ms);
    void setGrainSpeed (float speed);

    // Audition toggle — gates whether the scheduler produces audio.
    void setAuditionEnabled (bool enabled);

    // Slice audition — loops a specific slice region. Called on RMB
    // down, cleared on RMB up. Bypasses the scheduler entirely.
    void startSliceAudition (int64_t startFrame, int64_t endFrame);
    void stopSliceAudition();

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
    // Rebuild the slice list from the retained analysis after a marker/trim
    // edit, remapping generate.sliceWeights across the rebuild by slice
    // start-frame so painted probabilities survive (unchanged boundaries
    // keep their value; a split inherits its parent slice's value). Then
    // publishes the new state snapshot.
    void rebuildSlicesPreservingWeights();
    // Clamp a requested marker frame into the trim and, when snap is true,
    // pull it to the nearest transient (shared by add + move).
    [[nodiscard]] std::int64_t resolveManualFrame (std::int64_t frame, bool snap) const;
    void renderAudition (float* const* outAdd, int numOutChannels,
                         int numSamples, double hostSampleRate);
    void renderSliceAudition (float* const* outAdd, int numOutChannels,
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

    // Audio-thread scratch: the slice list clipped to the CURRENT soft trim
    // with weights remapped in lockstep (see process()). Pre-reserved in the
    // ctor so steady-state blocks never allocate.
    std::vector<engine::Slice> trimSlices_;
    std::vector<float> trimWeights_;

    // Slice audition state (RMB hold-to-loop).
    bool sliceAuditionActive_ = false;
    int64_t sliceAuditionStart_ = 0;
    int64_t sliceAuditionEnd_ = 0;
    double sliceAuditionPosition_ = 0.0;

    NeditProcessor (const NeditProcessor&) = delete;
    NeditProcessor& operator= (const NeditProcessor&) = delete;
};

} // namespace nedit::plugin
