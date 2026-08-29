// Nedit -- Engine layer.
//
// The per-mode schedulers: they decide WHEN picks start and what
// parameters each pick carries, then hand every pick to the shared
// PickRenderer (which decides what it sounds like). One VoiceScheduler
// owns the single monophonic voice, mirroring the original's structure:
// exactly one mode's boundary-tracking state is alive at a time, and
// entering any mode re-initializes just that mode ("snap to the window/
// grid we're currently inside and start immediately").
//
// ALL ppq boundary checks are per-sample -- the original's "Step 6 bug"
// (a boundary computed once per block silently misses ones landing
// mid-block) is the exact bug this discipline avoids.
//
// Faithful behaviours implemented here:
//
//   Slice Length: chains picks back-to-back as each finishes; periodic
//     reset windows cut the current pick at the next bar-grid boundary
//     (live-read so a mid-stream change takes effect at the next
//     boundary); beat-quantize substitutes palette-quantized durations
//     for Forward/Ping-Pong; every fresh pick is capped by the time
//     remaining until the reset boundary so its fade-out anticipates the
//     cut. All draws use Generate's weight tables (uniform fallback when
//     everything is zero).
//
//   Clock: an outer reference window with weighted-random slice/style/
//     subdivision draws ONCE PER WINDOW; every subdivision tick inside
//     the window retriggers that same slice+style from its start. Tape
//     Stop's Whole Window scope and Stretch override retriggering (one
//     continuous render spans the window). Durations anticipate whichever
//     comes first: the natural end or the next tick/window boundary.
//
//   Sequenced: the step grid advances on per-sample ppq boundaries. A
//     filled column triggers its cell's style DIRECTLY (no weighted draw)
//     with fallbackParams + that cell's overrides merged; durations come
//     from the cell's DECLARED length (natural length in steps, or its
//     Shift+drag extension), capped by the next active column anywhere in
//     the grid (anticipatory fade). Subdivide slices a step into retriggers
//     of one tick each -- the first trigger IS the first slot, retriggers
//     restart the same pick WITHOUT resetting its DSP or the window clock,
//     so sweeps glide across the whole step underneath. Pattern switching
//     (MIDI recall) defers to Set Interval / End of Pattern boundaries,
//     evaluated against the CURRENTLY playing pattern's own dimensions.
//
//   Performance: MIDI-recalled snapshots (segment trim + style + own
//     parameters + loop/sync). The focused slot's key plays the live
//     working state over the SHARED trim; any other populated slot plays a
//     frozen copy of ITS saved snapshot and trim. Sync off = native-rate
//     playback. Loop on rechains the same segment; loop off goes silent.
//     Quantize Recall defers to the next interval grid point (immediately
//     when the transport is stopped).
//
//   Control: chromatic slice triggering -- note baseNote+k plays slice k
//     with the last keyswitch-selected style at velocity-derived gain;
//     notes below base are style KEYSWITCHES (base-1-styleIndex) and never
//     sound. Gate mode adds a release fade that force-stops the pick when
//     complete; Trigger mode ignores note-offs.
//
// Runtime state here is NEVER serialized.

#pragma once

#include "PickRenderer.h"
#include "Slice.h"

#include <state/PluginState.h>
#include <state/Types.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <optional>
#include <random>
#include <vector>

namespace nedit::engine {

// Host transport values for one block. The caller substitutes the same
// fallbacks the original uses: bpm 120 when unreported, ppq 0 when absent.
struct TransportFrame
{
    bool playing = false;
    double bpm = 120.0;
    double ppqStart = 0.0;
};

class VoiceScheduler
{
public:
    // NOT audio-thread safe (prepares the renderer's flanger line).
    void prepare (double hostSampleRate);

    // Deterministic draw sequence for tests (and reproducible sessions).
    void setSeed (std::uint32_t seed) noexcept;

// Process one audio block.
    //
    // ctx must have the raw source fields pre-filled by the caller
    // (source pointers/channels/frames, host + source sample rates);
    // every DERIVED field (playback rate, granular settings, fades) is
    // recomputed here from state + transport so callers cannot desync
    // from the state snapshot.
    //
    // By default the picker reads per-slice weights from
    // state.generate.sliceWeights, assumed parallel to `slices`. When the
    // caller TRANSLATES `slices` (e.g. the shell clips the shared list to
    // the current SOFT trim so playback never reads audio outside it), it
    // must pass a matching `sliceWeightsOverride` so indices stay aligned
    // (nullptr = no override, default path). The override is POD scratch
    // the caller holds, read but never stored here.
    //
    // outAdd receives the rendered audio (rendered additively).
    void process (const state::PluginState& state,
                  const std::vector<Slice>& slices,
                  BlockContext& ctx,
                  const TransportFrame& transport,
                  float* const* outAdd,
                  int numOutChannels,
                  int numSamples,
                  const std::vector<float>* sliceWeightsOverride = nullptr);

