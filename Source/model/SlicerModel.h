#pragma once

#include <JuceHeader.h>
#include "TransientDetector.h"
#include "EasingCurve.h"
#include <array>
#include <functional>
#include <map>
#include <vector>

//==============================================================================
// SlicerModel -- the model layer of the decomposition of the former god-class
// SlicerAudioProcessor (target architecture: Engine <- Model <-> UI).
//
// Owns ALL shared audio state (the sample buffer, detected/manual slices,
// probability tables, sequencer grid + per-cell parameter overrides, pattern
// bank, performance state bank, trim markers, every stored parameter) plus
// the single sampleLock that guards it, the undo system, and the
// serialization/schema logic (static name/value/range tables) that used to
// live on the processor.
//
// Phase 1 was PURE RELOCATION: behavior is byte-identical. Phase 3 deleted
// the processor's forwarding shell -- the UI now talks to this model and to
// SlicerEngine directly, and the real-time audio core lives in SlicerEngine
// (which reads this model's (public) members under model.sampleLock).
//
// Members are deliberately public for now: the engine's processBlock() and
// the UI both need broad read access, and the refactor's rule is "move,
// don't redesign access control". Tightening the surface is a later phase.
//
// One explicit coupling to the engine: onPickStateInvalidated. The engine
// keeps per-pick/window scheduling state (hasCurrentPick,
// clockModeInitialized, clockCurrentPickValid) that loadSample() and
// rebuildSlicesFromDetectionAndManualPoints() must reset when the model's
// structure changes. The model calls this callback (under sampleLock, at the
// exact points the original code reset those flags); the engine binds it in
// its constructor. A no-op until bound.
//==============================================================================
class SlicerModel
{
public:
    SlicerModel();

    //=== Performance mode trim snap (moved from SlicerAudioProcessor) ===
    enum class TrimSnapMode { transients, grid };

    // Trim/onset-search constants (moved from SlicerAudioProcessor, where
    // the processor's kept inline accessors referenced them directly).
    static constexpr float manualSnapRadiusMs = 50.0f;
    static constexpr int minTrimGapSamples = 64;
    static constexpr float defaultSensitivity = 0.5f;

    void setPerformanceTrimSnapMode (TrimSnapMode mode) { performanceTrimSnapMode.store (mode); }
    TrimSnapMode getPerformanceTrimSnapMode() const { return performanceTrimSnapMode.load(); }

    //=== UI-facing accessors (Phase 3 -- moved from SlicerAudioProcessor) ===
    // The UI now talks to the model directly instead of through the
    // processor's forwarding shell (Phase 3 deleted it); these are the
    // accessors the sample and detection controls read.
    bool hasSample() const { return sampleLoaded; }
    const juce::AudioBuffer<float>& getSampleBuffer() const { return sampleBuffer; }
    double getSampleSampleRate() const { return sampleSampleRate; }
    juce::String getLoadedFileName() const { return loadedFileName; }
    int getNumSlices() const { return (int) slices.size(); }
    const std::vector<Slice>& getSlices() const { return slices; }

    // Live sensitivity control -- re-runs detection immediately (cheap,
    // since TransientDetector caches the expensive envelope/derivative
    // pass) and resets slice probabilities to 1.0, same as any other
    // re-slice, since the slice boundaries themselves change.
    void setSensitivityAndRedetect (float sensitivity)
    {
        currentSensitivity.store (juce::jlimit (0.0f, 1.0f, sensitivity));
        redetectSlices (currentSensitivity.load(), computeMinimumHoldoffMs());
    }

    float getSensitivity() const { return currentSensitivity.load(); }

    void setQuantizeTransientsEnabled (bool enabled)
    {
        quantizeTransientsEnabled.store (enabled);
        redetectSlices (currentSensitivity.load(), computeMinimumHoldoffMs());
    }

    bool getQuantizeTransientsEnabled() const { return quantizeTransientsEnabled.load(); }

    void setQuantizeGridIndex (int index)
    {
        quantizeGridIndex.store (juce::jlimit (0, numNoteValueOptions - 1, index));
        redetectSlices (currentSensitivity.load(), computeMinimumHoldoffMs());
    }

    int getQuantizeGridIndex() const { return quantizeGridIndex.load(); }

    // Grid resolution -- same 20-value note-value palette as Clock
    // reference/Quantize Transients' Grid/Subdivide (getNoteValueName()/
    // getNoteValueBeats(), no separate table). Default index 13 (4n / one
    // quarter note), the same default every other note-value-palette
    // control here uses.
    void setPerformanceTrimGridIndex (int index)
    {
        performanceTrimGridIndex.store (juce::jlimit (0, numNoteValueOptions - 1, index));
    }

    int getPerformanceTrimGridIndex() const { return performanceTrimGridIndex.load(); }

    //=== Trim markers (Step 23/25) ===
    // Two independent boundaries, in source-sample units, confining
    // EVERYTHING else in this class to [trimStart, trimEnd): transient
    // detection, manual slice point add/move (including the snap-to-
    // transient search), and therefore what can ever become a slice or get
    // played. Default to the full sample length on load (start=0,
    // end=buffer length). One exception: the sample's established TEMPO
    // reads tempoTrimStartSample/tempoTrimEndSample instead (see those
    // members' own comment) -- Performance mode repoints trimStartSample/
    // trimEndSample at whichever state slot has focus, while the tempo
    // copies stay pinned to the last REAL trim edit.
    int getTrimStartSample() const { return trimStartSample.load(); }
    int getTrimEndSample() const { return trimEndSample.load(); }

    // Snapping (Step 25) reuses TransientDetector::findNearestPeak, with
    // Shift held (snapToTransient = false) bypassing it for free placement.
    // A trim handle's search is unconstrained (findNearestPeak's default
    // (-1, -1) range), unlike manual points', since there's no "inside the
    // trim" yet until the trim itself is set. Trim Snap mode: while
    // snapToTransient is true AND Performance mode is active AND
    // performanceTrimSnapMode is Grid, findNearestGridSample() replaces the
    // transient search entirely.
    void setTrimStartSample (int sample, bool snapToTransient = true)
    {
        const int currentEnd = trimEndSample.load();
        const int upperBound = juce::jmax (0, currentEnd - minTrimGapSamples); // guards tiny/degenerate buffers
        int target = juce::jlimit (0, upperBound, sample);

        if (snapToTransient)
        {
            target = juce::jlimit (0, upperBound, shouldGridSnapTrim()
                ? findNearestGridSample (target)
                : transientDetector.findNearestPeak (target, (int) (manualSnapRadiusMs / 1000.0f * (float) sampleSampleRate)));
        }

        trimStartSample.store (target);

        // Performance mode reuses these same atomics to edit whichever
        // state slot currently has focus -- that's never a real change to
        // the sample's established tempo, so the tempo-trim copy sits this
        // one out.
        if (triggerMode.load() != TriggerMode::performance)
            tempoTrimStartSample.store (target);

        rebuildSlicesFromDetectionAndManualPoints (currentSensitivity.load(), computeMinimumHoldoffMs());
    }

