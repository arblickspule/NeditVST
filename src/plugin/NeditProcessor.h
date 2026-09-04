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
#include <atomic>
#include <string>
#include <vector>

namespace nedit::plugin {

class NeditEditor;

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
    void setSequencerViewport (float zoomX, float zoomY, float originX, float originY);

    // Per-style draw weight (GenerateState.styleWeights[i]). Publish only;
    // clamps to [0,1], ignores out-of-range style indices.
    void setStyleWeight (int styleIndex, float weight);

    // Per-style volume gain (GenerateState.styleParams.styleVolume[i]),
    // one independent value per PlaybackStyle (issue #7). Publish only;
    // clamps to [0,1], ignores out-of-range style indices.
    void setStyleVolume (int styleIndex, float volume);
    [[nodiscard]] float styleVolume (int styleIndex) const;

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

    // Whole-window vs per-tick sweep scope for the Tape Stop and Filter
    // Down+Up styles (GenerateState.tapeStopScope / filterSweepScope).
    // Publish-only -- no slice rebuild. The Clock scheduler honours them;
    // the other trigger modes pass fixed per-window values (the original
    // gated these on clock mode too).
    void setTapeStopScope (state::WindowScope scope);
    void setFilterSweepScope (state::WindowScope scope);

    // Generate-page timing (GenerateState.generateMode and its options).
    // Publish-only -- the scheduler reads these when Generate is the
    // active trigger mode.
    void setGenerateMode (state::TriggerMode mode);
    void setResetBars (int index);
    void setClockReference (int index);
    void setSubdivisionWeight (int noteIndex, float weight);
    // Momentary "n=0 / nd=0 / nt=0" quick-clears: zero every subdivision
    // weight in the palette group (plain / dotted / triplet) at once.
    void setSubdivisionGroupZero (state::NoteValueVariant variant);

    // Sequencer pattern editing (UI thread). Both operate on the working
    // grid in uiState_.sequencer and republish (the audio thread picks up
    // the new pattern via the snapshot provider).
    //
    // randomizeSequence rebuilds a fresh monophonic pattern from the
    // sequencer's own weight table + per-style randomize opt-ins, with
    // natural-length-aware, spacing-aware placement. Returns how many
    // cells were filled. Roles its own RNG seed per call.
    [[nodiscard]] int randomizeSequence();
    // clearSequence wipes the working grid + per-cell overrides/extensions,
    // keeping the dimensions and the fallback params.
    void clearSequence();

    // --- Sequencer step-grid cell edits (UI thread, publish-only) --------
    // All bounds-check against the working grid dimensions and enforce the
    // grid's monophonic invariant (at most one filled row per column).

    // Write (styleOrdinal >= 0) or clear (styleOrdinal < 0) the cell at
    // (row, column). Writing clears any other filled row in that column
    // (and its overrides/extensions). Returns whether anything changed.
    [[nodiscard]] bool setSequencerCell (int row, int column, int styleOrdinal);
    // Extend/contract the cell's declared length by `deltaSteps` (grows or
    // shrinks the per-cell extension, never below the natural length).
    // Extending clears conflicting cells in other rows of the covered
    // columns. Returns whether anything changed.
    [[nodiscard]] bool setSequencerCellExtension (int row, int column, int deltaSteps);
    // Set the style the palette paints with (selectedDrawingStyle).
    void setSelectedDrawingStyle (int styleOrdinal);

    // Write (or replace) one per-cell parameter override for the cell at
    // (row, column). `value` is clamped to the parameter's own range
    // (discrete params to a valid option index) before storing, keyed by
    // StyleParamId. Returns whether anything changed.
    [[nodiscard]] bool setSequencerCellOverride (int row, int column,
                                                 state::StyleParamId id, float value);
    // Remove a single per-cell parameter override (falls back to the
    // generated slider params at playback). Returns whether anything changed.
    [[nodiscard]] bool clearSequencerCellOverride (int row, int column,
                                                   state::StyleParamId id);

    // Sequencer transport/dimension controls (editor-local, publish-only).
    // Pattern length sets patternLengthBarsIndex; grid resolution sets
    // stepResolutionIndex. Both are grid DIMENSIONS, so they resize the
    // working grid via the documented reset-on-dimension-change contract.
    [[nodiscard]] bool setSequencerPatternLength (int index);
    [[nodiscard]] bool setSequencerStepResolution (int index);
    // Pattern-recall timing: PatternSwitchTiming ordinal (0/1/2) + the
    // note-value index of the switch interval (armed only in setInterval).
    [[nodiscard]] bool setSequencerSwitchTiming (int ordinal);
    [[nodiscard]] bool setSequencerSwitchInterval (int index);

    // Audition toggle — gates whether the scheduler produces audio.
    void setAuditionEnabled (bool enabled);

