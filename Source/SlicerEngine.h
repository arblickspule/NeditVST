#pragma once

#include <JuceHeader.h>
#include "GranularStretcher.h"
#include "EasingCurve.h"
#include "SlicerModel.h"
#include <array>
#include <atomic>
#include <vector>

//==============================================================================
// The real-time audio core (Phase 2) -- SlicerEngine owns everything the
// audio thread touches that isn't shared model state: the per-pick playback
// state (currentPick*/clock/reset/sequenced/performance scheduling) and the
// effect buffers (filter/bitcrush/flanger) and Random sources.
// SlicerAudioProcessor keeps the plugin plumbing (buses, editor, state
// serialization) and forwards the public API here.
//
// model is a public reference, fixed at construction -- the engine never
// outlives the SlicerModel it was built with (the processor owns both, in
// declaration order). processBlock() takes model.sampleLock itself, exactly
// as the old in-processor body did; the processor's own processBlock()
// samples the playhead/transport state before calling in.
//==============================================================================
class SlicerEngine
{
public:
    SlicerEngine (SlicerModel& modelToUse);
    ~SlicerEngine();

    // Shared audio state + model.sampleLock.
    SlicerModel& model;

    using TriggerMode = SlicerModel::TriggerMode;
    using PitchMode = SlicerModel::PitchMode;
    using PlaybackStyle = SlicerModel::PlaybackStyle;
    using FilterSweepScope = SlicerModel::FilterSweepScope;
    using PatternSwitchTiming = SlicerModel::PatternSwitchTiming;
    using PerformanceStateSnapshot = SlicerModel::PerformanceStateSnapshot;
    using GrainWindowShape = SlicerModel::GrainWindowShape;
    using TapeStopScope = SlicerModel::TapeStopScope;

    // Sample-rate-dependent setup -- forwarded from the processor's
    // prepareToPlay() (called there, same as before this class existed).
    void prepare (double sampleRate, int samplesPerBlock);

    // The real-time render call. position/hasPlayHead/hostTransportPlaying/
    // hostSampleRate are computed by the processor ahead of the lock (host
    // reads only); this body is byte-for-byte the old processBlock() DSP,
    // including taking model.sampleLock itself.
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages,
                       const juce::Optional<juce::AudioPlayHead::PositionInfo>& position,
                       bool hasPlayHead, bool hostTransportPlaying, double hostSampleRate);

    void setTriggerMode (TriggerMode mode);
    void setPitchMode (PitchMode mode);
    bool getRandomizeParametersForStyle (int index) const;
    void setRandomizeParametersForStyle (int index, bool shouldRandomize);
    void randomizeSequence();

    // Audition (Step 25) -- the UI's entry point, wrapping the model's
    // plain setAuditionActive() store: re-arms this engine's read cursor at
    // the current trim start on activation (the model itself no longer owns
    // the cursor, so this is where "start fresh from the current trim, not
    // wherever a stale position was left" lives now).
    void setAuditionActive (bool active);

    // Pattern Switch Timing / Quantize Recall -- the UI's entry points.
    // These wrap the model's plain setters with the engine-owned side
    // effect those setters used to do inline: changing the timing mode /
    // recall setting abandons any deferred switch/recall still pending
    // under the old one. (The pending notes themselves are engine state
    // now -- see the private members below.)
    void setPatternSwitchTiming (PatternSwitchTiming timing);
    void setPerformanceQuantizeRecallEnabled (bool enabled);

    // -1 if no slot has been recalled this session (pattern bank, Sequenced
    // mode). Moved here with the recall state in the layering refactor.
    int getActivePatternBankSlot() const { return activePatternBankSlot.load(); }

    // -1 if no switch is currently pending; otherwise the MIDI note number
    // (== pattern bank slot) a deferred switch is headed toward.
    int getPendingPatternSwitchSlot() const { return pendingPatternSwitchNote.load(); }

    // -1 if no recall is currently pending; otherwise the MIDI note number
    // (== performance state bank slot) a deferred recall is headed toward.
    int getPendingPerformanceRecallSlot() const { return pendingPerformanceRecallNote.load(); }

    // The audition read cursor, mutated every sample in renderAudition().
    // Moved here from the model in the layering refactor (it was model-
    // owned only because the model's setAuditionActive() used to touch it;
    // SlicerEngine::setAuditionActive() now re-arms it instead). Guarded by
    // model.sampleLock exactly like the rest of renderAudition's state, and
    // public because the existing processBlock() tests seed it directly.
    double auditionPosition = 0.0;