    void setTrimEndSample (int sample, bool snapToTransient = true)
    {
        const int currentStart = trimStartSample.load();
        const int bufferLength = sampleBuffer.getNumSamples();
        const int lowerBound = juce::jmin (currentStart + minTrimGapSamples, bufferLength); // guards tiny/degenerate buffers
        int target = juce::jlimit (lowerBound, bufferLength, sample);

        if (snapToTransient)
        {
            target = juce::jlimit (lowerBound, bufferLength, shouldGridSnapTrim()
                ? findNearestGridSample (target)
                : transientDetector.findNearestPeak (target, (int) (manualSnapRadiusMs / 1000.0f * (float) sampleSampleRate)));
        }

        trimEndSample.store (target);

        // See setTrimStartSample() above -- same reasoning, same guard.
        if (triggerMode.load() != TriggerMode::performance)
            tempoTrimEndSample.store (target);

        rebuildSlicesFromDetectionAndManualPoints (currentSensitivity.load(), computeMinimumHoldoffMs());
    }

    //=== Audition (Step 25) ===
    // Plays [trimStart, trimEnd) on a tight raw loop at native pitch/speed
    // (sample-rate-matched only), completely bypassing the generative
    // engine. Auto-stops the instant host transport starts. See the
    // engine's processBlock() auditionActive check, which runs before
    // (and instead of) everything below it.
    void setAuditionActive (bool active)
    {
        const juce::ScopedLock sl (sampleLock); // guards the UI playhead below; the engine's read cursor (SlicerEngine::auditionPosition) is re-armed by SlicerEngine::setAuditionActive(), which wraps this

        if (active)
        {
            auditionPlaybackPositionForUI.store (trimStartSample.load()); // immediate UI feedback, rather than waiting for the first rendered block
        }
        else
        {
            auditionPlaybackPositionForUI.store (-1);
        }

        auditionActive.store (active);
    }

    bool getAuditionActive() const { return auditionActive.load(); }

    //=== Audition playhead (Step 28) ===
    // Lock-free copy of the audition engine's current read position, for
    // the waveform's playhead indicator. -1 means "not currently
    // auditioning". Written every block by the engine's renderAudition()
    // while it's running.
    int getAuditionPlaybackPosition() const { return auditionPlaybackPositionForUI.load(); }

    // Live preview (Step 12): shows what detection WOULD produce at a
    // given sensitivity -- merged with the current manual/excluded points,
    // same as a real commit -- but without touching playback state at all
    // (no probability reset, no interrupting the current pick, not added
    // to undo history). Safe to call repeatedly while a slider is being
    // dragged; the real commit only happens via setSensitivityAndRedetect().
    std::vector<Slice> previewSlicesAtSensitivity (float sensitivity) const;

    //=== Manual slice points (Step 10) ===
    // User-placed slice boundaries, layered on top of whatever the
    // detector finds automatically. Unlike auto-detected slices, these
    // survive a sensitivity change -- redetection only regenerates the
    // auto side and re-merges it with whatever manual points already
    // exist.
    struct ManualPointInfo
    {
        int id = -1;
        int samplePosition = 0;
    };

    // Adds a new manual point near targetSample. Snaps to the nearest
    // real transient-like peak by default; pass snapToTransient = false
    // (Shift held) to place it at the exact position instead. Returns its
    // stable id, used later to move or remove it.
    int addManualSlicePoint (int targetSample, bool snapToTransient = true);

    // Moves an existing manual point (by id) to a new target. Deliberately
    // NOT undo-tracked (called continuously during a drag); call
    // commitManualPointMove() once, at drag-end, to record the whole drag
    // as a single undoable step.
    void moveManualSlicePoint (int id, int targetSample, bool snapToTransient = true);

    // Records a completed drag (from originalSamplePosition to wherever
    // the point currently is) as one undo step. Call this on mouse-up.
    void commitManualPointMove (int id, int originalSamplePosition);

    void removeManualSlicePoint (int id);

    std::vector<ManualPointInfo> getManualSlicePoints() const
    {
        const juce::ScopedLock sl (sampleLock);
        std::vector<ManualPointInfo> result;
        result.reserve (manualPoints.size());

        for (const auto& mp : manualPoints)
            result.push_back ({ mp.id, mp.samplePosition });

        return result;
    }

    //=== Deleting auto-detected transients (Step 11) ===
    // "Deletes" the nearest auto-detected boundary to targetSample by
    // adding it to an exclusion list -- matched by proximity (same
    // tolerance as manual-point snapping) rather than exact position.
    // Position 0 (the very start of the sample) can never be excluded.
    // Returns the new exclusion's id, or -1 if there was nothing nearby.
    int excludeNearestAutoPoint (int targetSample);

    // Un-deletes a single excluded point.
    void restoreExcludedPoint (int id);

    std::vector<ManualPointInfo> getExcludedPoints() const
    {
        const juce::ScopedLock sl (sampleLock);
        std::vector<ManualPointInfo> result;
        result.reserve (excludedPoints.size());

        for (const auto& ep : excludedPoints)
            result.push_back ({ ep.id, ep.samplePosition });

        return result;
    }

    // Safety net: clears every manual addition AND every exclusion in one
    // go, back to exactly what the detector alone would produce at the
    // current sensitivity. Undo-tracked like everything else in this
    // section.
    void resetAllManualEdits();

    //=== Undo/redo (Step 12) ===
    // Covers manual point add/move/remove and auto-point exclude/restore
    // (including Reset) -- every slice-editing action, as one coalesced
    // step each. Deliberately does NOT cover sensitivity, probability
    // sliders, loop length, or fades -- continuous parameters, not
    // discrete "actions".
    bool undoLastEdit() { return undoManager.undo(); }
    bool redoLastEdit() { return undoManager.redo(); }
    bool canUndoEdit() const { return undoManager.canUndo(); }
    bool canRedoEdit() const { return undoManager.canRedo(); }

    // Overwrites manual + excluded point state wholesale and rebuilds --
    // the one place all undo/redo actions actually apply a snapshot.
    // Public because the undo action objects (defined in SlicerModel.cpp)
    // need to call it; not intended to be called directly from the UI.
    void applyManualState (const std::vector<ManualPointInfo>& manual,
                            const std::vector<ManualPointInfo>& excluded);

    //=== Currently-playing slice (Step 11) ===
    // For UI highlighting -- which slice is sounding right now, updated by
    // the audio thread every time a new pick begins. -1 when nothing's
    // playing (including whenever the transport is stopped).
    int getCurrentlyPlayingSliceIndex() const { return currentlyPlayingSliceIndexForUI.load(); }

    //=== Loop length / tempo sync ===
    // How many bars (assumed 4/4) the loaded sample represents. This is
    // what lets us calculate the sample's original tempo and therefore how
    // much to repitch it to match the host.
    void setLoopLengthBars (int bars)
    {
        loopLengthBars.store (juce::jmax (1, bars));

        // Sequenced Trigger Mode (Step 37): column count is derived from
        // loopLengthBars, so any change here invalidates the existing
        // pattern's meaning -- same "reset on rebuild" convention
        // sliceProbabilities already uses.
        const juce::ScopedLock sl (sampleLock);
        resetSequencerGrid();
    }

    int getLoopLengthBars() const { return loopLengthBars.load(); }

    //=== Manual BPM override (Step 23) ===
    // When enabled, REPLACES the bars-derived tempo calculation entirely
    // (not layered alongside it) -- see computeSourceSpanSeconds(), the one
    // shared function both this and the Trim markers above feed into.
    void setManualBpmOverrideEnabled (bool enabled) { manualBpmOverrideEnabled.store (enabled); }
    bool getManualBpmOverrideEnabled() const { return manualBpmOverrideEnabled.load(); }