    // Diagnostics / tests: how many picks have been started since
    // construction (never reset).
    [[nodiscard]] std::uint64_t picksStarted() const noexcept { return picksStarted_; }

    // The step index most recently crossed in Sequenced mode (-1 before
    // the first boundary). UI-facing playhead signal.
    [[nodiscard]] int playingStepIndex() const noexcept { return playingStepIndex_; }

    // MIDI dispatch entry point (Sequenced mode): arm a pattern-bank
    // recall. When the bank slot is empty the pending request is dropped
    // at its firing boundary (no-op), exactly like the original.
    void requestPatternSwitch (int midiNote) noexcept;
    [[nodiscard]] bool patternSwitchPending() const noexcept { return pendingPatternNote_ >= 0; }

    // Request that a previously applied pattern recall be dropped so
    // scheduling reads state.sequencer's working grid again. The plugin
    // shell calls this (from ANY thread) whenever a new state snapshot
    // carries sequencer edits -- with immutable snapshots, an applied
    // recall must not shadow them.
    //
    // THREAD SAFETY: the release itself is deferred to the start of the
    // next process() block on the audio thread. Destroying
    // recalledPattern_ directly from another thread would free the grid/
    // override containers WHILE runSequenced holds raw pointers into them
    // (SequencerView) mid-block -- a use-after-free.
    void requestWorkingPatternRelease() noexcept
    {
        workingPatternReleaseRequested_.store (true, std::memory_order_release);
    }

    // MIDI dispatch entry point (Performance mode). Mirrors the original's
    // note-on routing: the focused slot's key flags a fresh pick of the
    // live working state; any other populated slot freezes ITS snapshot
    // for playback; Quantize Recall defers non-focused recalls while the
    // transport plays. Empty unfocused slots are no-ops.
    void requestPerformanceRecall (const state::PluginState& state, int midiNote,
                                   bool hostTransportPlaying);

    [[nodiscard]] bool performanceRecallPending() const noexcept
    {
        return performanceRecallPending_ || pendingPerformanceNote_ >= 0;
    }

    // MIDI dispatch entry points (Control mode). baseNote and
    // numAvailableSlices come from the caller's CURRENT state snapshot;
    // the keyswitch-selected style is scheduler-owned runtime state
    // (keyswitches cannot mutate an immutable snapshot) seeded from
    // state.control.activeStyle on mode entry -- `controlActiveStyle()`
    // lets the shell fold it back into future snapshots.
    void controlNoteOn (int noteNumber, float velocity01, int baseNote,
                        int numAvailableSlices);
    void controlNoteOff (int noteNumber, bool gateModeActive) noexcept;

    [[nodiscard]] int controlActiveStyleOrdinal() const noexcept
    {
        return controlActiveStyleOrdinal_;
    }

    [[nodiscard]] int controlSoundingNote() const noexcept
    {
        return controlSoundingNote_;
    }

    [[nodiscard]] const PickRenderer& renderer() const noexcept { return renderer_; }

private:
    // Per-block plumbing shared by the implemented modes.
    struct Run
    {
        const state::PluginState& state;
        const state::GenerateState& generate;
        const std::vector<Slice>& slices;
        const std::vector<float>* sliceWeights = nullptr;  // override; null = from state
        BlockContext& ctx;
        TransportFrame transport;
        double ppqPerSample = 0.0;
        float* const* outAdd = nullptr;
        int numOutChannels = 0;
        int numSamples = 0;
    };

    // Read-only view of whichever sequencer grid is live: the working
    // pattern, or a recalled bank snapshot (by value -- it must survive
    // across blocks whose state snapshots come and go).
    struct SequencerView
    {
        int stepResolutionIndex = 0;
        int rows = 0;
        int columns = 0;
        const std::vector<std::int8_t>* grid = nullptr;
        const std::map<std::uint32_t, std::map<state::StyleParamId, float>>* overrides
            = nullptr;
        const std::map<std::uint32_t, std::uint16_t>* extensions = nullptr;
    };

    void runSliceLength (Run& r);
    void runClock (Run& r);
    void runSequenced (Run& r);
    void runPerformance (Run& r);
    void runControl (Run& r);

    // Apply a performance recall against the CURRENT block's state: the
    // focused slot flags a fresh pick of the working state, anything else
    // freezes its bank snapshot. Caller clears the pending note.
    void applyPerformanceRecallFromState (const state::PluginState& state, int note);

    [[nodiscard]] SequencerView effectiveSequencer (const state::PluginState& state) const noexcept;

    // Apply a pending recall from the CURRENT block's bank (empty slot:
    // no-op). Re-syncs the step tracker so the current step re-triggers
    // against the new grid on this very sample.
    void applyPatternRecallFromState (const state::PluginState& state, int note);