public:
    // Pure helper functions -- exposed for unit testing (Phase 3). The UI
    // and processBlock() both depend on this math, so it gets direct
    // coverage rather than only indirect smoke coverage; these are
    // read-only and safe to call from any thread.
    // Weighted-random pick across a list of weights. Falls back to
    // uniform-random if every weight is 0 (rather than picking nothing
    // and stalling). Used for both slice selection and, in Clock mode,
    // subdivision selection — same math, different weight lists. A
    // zero-or-negative-weight entry is never selected (see the skip +
    // strict-< test below -- guards the edge case where random.nextFloat()
    // returns exactly 0.0 and would otherwise match a leading zero-weight
    // entry's cumulative boundary).
    int pickWeightedIndex (const std::vector<float>& weights)
    {
        if (weights.empty())
            return -1;

        float totalWeight = 0.0f;
        int lastPositiveIndex = -1;

        for (size_t i = 0; i < weights.size(); ++i)
        {
            if (weights[i] > 0.0f)
            {
                totalWeight += weights[i];
                lastPositiveIndex = (int) i;
            }
        }

        if (totalWeight <= 0.0f)
            return random.nextInt ((int) weights.size());

        const float target = random.nextFloat() * totalWeight;
        float cumulative = 0.0f;

        for (size_t i = 0; i < weights.size(); ++i)
        {
            if (weights[i] <= 0.0f)
                continue; // never selectable, regardless of where target lands

            cumulative += weights[i];

            if (target < cumulative)
                return (int) i;
        }

        return lastPositiveIndex; // float-rounding fallback -- always a positive-weight entry
    }

    int pickWeightedRandomSlice() { return pickWeightedIndex (model.sliceProbabilities); }

    // Maps a model.playbackStyleProbabilities index (as drawn by pickWeightedIndex)
    // to its enum value. A plain out-of-range/negative index (shouldn't
    // happen — the table always has SlicerModel::numPlaybackStyleOptions entries) falls
    // back to Forward rather than asserting, matching pickWeightedIndex's
    // own defensive style elsewhere.
    static PlaybackStyle indexToPlaybackStyle (int index)
    {
        if (index == 1) return PlaybackStyle::pingPong;
        if (index == 2) return PlaybackStyle::tapeStop;
        if (index == 3) return PlaybackStyle::stretch;
        if (index == 4) return PlaybackStyle::filterSweepDown;
        if (index == 5) return PlaybackStyle::filterSweepUp;
        if (index == 6) return PlaybackStyle::bitcrush;
        if (index == 7) return PlaybackStyle::scratch;
        if (index == 8) return PlaybackStyle::flanger;
        return PlaybackStyle::forward;
    }

    // Beat-quantized slice length (Step 24): finds the note-value palette
    // entry (see numNoteValueOptions/getNoteValueBeats() above) closest to
    // targetBeats. Reuses the existing palette directly rather than
    // duplicating it.
    static int nearestNoteValueIndex (double targetBeats);

    // Beat-quantized slice length (Step 24/26) — the shared target-duration
    // calculation both the Time-Stretch and Repitch toggles feed into, so
    // it's computed once here rather than duplicated per pitch mode:
    //   1. naturalBeats = (slice length in source seconds) / (60 / originalBpm)
    //      — Ping-Pong passes pingPong=true, using the FULL ROUND TRIP
    //      (2x sliceLength) as the span whose duration should land on the
    //      beat grid.
    //   2. Snap to the nearest note-value palette entry (nearestNoteValueIndex).
    //   3. targetHostSeconds = quantizedBeats * (60 / hostBpm)
    //   4. stretchRatio = sliceNaturalSourceSeconds / targetHostSeconds —
    //      this pick's own replacement for the global repitchRatio.
    // result.quantized stays false (stretchRatio/targetHostSeconds
    // meaningless) if sliceLength/originalBpm/hostBpm/targetHostSeconds
    // are degenerate (<= 0) — callers check this before using the rest.
    struct BeatQuantizeResult
    {
        bool quantized = false;
        double stretchRatio = 1.0;      // replaces repitchRatio for this pick
        double targetHostSeconds = 0.0; // this pick's target duration, in host seconds
    };

    static BeatQuantizeResult computeBeatQuantizeTarget (int sliceLength, bool pingPong,
                                                          double sampleSampleRate, double originalBpm, double hostBpm);

    // Scratch (v1): this pick's bounce-cycle length (one full forward-
    // backward cycle at its own Rate note value), in HOST samples --
    // shared by every trigger mode's own pick-start duration/currentEndSample
    // calculation and, converted back to source-domain samples via
    // playbackRate, the fold length actually passed to
    // GranularStretcher::foldPosition() (see processBlock()'s shared
    // bounceFoldLengthSamples). Clamped so one LEG of the cycle (half of
    // it) never exceeds the slice's own content length -- Rate is
    // tempo-synced and completely independent of slice length, so an
    // unclamped cycle could otherwise ask foldPosition to bounce past the
    // slice's actual audio into whatever follows it in the buffer.
    // Degenerate (<= 0) sliceLength/hostBpm/hostSampleRate/playbackRate
    // returns 0.0 rather than dividing by zero -- callers already treat a
    // zero cycle length as a harmless no-op (foldPosition's own
    // sliceLength <= 0.0 guard falls back to plain Forward).
    static double computeScratchCycleLengthHostSamples (int rateIndex, int sliceLength,
                                                          double hostBpm, double hostSampleRate, double playbackRate);