    void setManualBpmOverrideValue (double bpm) { manualBpmOverrideValue.store (juce::jmax (1.0, bpm)); }
    double getManualBpmOverrideValue() const { return manualBpmOverrideValue.load(); }

    // Calculated from loopLengthBars + (the trimmed span of the sample, or
    // the manual BPM override when enabled). Exposed mainly so the editor
    // can display it -- "this loop is ~140 BPM". Shows the override value
    // directly when it's active.
    double getCalculatedOriginalBpm() const
    {
        if (manualBpmOverrideEnabled.load())
            return manualBpmOverrideValue.load();

        if (! sampleLoaded || sampleBuffer.getNumSamples() == 0)
            return 0.0;

        const double lengthSeconds = computeSourceSpanSeconds();

        if (lengthSeconds <= 0.0)
            return 0.0;

        const double beats = (double) loopLengthBars.load() * 4.0; // assumes 4/4
        return (beats * 60.0) / lengthSeconds;
    }

    //=== Per-slice weight (Step 8) ===
    // Relative weight in the weighted-random draw that picks the next
    // slice to play -- NOT an independent per-hit probability anymore.
    // 0 = excluded from the draw entirely. Defaults to 1.0 (even odds
    // across all slices) on every re-slice.
    float getSliceProbability (int index) const
    {
        const juce::ScopedLock sl (sampleLock);

        if (index < 0 || index >= (int) sliceProbabilities.size())
            return 1.0f;

        return sliceProbabilities[(size_t) index];
    }

    void setSliceProbability (int index, float probability)
    {
        const juce::ScopedLock sl (sampleLock);

        if (index >= 0 && index < (int) sliceProbabilities.size())
            sliceProbabilities[(size_t) index] = juce::jlimit (0.0f, 1.0f, probability);
    }

    //=== De-clicking (Step 9) ===
    // Global fade-in/fade-out applied at the start/end of every slice
    // pick, in milliseconds. Clamped per-pick to at most half that pick's
    // own length so a very short slice can't have overlapping/inverted
    // fades.
    void setFadeInMs (float ms) { fadeInMs.store (juce::jmax (0.0f, ms)); }
    float getFadeInMs() const { return fadeInMs.load(); }

    void setFadeOutMs (float ms) { fadeOutMs.store (juce::jmax (0.0f, ms)); }
    float getFadeOutMs() const { return fadeOutMs.load(); }

    //=== Trigger mode (Step 14/37) ===
    //   sliceLength -- the picked slice plays in full at its own length,
    //     and finishing IS the cue to pick again.
    //   clock -- a fixed outer window (the "clock reference") picks ONE
    //     slice + ONE subdivision rate together at the top of the window,
    //     then retriggers that same slice from its start every subdivision
    //     tick for the rest of the window.
    //   sequenced (Step 37, v1 -- monophonic) -- everything is explicitly
    //     placed by the user on a step grid; the probability engine sits
    //     unused while this mode is active.
    //   performance (Pass 1) -- a single hand-defined segment played back
    //     on MIDI recall from a 128-note-indexed state bank.
    enum class TriggerMode { sliceLength, clock, sequenced, performance };

    // Low-level store only -- the engine's setTriggerMode() wraps this and
    // also resets its own engine scheduling flags (clockModeInitialized
    // etc.), which are NOT this model's concern.
    void setTriggerMode (TriggerMode mode) { triggerMode.store (mode); }
    TriggerMode getTriggerMode() const { return triggerMode.load(); }

    // Fixed palette of note values, shared between the outer clock
    // reference menu and the inner subdivision probability table --
    // expressed in quarter-note ("beat") units so nothing here needs to
    // assume a time signature. Matches the standard Max/M4L tempo-relative
    // rate set (128n up to 1n), capped at one bar as the longest option.
    static constexpr int numNoteValueOptions = 20;
    static juce::String getNoteValueName (int index);
    static double getNoteValueBeats (int index);

    void setClockReferenceIndex (int index)
    {
        clockReferenceIndex.store (juce::jlimit (0, numNoteValueOptions - 1, index));
    }

    int getClockReferenceIndex() const { return clockReferenceIndex.load(); }

    // Weighted-probability table for which subdivision gets picked each
    // window in Clock mode -- same 0-1 weight semantics as slice weights.
    float getSubdivisionProbability (int index) const
    {
        const juce::ScopedLock sl (sampleLock);

        if (index < 0 || index >= (int) subdivisionProbabilities.size())
            return 1.0f;

        return subdivisionProbabilities[(size_t) index];
    }

    void setSubdivisionProbability (int index, float probability)
    {
        const juce::ScopedLock sl (sampleLock);

        if (index >= 0 && index < (int) subdivisionProbabilities.size())
            subdivisionProbabilities[(size_t) index] = juce::jlimit (0.0f, 1.0f, probability);
    }

    //=== Playback style (Step 19/21/22/29/30/48/Scratch/Flanger) ===
    // A weighted table, independent of (but rolled at the same time as)
    // the slice/subdivision picks above. Forward is today's behaviour;
    // Ping-Pong plays a slice forward then immediately backward; Tape Stop
    // decelerates rate AND gain linearly to zero; Stretch renders through
    // GranularStretcher regardless of Pitch Mode; Filter Down/Up post-
    // process the pick's output with a resonant filter swept log-scale
    // across its duration; Bitcrush is a sample-and-hold + bit-depth
    // quantization pass; Scratch bounces via foldPosition() at a fast,
    // adjustable rate (with per-stroke EasingCurve speed profiles); Flanger
    // mixes a short feedback delay line with the dry signal. Defaults to
    // Forward-only (weight 0 on everything else) -- what guarantees the
    // default sounds byte-identical to before each style existed.
    enum class PlaybackStyle { forward, pingPong, tapeStop, stretch, filterSweepDown, filterSweepUp, bitcrush, scratch, flanger };

    static constexpr int numPlaybackStyleOptions = 9;
    static juce::String getPlaybackStyleName (int index); // "Forward" / "Ping-Pong" / "Tape Stop" / "Stretch" / "Filter Down" / "Filter Up" / "Bitcrush" / "Scratch" / "Flanger"

    float getPlaybackStyleProbability (int index) const
    {
        const juce::ScopedLock sl (sampleLock);

        if (index < 0 || index >= (int) playbackStyleProbabilities.size())
            return 1.0f;

        return playbackStyleProbabilities[(size_t) index];
    }

    void setPlaybackStyleProbability (int index, float probability)
    {
        const juce::ScopedLock sl (sampleLock);

        if (index >= 0 && index < (int) playbackStyleProbabilities.size())
            playbackStyleProbabilities[(size_t) index] = juce::jlimit (0.0f, 1.0f, probability);
    }

    //=== Tape Stop scope (Step 21) ===
    // Clock-mode-only: how long a Tape Stop pick's decel lasts.
    //   wholeWindow (default) -- one continuous decel across the entire
    //     clock reference length, overriding normal subdivision
    //     retriggering for that window.
    //   perTick -- each individual subdivision tick gets its own quick
    //     decel-to-zero-and-restart.
    enum class TapeStopScope { wholeWindow, perTick };