    // Start the note living at (row, step): merged parameters, declared
    // duration, next-active-column cap, Subdivide setup.
    void startSequencedPick (Run& r, const SequencerView& view, int row, int step,
                             std::size_t sliceIndex);

    // Render sample i through the voice, writing straight into the
    // block's channel planes.
    void renderSampleInto (Run& r, int i);

    void stopAndDisarm() noexcept;

    // --- weighted draws (uniform fallback when every weight is 0) ----------
    [[nodiscard]] double nextUniform() noexcept;
    [[nodiscard]] int pickWeightedIndex (const float* weights, std::size_t count);
    [[nodiscard]] int pickWeightedSlice (const Run& r);
    [[nodiscard]] state::PlaybackStyle pickWeightedStyle (const Run& r);

    // --- pick construction ---------------------------------------------------
    // The values every mode's pick-start needs, computed from one
    // (slice, style, params) triple.
    struct PreparedPick
    {
        std::int64_t schedulingEndFrame = 0;   // may exceed the source for bounce/stretch
        double naturalLengthHostSamples = 0.0; // one forward pass at playbackRate
        double midpointHostSamples = 0.0;      // bounce turnaround
        double scratchCycleHostSamples = 0.0;
    };

    [[nodiscard]] PreparedPick preparePick (state::PlaybackStyle style, const Slice& slice,
                                            const state::StyleParameters& params,
                                            double hostSampleRate, double playbackRate,
                                            double hostBpm) const;

    // Build PickParams from a preparation + finalized durations and hand
    // it to the renderer (which resets all per-pick DSP state).
    struct PickExtras
    {
        bool halfSliceFold = false;      // Sequenced Ping-Pong half window
        bool useDurationGate = false;    // Sequenced: gate on declared length
        bool volumeRampActive = false;   // Sequenced only
        bool volumeWholeWindow = false;  // Sequenced when Subdivide is on
        bool nativeRate = false;         // Performance Sync-off
        double velocityGain = 1.0;       // Control mode
    };

    void commitPick (const Slice& slice, const state::StyleParameters& params,
                     const PreparedPick& prepared,
                     state::PlaybackStyle style,
                     double pickLengthHostSamples,
                     double tapeStopDurationHostSamples,
                     bool filterWholeWindow, bool flangerWholeWindow,
                     bool beatQuantized, double quantizedStretchRatio,
                     PickExtras extras);

    // Slice Length runtime.
    enum class ArmedMode : std::uint8_t { none, sliceLength, clock, sequenced,
                                          performance, control };
    ArmedMode armedMode_ = ArmedMode::none;
    double resetWindowEndPpq_ = 0.0;

    // Clock runtime.
    double clockWindowEndPpq_ = 0.0;
    double clockNextTickPpq_ = 0.0;
    double clockWindowLengthHostSamples_ = 0.0;
    int clockSliceIndex_ = -1;
    int clockSubdivisionIndex_ = 0;
    int clockStyleOrdinal_ = 0;
    bool clockPickValid_ = false;

    // Sequenced runtime.
    int sequencedLastStepIndex_ = -1;
    int playingStepIndex_ = -1;
    int pendingPatternNote_ = -1;
    bool patternIntervalArmed_ = false;
    double patternIntervalBoundaryPpq_ = 0.0;
    bool subdivisionActive_ = false;
    int subdivisionRow_ = -1;
    double subdivisionTickLengthSamples_ = 0.0;
    double nextSubdivisionOffsetSamples_ = 0.0;
    std::optional<state::SequencerPattern> recalledPattern_;
    // Cross-thread release request, drained at the top of process() (the
    // audio thread owns recalledPattern_'s lifetime; see
    // requestWorkingPatternRelease).
    std::atomic<bool> workingPatternReleaseRequested_ { false };

    // Performance runtime.
    int pendingPerformanceNote_ = -1;
    bool performanceBoundaryArmed_ = false;
    double performanceBoundaryPpq_ = 0.0;
    bool performanceRecallPending_ = false;
    bool performancePlaybackIsFocused_ = false;
    state::PerformanceSnapshot performanceFrozenSnapshot_ {};

    // Control runtime (keyswitch style is scheduler-owned; see
    // controlNoteOn()).
    int controlActiveStyleOrdinal_ = 0;
    bool controlNoteOnPending_ = false;
    int controlPendingSliceIndex_ = -1;
    int controlPendingStyleOrdinal_ = 0;
    double controlPendingVelocityGain_ = 1.0;
    int controlPendingNoteNumber_ = -1;
    int controlSoundingNote_ = -1;

    // Safety bail (the original's 1000-attempt guard): after tripping,
    // the rest of THIS block renders silence.
    bool bailUntilBlockEnd_ = false;

    PickRenderer renderer_;
    std::mt19937 rng_ { 12345u };
    std::uint64_t picksStarted_ = 0;
};

} // namespace nedit::engine