private:
    // MIDI input dispatch layer -- see the public "MIDI input / Sequencer
    // pattern bank" section above for the overall design. All of these,
    // plus completeMidiLearn()/handleSequencerPatternRecallNoteOn() below,
    // are only ever called from within processBlock() and assume model.sampleLock
    // is already held by the caller (same convention resetSequencerGrid()
    // itself follows) -- they must never take the lock themselves.
    // hostTransportPlaying is threaded through from processBlock() (computed
    // there ahead of this call, see its own call site) purely so Quantize
    // Recall's note-on handler can decide immediate-vs-deferred on the spot
    // -- every other handler ignores it.
    void handleIncomingMidi (const juce::MidiBuffer& midiMessages, bool hostTransportPlaying);
    void dispatchNoteOn (int noteNumber, bool hostTransportPlaying);

    // Sequenced mode's recall entry point (Pass 2) -- routes by
    // model.patternSwitchTiming: immediate applies handleSequencerPatternRecallNoteOn()
    // below right away (unchanged Pass 1 behaviour); setInterval/endOfPattern
    // instead arm pendingPatternSwitchNote and let the per-sample boundary
    // check in processBlock()'s sequencedMode branch apply the switch once
    // the chosen boundary is crossed. Empty slot -> no-op either way, same
    // as Pass 1.
    void handlePatternSwitchNoteOn (int noteNumber);

    // The actual grid swap, called either directly (Immediate) or from the
    // deferred per-sample boundary check (Set Interval/End of Pattern). A
    // populated slot overwrites the live grid AND its defining dimensions
    // (model.stepResolutionIndex/model.patternLengthBarsIndex) wholesale, then forces
    // sequencedModeInitialized false so the step-boundary tracker re-syncs
    // against the new grid on the very next check -- the same "just
    // switched, trigger immediately" mechanic setTriggerMode() already
    // relies on for a mode change. Callers invoking this MID-BLOCK (the
    // deferred timing modes) must also set sequencedModeInitialized back to
    // true afterward once they've re-derived this sample's step alignment
    // themselves -- see the sequencedMode branch in processBlock() -- since
    // otherwise the NEXT block would re-trigger the same step a second time
    // (sequencedModeInitialized only gets consulted once per block, and
    // this call happens after that block's own check already ran).
    void handleSequencerPatternRecallNoteOn (int noteNumber);

    // Writes model.pendingSaveSnapshot (captured back when armMidiLearnForPatternSave()
    // was clicked) into model.patternBank[noteNumber] and clears model.midiLearnArmed.
    void completeMidiLearn (int noteNumber) { model.completeMidiLearn (noteNumber); }

    // Performance mode's own note-on entry point -- dispatched from
    // dispatchNoteOn()'s TriggerMode::performance case. An empty, unfocused
    // slot is a no-op regardless of Quantize Recall, same as Pass 1 (checked
    // up front here before either path below runs). Otherwise routes by
    // model.performanceQuantizeRecallEnabled, exactly the same "immediate vs.
    // arm-and-defer" split handlePatternSwitchNoteOn() uses for
    // model.patternSwitchTiming:
    //   off, OR on but the host transport isn't playing right now (no
    //     meaningful beat position to quantize against without it) --
    //     applies immediately via applyPerformanceStateRecall(), unchanged
    //     Pass 1 behaviour.
    //   on AND transport playing -- arms pendingPerformanceRecallNote and
    //     lets the per-sample boundary check in processBlock()'s
    //     performanceMode branch apply it once the chosen grid point is
    //     reached. A newer note-on before that point just overwrites the
    //     same atomic, so the newest press always wins -- never more than
    //     one recall pending at a time.
    void handlePerformanceStateNoteOn (int noteNumber, bool hostTransportPlaying);

    // The actual focus/playback-source swap, called either directly
    // (Quantize Recall off, or transport stopped) or from the deferred
    // per-sample boundary check (Quantize Recall on). Re-checks the target
    // slot's populated flag for the non-focused case (mirrors
    // handleSequencerPatternRecallNoteOn()'s own re-check) since a deferred
    // call runs an arbitrary amount of time after the note-on that armed
    // it -- moved out of handlePerformanceStateNoteOn() itself so both call
    // sites (immediate and deferred) share exactly one implementation.
    void applyPerformanceStateRecall (int noteNumber);

    // Unifies the tempo math (Step 23) that both Trim markers and Manual
    // BPM override feed into:
    //   sourceSpanSeconds = model.manualBpmOverrideEnabled
    //       ? (model.loopLengthBars * 4 * 60) / model.manualBpmOverrideValue
    //       : (model.tempoTrimEndSample - model.tempoTrimStartSample) / sampleSampleRate
    // Used by both getCalculatedOriginalBpm() (the UI's "~X BPM" label) and
    // processBlock()'s repitchRatio — replaces the old calculation, which
    // used the whole buffer's length regardless of trim (the bug this
    // fixes). The existing repitchRatio formula itself (sourceSpanSeconds /
    // hostLoopLengthSeconds) is otherwise unchanged, and GranularStretcher
    // never computes tempo itself — it only ever receives the ratios
    // (repitchRatio, srConversionRatio) this feeds into, so it stays
    // consistent with the direct-read path "for free."
    //
    // Deliberately reads model.tempoTrimStartSample/model.tempoTrimEndSample, NOT the
    // plain model.trimStartSample/model.trimEndSample -- see model.tempoTrimStartSample's own
    // comment (Performance mode's per-state trim fix). The two pairs are
    // identical outside Performance mode; they only diverge once Performance
    // mode starts repointing the shared trim atomics at whichever state slot
    // has editing focus, which must NOT feed back into "the sample's
    // original tempo."

    // Audition (Step 25) — the raw, generative-engine-bypassing loop
    // render. Called from processBlock() (model.sampleLock already held) in
    // place of everything below it whenever model.auditionActive is set.
    // Reads/writes auditionPosition (declared above in the public section);
    // safe from the UI thread too only because setAuditionActive() (and its
    // model wrapper) take the same lock.
    void renderAudition (juce::AudioBuffer<float>& buffer, double hostSampleRate);

    juce::Random random;

    std::vector<bool> randomizeParametersForStyle = std::vector<bool> ((size_t) SlicerModel::numPlaybackStyleOptions, false); // see getRandomizeParametersForStyle()'s own doc comment

    // Filter Down/Filter Up (Step 29/30) character parameters —
    // filterSweepStartHz/filterSweepEndHz remain deliberately fixed, no
    // exposed controls, same "defer the knob until proven necessary"
    // pattern as Stretch's grain size above. filterSweepStartHz/
    // filterSweepEndHz are Filter Down's open->closed endpoints (the
    // classic breakbeat/DnB "filter close"); Filter Up just swaps which
    // endpoint it starts/ends at (see processBlock()) -- no separate
    // constants needed. Resonance is no longer fixed -- see
    // model.filterSweepResonanceValue below (Step 45). One shared filter
    // instance is fine — Playback Style is a single mutually-exclusive
    // pick per pick, so it's never touched by more than one pick's
    // processing at a time.
    static constexpr float filterSweepStartHz = 9000.0f;
    static constexpr float filterSweepEndHz = 250.0f;
    juce::dsp::StateVariableTPTFilter<float> filterSweepFilter;

    // Bitcrush state (Step 48/49) -- sample-and-hold downsampler needs to
    // remember the last "grabbed" value per channel and how many samples
    // are left before the next grab, both reset alongside filterSweepFilter
    // in the shared pickJustStarted block so a new pick never inherits the
    // previous pick's hold phase or held value. The hold LENGTH and bit
    // DEPTH themselves are no longer fixed constants (Step 49 made them
    // per-step adjustable, each with its own Static/Sweep In/Sweep Out
    // mode -- see currentPickBitcrushRateValue/Mode and
    // currentPickBitcrushBitDepthValue/Mode above), just this held-sample
    // bookkeeping is unaffected by that -- it doesn't care WHY the hold
    // length changed from one grab to the next, only that it did.
    float bitcrushHeldSample[GranularStretcher::maxChannels] = {};
    int bitcrushHoldCounter = 0;

    // Flanger delay line -- a short circular buffer per channel (with its
    // own adjustable Feedback amount feeding the delayed signal back into
    // the line, see applyFlanger() in processBlock()), sized in
    // prepareToPlay() to comfortably hold
    // flangerDelayTimeExtremeMs of audio at the real host sample rate
    // (sample rate isn't known at construction, same reason
    // filterSweepFilter itself is only prepare()'d there rather than at
    // construction). Reset (cleared, write index rewound to 0) alongside
    // bitcrushHeldSample/bitcrushHoldCounter above in the shared
    // pickJustStarted block, so a fresh pick's comb character starts from
    // silence rather than inheriting whatever the previous pick (of any
    // style) left sitting in the line -- same "self-contained within one
    // pick's lifetime" contract every other style's per-pick state already
    // has.
    juce::AudioBuffer<float> flangerDelayBuffer;
    int flangerDelayWriteIndex = 0;

    // This pick's own resonance/filter type/curve shape (Step 45/46),
    // captured once at pick-start by every trigger mode (Slice Length/
    // Clock always capture the global value; Sequenced mode captures its
    // step's own override if present, else the global value) and applied
    // once in the shared pickJustStarted block (resonance/filter type) or
    // consulted directly during rendering (curve shape) below --
    // audio-thread-only state, same pattern as currentPickBeatQuantized/
    // currentPickTapeStopDurationHostSamples.
    float currentPickFilterSweepResonance = SlicerModel::defaultFilterSweepResonance;
    int currentPickFilterSweepType = 0;
    int currentPickCurveShape = 0;

    // This pick's own Stretch grain size/speed (Step 46), same capture
    // pattern as currentPickFilterSweepResonance just above -- Slice
    // Length/Clock always capture the global value; Sequenced mode
    // captures its step's own override if present. Harmless (unused) for
    // every style but Stretch, same as currentPickTapeStopDurationHostSamples
    // is for everything but Tape Stop.
    float currentPickStretchGrainSizeMs = SlicerModel::defaultStretchGrainSizeMs;
    float currentPickStretchSpeedMultiplier = SlicerModel::defaultStretchSpeedMultiplier;

    // This pick's own Bitcrush Sample Rate Reduction/Bit Depth VALUE and
    // MODE (Step 49), same capture pattern as currentPickFilterSweepResonance
    // above -- Slice Length/Clock always capture the fixed default value
    // with Static mode (no global dial for either, same as Subdivide);
    // Sequenced mode captures its step's own override if present. Harmless
    // (unused) for every style but Bitcrush. Mode is Static (0) rather
    // than an enum class -- consulted only via plain int comparison in
    // processBlock's sweep-interpolation lambda, same convention
    // currentPickFilterSweepType/currentPickCurveShape already use for
    // their own small fixed option sets.
    float currentPickBitcrushRateValue = 0.0f;
    int currentPickBitcrushRateMode = 0;
    float currentPickBitcrushBitDepthValue = 0.0f;
    int currentPickBitcrushBitDepthMode = 0;

    // This pick's own Flanger Delay Time/Mix/Feedback VALUE and MODE,
    // identical capture pattern to currentPickBitcrushRateValue/Mode
    // above -- Slice Length/Clock always capture the fixed default value
    // with Static mode (no global dial for any of the three); Sequenced
    // mode captures its step's own override if present. Harmless (unused)
    // for every style but Flanger.
    float currentPickFlangerDelayValue = 0.0f;
    int currentPickFlangerDelayMode = 0;
    float currentPickFlangerMixValue = 0.0f;
    int currentPickFlangerMixMode = 0;
    float currentPickFlangerFeedbackValue = 0.0f;
    int currentPickFlangerFeedbackMode = 0;

    // This pick's own Volume ramp VALUE and MODE (style-independent) --
    // captured ONLY by Sequenced mode's step-trigger block, unlike every
    // other currentPick* pair above, since Volume has no global dial and
    // is deliberately not offered in Slice Length/Clock mode (see
    // isSequencerCellParameterSwept()'s own doc comment). Defaults here
    // (1.0/Static, i.e. full volume, unchanged) are the "no ramp" no-op,
    // matching what an absent override resolves to anyway -- but
    // processBlock() still gates its use behind `sequencedMode` itself
    // rather than relying on these defaults alone, so a value captured
    // during a previous Sequenced-mode step can never bleed into Slice
    // Length/Clock mode playback after switching modes.
    float currentPickVolumeValue = 1.0f;
    int currentPickVolumeMode = 0;

    // Clock-mode scheduling state (audio thread only). A "window" is one
    // span of the outer clock reference; a "tick" is one subdivision
    // retrigger within that window.
    bool clockModeInitialized = false; // false forces a fresh window on next block
    bool clockCurrentPickValid = false; // false forces a pick even mid-window (very first tick)
    double nextTickPpq = 0.0;
    double windowEndPpq = 0.0;
    int clockCurrentSliceIndex = -1;
    int clockCurrentSubdivisionIndex = -1;
    PlaybackStyle clockCurrentPlaybackStyle = PlaybackStyle::forward; // drawn once per window, alongside the two above

    // Slice Length mode's periodic reset (Step 34, audio thread only) --
    // a lightweight, independent version of the window-boundary tracking
    // just above: resetWindowInitialized false forces a fresh window +
    // fresh pick on next block (transport start, or entering Slice Length
    // mode), same "always start aligned" behaviour clockModeInitialized
    // already gives Clock mode. resetWindowEndPpq is checked every SAMPLE
    // in the Slice Length branch below, not once per block -- the exact
    // bug Step 6 introduced and fixed was computing a boundary once per
    // block from the block's start position, silently missing boundaries
    // that fell mid-block; this reuses Clock mode's own per-sample
    // newWindow check directly rather than re-deriving that logic.
    bool resetWindowInitialized = false;
    double resetWindowEndPpq = 0.0;

    // Sequenced Trigger Mode (Step 37, audio thread only) --
    // sequencedModeInitialized false forces the very first per-sample
    // check on next block to treat the current step as new (transport
    // start, or entering Sequenced mode), same "always start aligned"
    // guarantee clockModeInitialized/resetWindowInitialized already give
    // their own modes. sequencedLastStepIndex is what that per-sample
    // check compares against to detect a genuine step-boundary crossing
    // -- -1 is never a valid step index, so it always counts as "new" the
    // first time.
    bool sequencedModeInitialized = false;
    int sequencedLastStepIndex = -1;

    // Performance mode (audio thread only) -- performanceModeInitialized
    // false forces the very first per-sample check on next block to start
    // SILENT (hasCurrentPick = false), NOT with an immediately-forced pick
    // the way clockModeInitialized/resetWindowInitialized/sequencedModeInitialized
    // all do for their own modes -- Performance mode has nothing to play
    // until a note-on triggers a pick. performanceRecallPending is the
    // same-call handoff from handlePerformanceStateNoteOn() (set true
    // there, consumed and cleared on the very next per-sample check below)
    // that sequencedModeInitialized already models for pattern recall.
    bool performanceModeInitialized = false;
    bool performanceRecallPending = false;

    // Which source governs the pick that performanceRecallPending is about
    // to start (or that's already sounding) -- true: model.performanceWorkingState,
    // the focused slot's own live/in-progress edits (played via the shared
    // trim atomics every other mode also edits through); false:
    // currentlyPlayingPerformanceSnapshot, a frozen copy of some OTHER
    // slot's saved state (including its own saved trim), copied in at the
    // note-on that started this pick so auditioning it can never disturb
    // model.performanceWorkingState/focus. Both set together, only from
    // handlePerformanceStateNoteOn() -- audio-thread-only, same convention
    // as every other currentPick*/performance* field in this section.
    bool performancePlaybackIsFocused = false;
    PerformanceStateSnapshot currentlyPlayingPerformanceSnapshot;

    // Sequenced pattern-bank recall state (Pass 1/2) -- the currently
    // recalled slot (-1 = none yet this session) and the deferred-switch
    // target armed by handlePatternSwitchNoteOn() for Set Interval/End of
    // Pattern, consumed by the per-sample boundary check in processBlock()'s
    // sequencedMode branch. Moved here from the model in the layering
    // refactor; atomics so the UI thread can abandon a pending switch (via
    // SlicerEngine::setPatternSwitchTiming()) while the audio thread may be
    // reading it in that same per-sample check.
    std::atomic<int> activePatternBankSlot { -1 }; // -1 = no recall yet this session
    std::atomic<int> pendingPatternSwitchNote { -1 };

    // Performance mode's own pending-recall target (Pass 1) -- the
    // counterpart to pendingPatternSwitchNote above, armed by
    // handlePerformanceStateNoteOn() for Quantize Recall and consumed by the
    // per-sample boundary check in processBlock()'s performanceMode branch.
    // Same atomic/threading story: the UI thread can abandon it via
    // SlicerEngine::setPerformanceQuantizeRecallEnabled().
    std::atomic<int> pendingPerformanceRecallNote { -1 };

    // Set Interval pattern-switch scheduling (Pass 2, audio thread only) --
    // patternSwitchIntervalBoundaryArmed false means "next occurrence not
    // computed yet," forcing the per-sample check in processBlock() to snap
    // patternSwitchIntervalBoundaryPpq to the next grid point fresh from
    // wherever ppq currently is, the moment a switch gets (re-)armed --
    // same "arm now, resolve against the very next per-sample check" shape
    // clockModeInitialized/resetWindowInitialized use for their own first
    // boundary. Re-armed (set back to false) every time
    // pendingPatternSwitchNote changes, including on replacement by a newer
    // note-on -- always tracks "next occurrence from NOW," not from
    // whenever the original note-on arrived.
    bool patternSwitchIntervalBoundaryArmed = false;
    double patternSwitchIntervalBoundaryPpq = 0.0;

    // Quantize Recall scheduling (Performance mode, audio thread only) --
    // exactly the same "arm now, resolve against the very next per-sample
    // check" shape as patternSwitchIntervalBoundaryArmed/Ppq just above,
    // just for pendingPerformanceRecallNote instead of
    // pendingPatternSwitchNote. Re-armed (set back to false) every time
    // pendingPerformanceRecallNote changes, including on replacement by a
    // newer note-on -- always tracks "next occurrence from NOW."
    bool performanceQuantizeRecallBoundaryArmed = false;
    double performanceQuantizeRecallBoundaryPpq = 0.0;

    // Subdivide (Step 47, audio thread only) -- per-step retrigger rate,
    // Sequenced mode only. Captured once at a step's own pick-start (see
    // the sequencedMode branch in processBlock) from that cell's
    // "Subdivide" override; sequencedSubdivisionActive false (the
    // default -- Off/no override) means nothing here runs and playback
    // is identical to before this feature existed.
    // sequencedSubdivisionRow is which slice to restart on each
    // retrigger -- cached rather than re-read from currentStepIndex,
    // since currentStepIndex keeps advancing for as long as this note
    // sustains across later (inactive) step columns, while this note's
    // own row doesn't change.
    // sequencedNextSubdivisionOffsetHostSamples/
    // sequencedSubdivisionTickLengthHostSamples are host-sample-domain
    // scheduling state measured against samplesSinceWindowStart/
    // currentWindowLengthHostSamples just below -- reused directly
    // rather than a separate ppq-based scheduler, since those already
    // track "how far into this step's own window are we" (and, for
    // Filter Down/Up, already drive the Whole Window sweep -- see
    // useWholeWindow in processBlock, generalized to also cover a
    // subdivided Sequenced step's window).
    bool sequencedSubdivisionActive = false;
    int sequencedSubdivisionRow = -1;
    double sequencedNextSubdivisionOffsetHostSamples = 0.0;
    double sequencedSubdivisionTickLengthHostSamples = 0.0;

    // Filter Sweep's Whole Window scope (Step 30) — how far into the
    // CURRENT WINDOW we are, in host samples, as opposed to samplesSince-
    // PickStart's per-pick tracking just below. Reset only on a genuine
    // new-window event (never on an ordinary per-tick retrigger, unlike
    // samplesSincePickStart), so it stays continuous across every tick
    // inside one window. currentWindowLengthHostSamples is set alongside
    // it, at the same new-window event, from whatever the clock reference
    // note value resolves to in host samples at that moment — both were
    // Clock-mode-only through Step 46 and are meaningless in Slice Length
    // mode (which has no concept of a "window").
    // Step 47 (Subdivide) reuses this exact same pair for Sequenced mode
    // too: "window" there means one currently-playing step's own
    // (already monophony-clamped) total duration, reset at that step's
    // pick-start instead of a recurring Clock window -- this is what
    // lets a subdivided Filter Down/Up step's sweep glide continuously
    // across the whole step while individual retriggers happen
    // underneath, and also doubles as the Subdivide retrigger scheduler
    // itself (see sequencedNextSubdivisionOffsetHostSamples above).
    double samplesSinceWindowStart = 0.0;
    double currentWindowLengthHostSamples = 0.0;

    // Self-chaining playback state — which slice is currently sounding,
    // where we are within it (source sample units), and where it ends.
    // When position reaches the end, the very next sample immediately
    // picks a new slice and continues with zero gap.
    //
    // currentPosition/currentEndSample are the "unfolded" scheduling
    // position — for Ping-Pong, currentEndSample is pushed out to a full
    // round trip (2x slice length) and currentPosition just keeps
    // counting up through it, same as it always has for Forward.
    // currentSliceStartSample/currentSliceLength are the TRUE slice
    // bounds regardless of style, kept separately since currentEndSample
    // no longer is one for Ping-Pong — these feed GranularStretcher::
    // foldPosition() to compute the actual (bounced, for Ping-Pong) read
    // position each render step.
    bool hasCurrentPick = false;
    int currentSliceIndex = -1;
    double currentPosition = 0.0;
    int currentEndSample = 0;
    int currentSliceStartSample = 0;
    int currentSliceLength = 0;
    PlaybackStyle currentPlaybackStyle = PlaybackStyle::forward;

    // Where (in host-output samples since this pick started) a Ping-Pong
    // round trip reverses direction — always one slice's worth of natural
    // (un-doubled) playback time, regardless of how currentPickLength-
    // InHostSamples itself might get shortened by a Clock-mode tick.
    // Meaningless/unused for Forward.
    double currentPickMidpointHostSamples = 0.0;

    // Fixed real-time length (host samples) of a Tape Stop pick's decel
    // ramp — the pick's natural slice length in Slice Length mode; the
    // window or tick length in Clock mode, per Tape Stop scope. Rate and
    // gain both ramp from 1.0 to 0.0 across this, via samplesSincePick-
    // Start / this. Deliberately NOT capped by the slice's own natural
    // length in Clock mode (unlike Forward/Ping-Pong's currentPickLength-
    // InHostSamples) — the whole point is that read position may not
    // reach the slice's actual end before the rate hits zero. Meaningless
    // /unused for Forward/Ping-Pong.
    double currentPickTapeStopDurationHostSamples = 0.0;

    // Scratch (v1): this pick's own bounce-cycle length in host samples,
    // captured once at pick-start by every trigger mode via
    // computeScratchCycleLengthHostSamples() -- see that function's own
    // doc comment. Meaningless/unused for every other style.
    double currentPickScratchCycleLengthHostSamples = 0.0;

    // Scratch (v2): this pick's own Forward/Backward Curve choice --
    // captured once at pick-start, same per-step-override-else-fixed-
    // default pattern as every other Scratch/Bitcrush parameter (Slice
    // Length/Clock modes always Linear/Linear; Sequenced mode reads this
    // step's own override if it has one). Fed into both render paths'
    // foldPosition()/renderAndAdvance() calls in the shared per-sample
    // section below. Meaningless/unused for every other style.
    EasingCurve currentPickScratchForwardCurve = EasingCurve::linear;
    EasingCurve currentPickScratchBackwardCurve = EasingCurve::linear;

    // Fade tracking, in host-output-sample units (not source-sample units,
    // so fade length in ms stays constant regardless of repitching).
    // Reset every time a new pick starts.
    double samplesSincePickStart = 0.0;
    double currentPickLengthInHostSamples = 0.0;

    // Pitch mode (Step 17) — Time-Stretch state. granularStretcher is
    // reseeded from currentPosition every time a new pick starts
    // (regardless of which mode is active, so switching mid-pick always
    // finds it already in sync) and again, mid-pick, whenever the mode
    // itself changes (granularNeedsReseed).
    std::atomic<bool> granularNeedsReseed { false };
    GranularStretcher granularStretcher;

    // Per-pick beat-quantization state (Step 24, audio thread only) —
    // computed once at pick-start in Slice Length mode (never in Clock
    // mode, and never for Tape Stop/Stretch picks — currentPickBeatQuantized
    // stays false for those, and currentPickQuantizedStretchRatio is simply
    // not consulted). Substitutes for repitchRatio, symmetrically, in both
    // this pick's granular hop schedule and its scheduling-position advance
    // rate — see processBlock().
    bool currentPickBeatQuantized = false;
    double currentPickQuantizedStretchRatio = 1.0;

    // Performance mode's Sync toggle (Pass 1, audio thread only) -- captured
    // once at pick-start, same shape as currentPickBeatQuantized just above:
    // explicitly false for every OTHER mode's picks (Clock/Sequenced/Slice
    // Length), and set to `! model.performanceWorkingState.sync` for a Performance
    // pick. When true, processBlock() substitutes a native (sampleSampleRate/
    // hostSampleRate) rate for playbackRate at every downstream use, instead
    // of whichever global Pitch Mode (Repitch/Time-Stretch) is currently
    // selected -- captured once here, rather than checked inline at each of
    // those several downstream sites, so a pick's rate can never read as one
    // thing at pick-start and another mid-render.
    bool currentPickNativeRateActive = false;

};