    static constexpr int numTapeStopScopeOptions = 2;
    static juce::String getTapeStopScopeName (int index); // "Whole window" / "Per tick"

    void setTapeStopScope (TapeStopScope scope) { tapeStopScope.store (scope); }
    TapeStopScope getTapeStopScope() const { return tapeStopScope.load(); }

    //=== Slice Length periodic reset (Step 34) ===
    // Mandatory (no "Off" option) -- forces a hard resync every resetBars
    // bars in Slice Length mode. Visible only in Slice Length mode.
    static constexpr int numResetBarsOptions = 4;
    static juce::String getResetBarsName (int index); // "1 bar" / "2 bars" / "4 bars" / "8 bars"
    static int getResetBarsValue (int index);         // 1 / 2 / 4 / 8

    void setResetBarsIndex (int index) { resetBarsIndex.store (juce::jlimit (0, numResetBarsOptions - 1, index)); }
    int getResetBarsIndex() const { return resetBarsIndex.load(); }

    //=== Filter Sweep scope (Step 30) ===
    // Clock-mode-only:
    //   perTick (default) -- sweep progress is samplesSincePickStart /
    //     currentPickLengthInHostSamples, resetting at every retrigger.
    //   wholeWindow -- progress is samplesSinceWindowStart /
    //     currentWindowLengthHostSamples, continuous across every tick.
    enum class FilterSweepScope { wholeWindow, perTick };

    static constexpr int numFilterSweepScopeOptions = 2;
    static juce::String getFilterSweepScopeName (int index); // "Whole window" / "Per tick"

    void setFilterSweepScope (FilterSweepScope scope) { filterSweepScope.store (scope); }
    FilterSweepScope getFilterSweepScope() const { return filterSweepScope.load(); }

    //=== Filter Sweep resonance (Step 45) ===
    // Was a compile-time constant (filterSweepResonance = 2.0) until now.
    static constexpr float defaultFilterSweepResonance = 2.0f;
    static constexpr float minFilterSweepResonance = 0.5f;
    static constexpr float maxFilterSweepResonance = 10.0f;

    void setFilterSweepResonance (float resonance)
    {
        filterSweepResonanceValue.store (juce::jlimit (minFilterSweepResonance, maxFilterSweepResonance, resonance));
    }

    float getFilterSweepResonance() const { return filterSweepResonanceValue.load(); }

    //=== Filter Sweep filter type (Step 46) ===
    // Index-based (0/1/2). Default (0 -- lowpass) matches the filter's
    // original hardcoded setType() call exactly.
    static constexpr int numFilterSweepFilterTypeOptions = 3;
    static juce::String getFilterSweepFilterTypeName (int index); // "Low-pass" / "High-pass" / "Band-pass"

    void setFilterSweepFilterType (int index) { filterSweepFilterTypeValue.store (juce::jlimit (0, numFilterSweepFilterTypeOptions - 1, index)); }
    int getFilterSweepFilterType() const { return filterSweepFilterTypeValue.load(); }

    //=== Curve shape (Step 46) ===
    // Shared between Tape Stop's decel and Ping-Pong's turnaround fade.
    // Default (0 -- Linear) matches both styles' existing behaviour.
    static constexpr int numCurveShapeOptions = 2;
    static juce::String getCurveShapeName (int index); // "Linear" / "Exponential"

    void setCurveShape (int index) { curveShapeValue.store (juce::jlimit (0, numCurveShapeOptions - 1, index)); }
    int getCurveShape() const { return curveShapeValue.load(); }

    //=== Stretch grain settings (Step 46) ===
    // Stretch playback style's own grain size/speed. Defaults match the
    // original constants exactly.
    static constexpr float defaultStretchGrainSizeMs = 10.0f;
    static constexpr float minStretchGrainSizeMs = 5.0f;
    static constexpr float maxStretchGrainSizeMs = 30.0f;

    void setStretchGrainSizeMs (float ms) { stretchGrainSizeMsValue.store (juce::jlimit (minStretchGrainSizeMs, maxStretchGrainSizeMs, ms)); }
    float getStretchGrainSizeMs() const { return stretchGrainSizeMsValue.load(); }

    // "Speed" here is a FIXED character constant -- how many times slower
    // than normal playback grains march through the source material for
    // ONE pass (originally the fixed stretchDurationMultiplier = 4.0).
    static constexpr float defaultStretchSpeedMultiplier = 4.0f;
    static constexpr float minStretchSpeedMultiplier = 1.0f;
    static constexpr float maxStretchSpeedMultiplier = 8.0f;

    void setStretchSpeedMultiplier (float multiplier) { stretchSpeedMultiplierValue.store (juce::jlimit (minStretchSpeedMultiplier, maxStretchSpeedMultiplier, multiplier)); }
    float getStretchSpeedMultiplier() const { return stretchSpeedMultiplierValue.load(); }

    //=== Bitcrush/Flanger/Scratch character constants ===
    // Fixed Sweep In/Out extremes (Slice Length/Clock mode picks always
    // use the global defaults below when no per-step override exists).
    // Shared with the engine's processBlock() sweep code.
    static constexpr float bitcrushRateReductionExtreme = 48.0f;
    static constexpr float bitcrushBitDepthExtreme = 1.0f;
    static constexpr float flangerDelayTimeMinMs = 0.5f;
    static constexpr float flangerDelayTimeExtremeMs = 10.0f;
    static constexpr float flangerMixExtreme = 1.0f;
    static constexpr float flangerFeedbackExtreme = 0.88f;

    //=== Bitcrush/Flanger/Scratch global defaults (Slice Length/Clock mode
    // parameter panel) ===
    // Stored, adjustable values with defaults matching the original fixed
    // constants. Min/max/option-count clamping reuses the generic
    // getSequencerCellParameterMin()/Max()/NumOptions() (indices 6-18).
    void setBitcrushRateReductionGlobal (float value) { bitcrushRateReductionGlobalValue.store (juce::jlimit (getSequencerCellParameterMin (6), getSequencerCellParameterMax (6), value)); }
    float getBitcrushRateReductionGlobal() const { return bitcrushRateReductionGlobalValue.load(); }
    void setBitcrushRateReductionModeGlobal (int mode) { bitcrushRateReductionModeGlobalValue.store (juce::jlimit (0, getSequencerCellParameterNumOptions (7) - 1, mode)); }
    int getBitcrushRateReductionModeGlobal() const { return bitcrushRateReductionModeGlobalValue.load(); }

    void setBitcrushBitDepthGlobal (float value) { bitcrushBitDepthGlobalValue.store (juce::jlimit (getSequencerCellParameterMin (8), getSequencerCellParameterMax (8), value)); }
    float getBitcrushBitDepthGlobal() const { return bitcrushBitDepthGlobalValue.load(); }
    void setBitcrushBitDepthModeGlobal (int mode) { bitcrushBitDepthModeGlobalValue.store (juce::jlimit (0, getSequencerCellParameterNumOptions (9) - 1, mode)); }
    int getBitcrushBitDepthModeGlobal() const { return bitcrushBitDepthModeGlobalValue.load(); }