    // UI-thread drain for the audition auto-stop. The AUDIO thread never
    // mutates or clones uiState_ (a publish() from process() deep-copies
    // vectors the UI thread may be reallocating mid-copy — heap corruption
    // — and allocates on the audio thread); instead process() raises an
    // atomic flag when the transport starts while audition is on, and the
    // editor's idle timer folds it back into state here. Audio correctness
    // does not depend on this: process() gates audition rendering on the
    // live transport, so playback wins instantly even if nothing drains.
    void pollAuditionAutoStop();

    // Slice audition — loops a specific slice region. Called on RMB
    // down, cleared on RMB up. Bypasses the scheduler entirely. Bounds are
    // written BEFORE the active flag (release/acquire pairing with
    // process()) so the audio thread can never see the flag with stale
    // bounds; the read cursor itself is audio-thread-owned and reseeded on
    // the inactive->active edge.
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

#if !defined(NDEBUG)
    // Debug-only UI-test hook (tools/nedit_ui_harness). Returns the live
    // editor created by the most recent createView(kEditor), or nullptr when
    // no editor is open. NOT part of any VST3 interface -- compiled out of
    // release builds so the shipping plugin exposes no test surface.
    [[nodiscard]] NeditEditor* testHookEditor() const noexcept { return editor_; }
#endif

private:
    void registerParameters();
    void syncParameterObjectsFromState();
    // Rebuild the slice list from the retained analysis after a marker/trim
    // edit, remapping generate.sliceWeights across the rebuild by slice
    // start-frame so painted probabilities survive (unchanged boundaries
    // keep their value; a split inherits its parent slice's value). Then
    // publishes the new state snapshot.
    void rebuildSlicesPreservingWeights();
    // Derive the sequencer working-grid dimensions from the current slice
    // count + step resolution + pattern length; resets the grid when they
    // change (documented reset-on-dimension-change contract). Caller
    // publishes. Returns whether the dimensions changed.
    bool resizeSequencerGridForSample();
    // Clamp a requested marker frame into the trim and, when snap is true,
    // pull it to the nearest transient (shared by add + move).
    [[nodiscard]] std::int64_t resolveManualFrame (std::int64_t frame, bool snap) const;
    // Audition renderers (audio thread). Everything they read comes in as
    // arguments (the per-block snapshot's SampleState + the already-
    // acquired sample slot) -- they must NOT touch uiState_, which the UI
    // thread mutates freely.
    void renderAudition (const state::SampleState& sample, const LoadedSample& loaded,
                         float* const* outAdd, int numOutChannels,
                         int numSamples, double hostSampleRate);
    void renderSliceAudition (const state::SampleState& sample, const LoadedSample& loaded,
                              float* const* outAdd, int numOutChannels,
                              int numSamples, double hostSampleRate);

    // Controller-side authoritative live state.
    state::PluginState uiState_;

#if !defined(NDEBUG)
public:
    // Editor back-pointer for the Debug-only UI-test hook. Set in
    // createView(kEditor), cleared by NeditEditor::close() so testHookEditor()
    // never returns a dangling view. Public setter (not a friend) keeps the
    // coupling to a single guarded call site.
    void setTestHookEditor (NeditEditor* editor) noexcept { editor_ = editor; }
private:
    NeditEditor* editor_ = nullptr;
#endif

    // Audio-thread plumbing.
    engine::SnapshotProvider provider_;
    engine::VoiceScheduler scheduler_;
    engine::BlockContext ctx_ {};
    SampleManager sampleManager_;

    static constexpr int kMaxSourceChannels = 16;
    std::array<const float*, kMaxSourceChannels> sourceChannelPointers_ {};
    state::PluginState automationScratch_;
    double lastBlockEndPpq_ = 0.0;
    // Audition read cursor + its enable edge detector. AUDIO-thread-owned:
    // the cursor is reseeded to the trim start on the off->on edge inside
    // process(), never written from the UI thread (a cross-thread double
    // write can tear).
    double auditionPosition_ = 0.0;
    bool auditionWasActive_ = false;
    // Raised by the audio thread when the transport starts while audition
    // is enabled; drained on the UI thread by pollAuditionAutoStop().
    std::atomic<bool> auditionAutoStopPending_ { false };

    // Audio-thread scratch: the slice list clipped to the CURRENT soft trim
    // with weights remapped in lockstep (see process()). Pre-reserved in the
    // ctor so steady-state blocks never allocate.
    std::vector<engine::Slice> trimSlices_;
    std::vector<float> trimWeights_;

    // Slice audition state (RMB hold-to-loop). Bounds are stored before the
    // active flag's release-store (see startSliceAudition); the position
    // cursor is audio-thread-owned and reseeded on the inactive->active
    // edge (sliceAuditionWasActive_ is audio-thread-local state).
    std::atomic<bool> sliceAuditionActive_ { false };
    std::atomic<int64_t> sliceAuditionStart_ { 0 };
    std::atomic<int64_t> sliceAuditionEnd_ { 0 };
    double sliceAuditionPosition_ = 0.0;
    bool sliceAuditionWasActive_ = false;

    NeditProcessor (const NeditProcessor&) = delete;
    NeditProcessor& operator= (const NeditProcessor&) = delete;
};

} // namespace nedit::plugin