    void setScratchRateGlobal (int index) { scratchRateGlobalValue.store (juce::jlimit (0, getSequencerCellParameterNumOptions (10) - 1, index)); }
    int getScratchRateGlobal() const { return scratchRateGlobalValue.load(); }
    void setScratchForwardCurveGlobal (int index) { scratchForwardCurveGlobalValue.store (juce::jlimit (0, getSequencerCellParameterNumOptions (11) - 1, index)); }
    int getScratchForwardCurveGlobal() const { return scratchForwardCurveGlobalValue.load(); }
    void setScratchBackwardCurveGlobal (int index) { scratchBackwardCurveGlobalValue.store (juce::jlimit (0, getSequencerCellParameterNumOptions (12) - 1, index)); }
    int getScratchBackwardCurveGlobal() const { return scratchBackwardCurveGlobalValue.load(); }

    void setFlangerDelayTimeGlobal (float value) { flangerDelayTimeGlobalValue.store (juce::jlimit (getSequencerCellParameterMin (13), getSequencerCellParameterMax (13), value)); }
    float getFlangerDelayTimeGlobal() const { return flangerDelayTimeGlobalValue.load(); }
    void setFlangerDelayTimeModeGlobal (int mode) { flangerDelayTimeModeGlobalValue.store (juce::jlimit (0, getSequencerCellParameterNumOptions (14) - 1, mode)); }
    int getFlangerDelayTimeModeGlobal() const { return flangerDelayTimeModeGlobalValue.load(); }

    void setFlangerMixGlobal (float value) { flangerMixGlobalValue.store (juce::jlimit (getSequencerCellParameterMin (15), getSequencerCellParameterMax (15), value)); }
    float getFlangerMixGlobal() const { return flangerMixGlobalValue.load(); }
    void setFlangerMixModeGlobal (int mode) { flangerMixModeGlobalValue.store (juce::jlimit (0, getSequencerCellParameterNumOptions (16) - 1, mode)); }
    int getFlangerMixModeGlobal() const { return flangerMixModeGlobalValue.load(); }

    void setFlangerFeedbackGlobal (float value) { flangerFeedbackGlobalValue.store (juce::jlimit (getSequencerCellParameterMin (17), getSequencerCellParameterMax (17), value)); }
    float getFlangerFeedbackGlobal() const { return flangerFeedbackGlobalValue.load(); }
    void setFlangerFeedbackModeGlobal (int mode) { flangerFeedbackModeGlobalValue.store (juce::jlimit (0, getSequencerCellParameterNumOptions (18) - 1, mode)); }
    int getFlangerFeedbackModeGlobal() const { return flangerFeedbackModeGlobalValue.load(); }

    //=== Pitch mode (Step 17) ===
    //   repitch -- today's varispeed behaviour.
    //   timeStretch -- lightweight overlap-add granular synthesis.
    enum class PitchMode { repitch, timeStretch };

    // Low-level store only -- the engine's setPitchMode() wraps this and
    // also sets its own granularNeedsReseed engine flag.
    void setPitchMode (PitchMode mode) { pitchMode.store (mode); }
    PitchMode getPitchMode() const { return pitchMode.load(); }

    // Grain length for Time-Stretch mode. Overlap is fixed at 50% (not
    // exposed) to keep the UI minimal.
    void setGrainSizeMs (float ms) { grainSizeMs.store (juce::jlimit (20.0f, 150.0f, ms)); }
    float getGrainSizeMs() const { return grainSizeMs.load(); }

    enum class GrainWindowShape { hann, triangular };

    void setGrainWindowShape (GrainWindowShape shape) { grainWindowShape.store (shape); }
    GrainWindowShape getGrainWindowShape() const { return grainWindowShape.load(); }

    // Time-Stretch-only pitch control (Step 18) -- a multiplier on each
    // grain's own internal read-rate, entirely separate from the hop
    // scheduling above that controls stretch amount.
    void setPitchShiftSemitones (float semitones) { pitchShiftSemitones.store (juce::jlimit (-24.0f, 24.0f, semitones)); }
    float getPitchShiftSemitones() const { return pitchShiftSemitones.load(); }

    //=== Beat-quantized slice length (Step 24/26) ===
    // Only takes effect for Pitch Mode == timeStretch (or repitch, via the
    // separate toggle below) AND Trigger Mode == sliceLength.
    void setBeatQuantizeSliceLengthEnabled (bool enabled) { beatQuantizeSliceLengthEnabled.store (enabled); }
    bool getBeatQuantizeSliceLengthEnabled() const { return beatQuantizeSliceLengthEnabled.load(); }

    void setBeatQuantizeSliceLengthEnabledRepitch (bool enabled) { beatQuantizeSliceLengthEnabledRepitch.store (enabled); }
    bool getBeatQuantizeSliceLengthEnabledRepitch() const { return beatQuantizeSliceLengthEnabledRepitch.load(); }

    //=== Sequenced Trigger Mode (Step 37, v1 -- monophonic) ===
    // A mouse-drawable step grid: rows are available slices, columns are
    // steps. Structural monophony is enforced at the INPUT level -- only
    // one cell may be active per column across the whole grid.
    //
    // Grid dimensions:
    //   rows -- one per available slice (from the existing `slices` list),
    //     capped at numSequencerRows (32).
    //   columns ("steps") -- patternLengthBars * 4 * stepsPerBeat, where
    //     stepsPerBeat comes from the Step resolution dropdown.
    //
    // The pattern is reset to all-off whenever any dimension changes.
    static constexpr int numSequencerRows = 32;

    // Defensive cap purely for UI/performance sanity at extreme parameter
    // combinations (e.g. 8 bars at 128th notes would otherwise be 1024
    // columns).
    static constexpr int maxSequencerColumns = 256;

    int getSequencerNumRows() const { return juce::jmin (numSequencerRows, (int) slices.size()); }

    int getSequencerNumSteps() const
    {
        const double gridBeats = getNoteValueBeats (stepResolutionIndex.load());
        const double stepsPerBeat = (gridBeats > 0.0) ? (1.0 / gridBeats) : 1.0;
        const int patternBars = getPatternLengthBarsValue (patternLengthBarsIndex.load());
        const int rawSteps = juce::roundToInt ((double) patternBars * 4.0 * stepsPerBeat);
        return juce::jlimit (1, maxSequencerColumns, rawSteps);
    }

    // Pattern length (Step 38) -- 1/2/4 bars, deliberately separate from
    // loopLengthBars. Changing it changes the column count, so it resets
    // the grid the same way Step resolution already does.
    static constexpr int numPatternLengthBarsOptions = 3;
    static juce::String getPatternLengthBarsName (int index); // "1 bar" / "2 bars" / "4 bars"
    static int getPatternLengthBarsValue (int index);         // 1 / 2 / 4

    void setPatternLengthBarsIndex (int index)
    {
        patternLengthBarsIndex.store (juce::jlimit (0, numPatternLengthBarsOptions - 1, index));
        const juce::ScopedLock sl (sampleLock);
        resetSequencerGrid(); // column count just changed
    }

    int getPatternLengthBarsIndex() const { return patternLengthBarsIndex.load(); }

    // Step resolution -- reuses the same 20-value note-value palette as
    // Clock reference/Quantize Transients' Grid. Defaults to index 7 (16n).
    void setStepResolutionIndex (int index)
    {
        stepResolutionIndex.store (juce::jlimit (0, numNoteValueOptions - 1, index));
        const juce::ScopedLock sl (sampleLock);
        resetSequencerGrid(); // column count just changed
    }

    int getStepResolutionIndex() const { return stepResolutionIndex.load(); }

    // Cell state (Step 41): each cell stores -1 (empty) or a PlaybackStyle
    // index. row/column outside the current grid dimensions are silently
    // ignored.
    int getSequencerCellStyle (int row, int column) const; // -1 if empty or out-of-range
    void setSequencerCell (int row, int column, int style); // style -1 clears; 0 to numPlaybackStyleOptions-1 sets that PlaybackStyle

    // Step-extension (Pass 1, mechanism only) -- an optional per-cell
    // "extended length in steps" override. Unset (0) means "use today's
    // behaviour".
    int getSequencerCellExtendedLengthSteps (int row, int column) const; // 0 if unset/empty/out-of-range
    void setSequencerCellExtendedLengthSteps (int row, int column, int lengthSteps); // clamped into [1, numSteps - column]; no-op on an empty cell

    // `row`'s slice, expressed in steps at the current Step-resolution --
    // the same calculation SequencerGrid's piano-roll bar has always
    // started from. Shared here (not just computed in the UI) so the audio
    // thread can read the exact same value.
    int getSequencerNaturalLengthSteps (int row) const;

    // This cell's own declared length in steps: its Step-extension
    // override if longer than natural, else natural.
    int getSequencerCellDeclaredLengthSteps (int row, int column) const;

    // Currently selected drawing style (Step 41) -- persistent UI state
    // for the Style Palette. Defaults to Forward (index 0).
    int getSelectedDrawingStyle() const { return selectedDrawingStyle.load(); }

    void setSelectedDrawingStyle (int style)
    {
        selectedDrawingStyle.store (juce::jlimit (0, numPlaybackStyleOptions - 1, style));
    }

    // Clear Sequence (Step 41): wipes the pattern back to all-empty, no
    // generation afterward.
    void clearSequence();

    //=== Sequencer step parameter overrides (Step 45/46, Sequenced mode only) ===
    // Each sequencer cell can optionally carry a sparse map from parameter
    // name to value. An empty/absent entry means "use the global default."
    // Overrides for a given cell are cleared whenever that cell's own style
    // is set/changed (including cleared to empty), and wiped entirely
    // whenever the grid itself resets or Clear/Randomize Sequence runs.
    static constexpr int numSequencerCellParameters = 21;
    static juce::String getSequencerCellParameterName (int index); // "Resonance" / "Filter Type" / "Curve Shape" / "Grain Size" / "Grain Speed" / "Subdivide" / "Sample Rate Reduction" / "Sample Rate Reduction Mode" / "Bit Depth" / "Bit Depth Mode" / "Rate" / "Forward Curve" / "Backward Curve" / "Delay Time" / "Delay Time Mode" / "Mix" / "Mix Mode" / "Feedback" / "Feedback Mode" / "Volume" / "Volume Mode"

    // Swept parameters: indices 6/8 (Sample Rate Reduction, Bit Depth),
    // 13/15/17 (Flanger's Delay Time, Mix, Feedback), 19 (Volume) -- these
    // open a mode-choice submenu FIRST (Static/Sweep In/Sweep Out, or for
    // Volume Static/Ramp Up/Ramp Down).
    static bool isSequencerCellParameterSwept (int index);

    static bool isSequencerCellParameterDiscrete (int index);
    static int getSequencerCellParameterNumOptions (int index); // only meaningful when isSequencerCellParameterDiscrete() is true
    static juce::String getSequencerCellParameterOptionName (int index, int optionIndex);

    // Subdivide (Step 47) alone: discrete like Filter Type/Curve Shape but
    // presented via the SAME drag-slider overlay Resonance/Grain Size/Grain
    // Speed use rather than a plain list submenu.
    static bool isSequencerCellParameterSteppedSlider (int index);

    static float getSequencerCellParameterMin (int index);
    static float getSequencerCellParameterMax (int index);

    // Which parameters above are relevant to a given cell style.
    static std::vector<int> getApplicableSequencerCellParameters (int style);

    // This parameter's current GLOBAL value (i.e. what applies when no
    // per-step override exists) -- used as the slider overlay's fallback.
    // Not static (unlike the helpers above) since it reads live atomic
    // state.
    float getSequencerCellParameterGlobalValue (int index) const;

    // Writes this parameter's GLOBAL value (the mirror-image dispatcher of
    // getSequencerCellParameterGlobalValue() above). A no-op for Subdivide
    // (index 5) and Volume/Volume Mode (indices 19/20), none of which have
    // a global dial.
    void setSequencerCellParameterGlobalValue (int index, float value);

    bool getSequencerCellHasParameterOverride (int row, int column, const juce::String& parameterName) const;
    float getSequencerCellParameterOverride (int row, int column, const juce::String& parameterName, float fallbackValue) const;
    void setSequencerCellParameterOverride (int row, int column, const juce::String& parameterName, float value);

    // True if the cell has ANY parameter override at all -- drives the
    // small corner marker SequencerGrid draws on customized steps.
    bool getSequencerCellHasAnyParameterOverride (int row, int column) const;

    // Lock-free copy of the currently active step column, for the UI's
    // playhead indicator on the sequencer grid. -1 when Sequenced mode
    // isn't active (or transport stopped). Written by the engine.
    int getCurrentlyPlayingStepIndex() const { return currentlyPlayingStepIndexForUI.load(); }

    struct SequencerPatternSnapshot; // defined below; used by the pattern-bank API above it

    //=== MIDI input / Sequencer pattern bank (Pass 1/2) ===
    // A 128-slot, lazily-populated bank indexed 1:1 by MIDI note number,
    // each slot holding a complete snapshot of the Sequencer grid. Slots
    // are populated lazily, only via MIDI Learn: armMidiLearnForPatternSave()
    // captures the CURRENT grid immediately, then the next note-on received
    // while armed claims that slot. None of this is persisted yet.
    void armMidiLearnForPatternSave(); // captures the current grid; takes sampleLock itself -- UI-thread entry point
    void cancelMidiLearn();
    bool isMidiLearnArmed() const { return midiLearnArmed.load(); }

    // Captures rows/columns/stepResolutionIndex/patternLengthBarsIndex + the
    // full grid/overrides/extended lengths into a SequencerPatternSnapshot;
    // the data backing armMidiLearnForPatternSave() and the pattern bank.
    SequencerPatternSnapshot captureCurrentSequencerPatternSnapshot() const;

    // One locked snapshot copy per call, cheap enough for a UI timer to
    // poll at a modest rate without hammering sampleLock 128 times a tick.
    std::array<bool, 128> getPopulatedPatternBankSlots() const;

    // Pattern Switch Timing (Pass 2) -- governs WHEN a recall note-on for a
    // populated slot actually takes effect.
    //   immediate    -- switches the instant the note-on arrives.
    //   setInterval  -- defers to the next occurrence of a chosen musical
    //     grid point (checked every sample against host ppq in
    //     processBlock()).
    //   endOfPattern -- defers to the moment the currently playing pattern
    //     wraps at its own Pattern Length.
    enum class PatternSwitchTiming { immediate, setInterval, endOfPattern };

    void setPatternSwitchTiming (PatternSwitchTiming timing)
    {
        // Side-effect note: the engine's SlicerEngine::setPatternSwitchTiming()
        // wrapper (the UI's real entry point) additionally abandons any
        // deferred switch still pending under the old timing mode.
        patternSwitchTiming.store (timing);
    }

    PatternSwitchTiming getPatternSwitchTiming() const { return patternSwitchTiming.load(); }

    // Set Interval's grid point -- same numNoteValueOptions palette as
    // Clock reference/Step resolution.
    void setPatternSwitchIntervalIndex (int index)
    {
        patternSwitchIntervalIndex.store (juce::jlimit (0, numNoteValueOptions - 1, index));
    }

    int getPatternSwitchIntervalIndex() const { return patternSwitchIntervalIndex.load(); }

    //=== Performance mode state bank (click-to-focus + auto-save) ===
    // A 128-note-indexed, lazily-populated bank, each slot a self-
    // contained PERFORMANCE STATE: one hand-trimmed segment, one
    // PlaybackStyle, that style's own independent 21-parameter values, a
    // Loop toggle, and a Sync toggle. Editing focus is set exclusively by
    // clicking a key on Performance mode's on-screen keyboard; physical
    // MIDI is playback-only.
    struct PerformanceStateSnapshot
    {
        bool populated = false;
        int trimStartSample = 0, trimEndSample = 0;
        int style = 0; // PlaybackStyle index
        std::array<float, numSequencerCellParameters> parameterValues {}; // indexed exactly as getSequencerCellParameterName()
        bool loop = false;
        bool sync = true;
    };

    // One pattern bank slot's full contents (Step: MIDI pattern bank) --
    // see captureCurrentSequencerPatternSnapshot()/patternBank below for how
    // it's populated and read.
    struct SequencerPatternSnapshot
    {
        bool populated = false;
        int rows = 0, columns = 0;
        int stepResolutionIndex = 0, patternLengthBarsIndex = 0;
        std::vector<int> grid;
        std::map<int, std::map<juce::String, float>> parameterOverrides;
        std::map<int, int> extendedLengthSteps;
    };

    // Click-to-focus: moves editing focus to noteNumber, auto-saving
    // whatever was being edited in the previously-focused slot first, then
    // loading noteNumber's own saved state (or a fresh default) into
    // performanceWorkingState + the shared trim atomics. UI-thread entry
    // point; takes sampleLock itself.
    void setFocusedPerformanceStateSlot (int noteNumber);
    int getFocusedPerformanceStateSlot() const { return focusedPerformanceStateSlot.load(); } // -1 = nothing focused yet

    std::array<bool, 128> getPopulatedPerformanceStateBankSlots() const;

    // Quantize Recall -- same mechanism as Pattern Switch Timing's Set
    // Interval mode, applied to Performance mode's MIDI state recall. Off
    // (immediate, the original behaviour) by default.
    void setPerformanceQuantizeRecallEnabled (bool enabled)
    {
        // Side-effect note: the engine's SlicerEngine::
        // setPerformanceQuantizeRecallEnabled() wrapper (the UI's real entry
        // point) additionally abandons any recall still pending under the old
        // setting.
        performanceQuantizeRecallEnabled.store (enabled);
    }

    bool getPerformanceQuantizeRecallEnabled() const { return performanceQuantizeRecallEnabled.load(); }

    void setPerformanceQuantizeRecallIntervalIndex (int index)
    {
        performanceQuantizeRecallIntervalIndex.store (juce::jlimit (0, numNoteValueOptions - 1, index));
    }

    int getPerformanceQuantizeRecallIntervalIndex() const { return performanceQuantizeRecallIntervalIndex.load(); }

    // The "working state" -- style/params/loop/sync currently being edited
    // via the parameter panel/the Loop+Sync toggles, ahead of whatever the
    // next Save captures. Parameter values are seeded once, in the
    // constructor, from getSequencerCellParameterGlobalValue().
    int getPerformanceWorkingStyle() const;
    void setPerformanceWorkingStyle (int style);
    float getPerformanceWorkingParameterValue (int index) const;
    void setPerformanceWorkingParameterValue (int index, float value);
    bool getPerformanceWorkingLoop() const;
    void setPerformanceWorkingLoop (bool loop);
    bool getPerformanceWorkingSync() const;
    void setPerformanceWorkingSync (bool sync);

    //=== State persistence (nextsteps 1.4) ===
    // Full instrument-state serialization -- everything except the audio
    // sample itself (transient by design, see
    // docs/state-serialization-decision.md): global parameters, probability
    // tables, sequencer grid + per-cell overrides, the 128-slot pattern
    // bank, the 128-slot performance state bank, and the performance
    // working state. XML-encoded for the dev stage, behind these methods so
    // the encoding is a swappable detail (the processor's
    // getStateInformation/setStateInformation are the only other callers).
    // Message-thread entry points; both take sampleLock. restoreState()
    // ignores unknown tags, leaves absent sections unchanged, clamps every
    // index to its valid range, size-guards the slice-indexed tables (only
    // meaningful once the matching sample is loaded), and ends with
    // onPickStateInvalidated() under the lock.
    void saveState (juce::MemoryBlock& destData);
    void restoreState (const void* data, int sizeInBytes);

    //=== Audio-thread data path (engine support) ===
    // The engine (processBlock() and the MIDI/audition helpers) reads these
    // directly. They live here because they're pure
    // model state: the loaded sample, its detected/manual slices, and the
    // per-pick/recall structures the engine mutates.
    void loadSample (const juce::File& file);
    void redetectSlices (float sensitivity, float holdoffMs);
    double computeSourceSpanSeconds() const;
    void rebuildSlicesFromDetectionAndManualPoints (float sensitivity, float holdoffMs);
    std::vector<Slice> mergeOnsetsIntoSlices (const std::vector<Slice>& autoSlices, int trimStart, int trimEnd) const;
    int quantizeOnsetToGrid (int onsetSample, int trimStart, int trimEnd) const;
    int findNearestGridSample (int rawSample) const;
    void resetSequencerGrid();
    bool shouldGridSnapTrim() const
    {
        return triggerMode.load() == TriggerMode::performance
            && performanceTrimSnapMode.load() == TrimSnapMode::grid;
    }

    // Tempo-relative minimum holdoff between consecutive detected
    // transients, replacing the old fixed defaultHoldoffMs floor everywhere
    // detection actually runs (see the call sites below). At max
    // sensitivity, a fixed ms floor lets detection density run away on fast
    // material and stay needlessly sparse on slow material — neither
    // bounded by anything musical. This instead never allows two onsets
    // closer than roughly a 32nd note apart AT THE LOOP'S OWN CALCULATED
    // TEMPO (getCalculatedOriginalBpm(), the same trim/bars/manual-override
    // -aware derivation used everywhere else tempo matters in this class),
    // so density scales with how fast the material actually is instead of
    // an arbitrary constant. A 32nd note is 1/8 of a quarter-note beat
    // (assumes 4/4, same assumption used throughout); 60000/bpm is one
    // beat in ms.
    //
    // Falls back to the old fixed defaultHoldoffMs when there's no usable
    // tempo yet (bpm <= 0 -- no sample loaded, or a degenerate span), so
    // behaviour before a sample loads is unchanged. Also floors at 1ms as a
    // numerical safety net (not a musical one) against an absurd manual BPM
    // override collapsing the holdoff to ~0 and effectively disabling it.
    static constexpr float defaultHoldoffMs = 30.0f;

    float computeMinimumHoldoffMs() const
    {
        const double bpm = getCalculatedOriginalBpm();

        if (bpm <= 0.0)
            return defaultHoldoffMs;

        constexpr double thirtySecondNoteFractionOfBeat = 1.0 / 8.0;
        const double beatMs = 60000.0 / bpm;
        return (float) juce::jmax (1.0, beatMs * thirtySecondNoteFractionOfBeat);
    }

    // Complete MIDI Learn: stores the captured snapshot in noteNumber's
    // slot and disarms. Assumes sampleLock is already held by the caller
    // (the engine's dispatchNoteOn(), which runs from processBlock()).
    void completeMidiLearn (int noteNumber);

    //=== Shared audio state (public for Phase 1; see the class doc) ===
    juce::CriticalSection sampleLock; // guards sampleBuffer/slices during loadSample() and the probability/sequencer tables

    // Slice detection + manual/excluded points + undo.
    TransientDetector transientDetector;
    std::vector<ManualPointInfo> manualPoints;
    int nextManualPointId = 1;
    std::vector<ManualPointInfo> excludedPoints;
    int nextExcludedPointId = 1;
    juce::UndoManager undoManager;

    // Sample + slices.
    juce::AudioFormatManager formatManager;
    juce::AudioBuffer<float> sampleBuffer;
    double sampleSampleRate = 44100.0;
    bool sampleLoaded = false;
    juce::String loadedFileName;
    std::vector<Slice> slices;

    // Probability tables.
    std::vector<float> sliceProbabilities;
    std::vector<float> subdivisionProbabilities;
    std::vector<float> playbackStyleProbabilities;

    // Trim markers -- see the accessors above. tempoTrim* are the copies
    // pinned to the last REAL trim edit (Performance mode repoints the
    // live trimStartSample/trimEndSample at whichever slot has focus, but
    // tempo keeps measuring against one stable span).
    std::atomic<int> loopLengthBars { 1 };
    std::atomic<int> trimStartSample { 0 };
    std::atomic<int> trimEndSample { 0 };
    std::atomic<int> tempoTrimStartSample { 0 };
    std::atomic<int> tempoTrimEndSample { 0 };

    // Audition (written by the engine's renderAudition(), guarded by
    // sampleLock). The engine owns the read cursor itself
    // (SlicerEngine::auditionPosition); the model keeps the UI-facing
    // active flag and lock-free playhead copy.
    std::atomic<bool> auditionActive { false };
    std::atomic<int> auditionPlaybackPositionForUI { -1 };

    std::atomic<bool> manualBpmOverrideEnabled { false };
    std::atomic<double> manualBpmOverrideValue { 120.0 };
    std::atomic<float> currentSensitivity { defaultSensitivity };
    std::atomic<float> fadeInMs { 5.0f };
    std::atomic<float> fadeOutMs { 15.0f };

    std::atomic<bool> quantizeTransientsEnabled { false };
    std::atomic<int> quantizeGridIndex { 13 };

    std::atomic<TrimSnapMode> performanceTrimSnapMode { TrimSnapMode::transients };
    std::atomic<int> performanceTrimGridIndex { 13 };
    std::atomic<TriggerMode> triggerMode { TriggerMode::sliceLength };
    std::atomic<int> clockReferenceIndex { 13 };
    std::atomic<TapeStopScope> tapeStopScope { TapeStopScope::wholeWindow };
    std::atomic<FilterSweepScope> filterSweepScope { FilterSweepScope::perTick };
    std::atomic<int> resetBarsIndex { 2 };

    // Sequencer grid + per-cell parameter overrides + step extensions.
    std::vector<int> sequencerGrid;
    std::map<int, std::map<juce::String, float>> sequencerCellParameterOverrides;
    std::map<int, int> sequencerCellExtendedLengthSteps;

    std::atomic<int> stepResolutionIndex { 7 };
    std::atomic<int> patternLengthBarsIndex { 0 };

    std::atomic<int> selectedDrawingStyle { 0 };

    // Pattern bank (Sequenced mode).
    std::array<SequencerPatternSnapshot, 128> patternBank;
    SequencerPatternSnapshot pendingSaveSnapshot;
    std::atomic<bool> midiLearnArmed { false };

    std::atomic<PatternSwitchTiming> patternSwitchTiming { PatternSwitchTiming::immediate };
    std::atomic<int> patternSwitchIntervalIndex { 19 };

    // Performance state bank + working state.
    std::array<PerformanceStateSnapshot, 128> performanceStateBank;
    PerformanceStateSnapshot performanceWorkingState;
    std::atomic<int> focusedPerformanceStateSlot { -1 }; // -1 = nothing focused yet

    std::atomic<bool> performanceQuantizeRecallEnabled { false };
    std::atomic<int> performanceQuantizeRecallIntervalIndex { 13 };

    // Parameter values (Slice Length/Clock globals).
    std::atomic<float> stretchGrainSizeMsValue { defaultStretchGrainSizeMs };
    std::atomic<float> stretchSpeedMultiplierValue { defaultStretchSpeedMultiplier };
    std::atomic<float> filterSweepResonanceValue { defaultFilterSweepResonance };
    std::atomic<int> filterSweepFilterTypeValue { 0 };
    std::atomic<int> curveShapeValue { 0 };
    std::atomic<float> bitcrushRateReductionGlobalValue { 12.0f };
    std::atomic<int> bitcrushRateReductionModeGlobalValue { 0 };
    std::atomic<float> bitcrushBitDepthGlobalValue { 5.0f };
    std::atomic<int> bitcrushBitDepthModeGlobalValue { 0 };
    std::atomic<int> scratchRateGlobalValue { 7 }; // 16n, matching scratchDefaultRateIndex
    std::atomic<int> scratchForwardCurveGlobalValue { 0 };
    std::atomic<int> scratchBackwardCurveGlobalValue { 0 };
    std::atomic<float> flangerDelayTimeGlobalValue { 2.0f };
    std::atomic<int> flangerDelayTimeModeGlobalValue { 0 };
    std::atomic<float> flangerMixGlobalValue { 0.5f };
    std::atomic<int> flangerMixModeGlobalValue { 0 };
    std::atomic<float> flangerFeedbackGlobalValue { 0.3f };
    std::atomic<int> flangerFeedbackModeGlobalValue { 0 };

    // Pitch/Grain/quantize settings.
    std::atomic<PitchMode> pitchMode { PitchMode::repitch };
    std::atomic<float> grainSizeMs { 60.0f };
    std::atomic<GrainWindowShape> grainWindowShape { GrainWindowShape::hann };
    std::atomic<float> pitchShiftSemitones { 0.0f };
    std::atomic<bool> beatQuantizeSliceLengthEnabled { true };
    std::atomic<bool> beatQuantizeSliceLengthEnabledRepitch { false };

    // UI telemetry, written by the audio-thread engine.
    std::atomic<int> currentlyPlayingSliceIndexForUI { -1 };
    std::atomic<int> currentlyPlayingStepIndexForUI { -1 };

    // Engine invalidation hook -- see the class doc comment at the top.
    std::function<void()> onPickStateInvalidated;

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SlicerModel)
};
