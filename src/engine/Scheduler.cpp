#include "Scheduler.h"
#include "Tempo.h"

#include <algorithm>
#include <cmath>

namespace nedit::engine {

namespace {

// The plugin's bus layout is stereo; this cap only guards a hostile
// channel count from overflowing the cursor stack array below.
inline constexpr int kMaxOutputChannels = 8;

[[nodiscard]] double noteValueBeats (int index) noexcept
{
    return state::isValidNoteValueIndex (index)
        ? state::kNoteValues[static_cast<std::size_t> (index)].beats
        : 0.0;
}

[[nodiscard]] state::PlaybackStyle styleFromOrdinal (int ordinal) noexcept
{
    return state::isValidPlaybackStyleIndex (ordinal)
        ? static_cast<state::PlaybackStyle> (ordinal)
        : state::PlaybackStyle::forward;
}

// Slice Length reset-window length in beats for a (possibly unsanitized)
// bars index. The scheduler consumes state snapshots as given, and every
// OTHER state-derived index in this file is range-guarded before use --
// this one is too: out-of-range indices clamp into the table instead of a
// wild read of a 4-entry constexpr array (and the result is a divisor, so
// a garbage 0 would cascade into floor(x / 0.0) -> UB int64 cast).
[[nodiscard]] double resetWindowBeats (int barsIndex) noexcept
{
    const auto last = static_cast<int> (state::kResetBarsValues.size()) - 1;
    const int clamped = std::clamp (barsIndex, 0, last);
    return static_cast<double> (
               state::kResetBarsValues[static_cast<std::size_t> (clamped)])
         * 4.0;   // 4/4
}

// Clock-mode outer window length in beats. Invalid palette indices fall
// back to the default reference (quarter note) INSTEAD of noteValueBeats'
// 0.0 sentinel: windowBeats is a divisor at both call sites, and
// floor(x / 0.0) casts inf/NaN to int64 -- UB. The sibling interval
// divisions (pattern switch / performance recall) guard with
// max(.., 1e-6) for exactly the same reason.
[[nodiscard]] double clockWindowBeats (int index) noexcept
{
    const double beats = noteValueBeats (index);
    return beats > 0.0
        ? beats
        : state::kNoteValues[static_cast<std::size_t> (state::kNoteValue4n)].beats;
}

} // namespace

void VoiceScheduler::prepare (double hostSampleRate)
{
    renderer_.prepare (hostSampleRate);
}

void VoiceScheduler::setSeed (std::uint32_t seed) noexcept
{
    rng_.seed (seed);
}

double VoiceScheduler::nextUniform() noexcept
{
    return static_cast<double> (rng_()) / 4294967296.0;  // [0, 1)
}

int VoiceScheduler::pickWeightedIndex (const float* weights, std::size_t count)
{
    if (weights == nullptr || count == 0)
        return -1;

    float totalWeight = 0.0f;

    for (std::size_t i = 0; i < count; ++i)
        totalWeight += std::max (0.0f, weights[i]);

    if (totalWeight <= 0.0f)
    {
        // Uniform fallback rather than picking nothing and stalling.
        const auto index = static_cast<std::size_t> (
            nextUniform() * static_cast<double> (count));
        return static_cast<int> (std::min (index, count - 1));
    }

    const float target = static_cast<float> (nextUniform()) * totalWeight;
    float cumulative = 0.0f;

    for (std::size_t i = 0; i < count; ++i)
    {
        cumulative += std::max (0.0f, weights[i]);

        if (target <= cumulative)
            return static_cast<int> (i);
    }

    return static_cast<int> (count) - 1;  // float rounding fallback
}

int VoiceScheduler::pickWeightedSlice (const Run& r)
{
    const auto& weights = (r.sliceWeights != nullptr) ? *r.sliceWeights
                                                      : r.generate.sliceWeights;
    const auto count = std::min<std::size_t> (r.slices.size(), weights.size());

    if (count == 0)
        return -1;

    return pickWeightedIndex (weights.data(), count);
}

state::PlaybackStyle VoiceScheduler::pickWeightedStyle (const Run& r)
{
    return styleFromOrdinal (
        pickWeightedIndex (r.generate.styleWeights.data(), state::kNumPlaybackStyles));
}

VoiceScheduler::PreparedPick VoiceScheduler::preparePick (
    state::PlaybackStyle style, const Slice& slice,
    const state::StyleParameters& params,
    double hostSampleRate, double playbackRate, double hostBpm) const
{
    PreparedPick p;

    p.scratchCycleHostSamples = tempo::scratchCycleLengthHostSamples (
        params.scratchRate, slice.lengthFrames(), hostBpm, hostSampleRate, playbackRate);

    p.naturalLengthHostSamples = playbackRate > 0.0
        ? static_cast<double> (slice.lengthFrames()) / playbackRate
        : 0.0;

    switch (style)
    {
        case state::PlaybackStyle::pingPong:
            // The full there-and-back round trip.
            p.schedulingEndFrame = 2 * slice.endFrame - slice.startFrame;
            break;

        case state::PlaybackStyle::stretch:
            p.schedulingEndFrame = slice.startFrame + static_cast<std::int64_t> (
                static_cast<double> (params.grainSpeed)
                    * static_cast<double> (slice.lengthFrames()));
            break;

        case state::PlaybackStyle::scratch:
            p.schedulingEndFrame = slice.startFrame + static_cast<std::int64_t> (
                p.scratchCycleHostSamples * playbackRate);
            break;

        default:
            p.schedulingEndFrame = slice.endFrame;
            break;
    }

    // Where a Ping-Pong round trip reverses -- one natural pass. Scratch's
    // bounce has nothing to do with the slice's own length: half of ITS
    // cycle instead.
    p.midpointHostSamples = (style == state::PlaybackStyle::scratch)
        ? p.scratchCycleHostSamples * 0.5
        : p.naturalLengthHostSamples;

    return p;
}

void VoiceScheduler::commitPick (const Slice& slice, const state::StyleParameters& params,
                                 const PreparedPick& prepared,
                                 state::PlaybackStyle style,
                                 double pickLengthHostSamples,
                                 double tapeStopDurationHostSamples,
                                 bool filterWholeWindow, bool flangerWholeWindow,
                                 bool beatQuantized, double quantizedStretchRatio,
                                 PickExtras extras)
{
    PickParams pick;
    pick.style = style;
    pick.sliceStartFrame = slice.startFrame;
    pick.sliceLengthFrames = slice.lengthFrames();
    pick.schedulingEndFrame = prepared.schedulingEndFrame;
    pick.pickLengthHostSamples = pickLengthHostSamples;
    pick.tapeStopDurationHostSamples = tapeStopDurationHostSamples;
    pick.midpointHostSamples = prepared.midpointHostSamples;
    pick.scratchCycleLengthHostSamples = prepared.scratchCycleHostSamples;
    pick.beatQuantized = beatQuantized;
    pick.quantizedStretchRatio = quantizedStretchRatio;
    pick.nativeRate = extras.nativeRate;
    pick.halfSliceFold = extras.halfSliceFold;
    pick.useDurationGate = extras.useDurationGate;
    pick.filterWholeWindow = filterWholeWindow;
    pick.flangerWholeWindow = flangerWholeWindow;
    pick.volumeWholeWindow = extras.volumeWholeWindow;
    pick.volumeRampActive = extras.volumeRampActive;
    pick.velocityGain = extras.velocityGain;
    pick.params = params;

    renderer_.startPick (pick);
    ++picksStarted_;
}

void VoiceScheduler::renderSampleInto (Run& r, int i)
{
    float* cursors[kMaxOutputChannels];
    const int channels = std::min (r.numOutChannels, kMaxOutputChannels);

    for (int ch = 0; ch < channels; ++ch)
        cursors[ch] = r.outAdd[ch] + i;

    renderer_.renderSample (r.ctx, cursors, channels);
}

void VoiceScheduler::stopAndDisarm() noexcept
{
    armedMode_ = ArmedMode::none;
    renderer_.clearPick();
}

void VoiceScheduler::process (const state::PluginState& state,
                              const std::vector<Slice>& slices,
                              BlockContext& ctx,
                              const TransportFrame& transport,
                              float* const* outAdd,
                              int numOutChannels,
                              int numSamples,
                              const std::vector<float>* sliceWeightsOverride)
{
    if (numSamples <= 0 || outAdd == nullptr || numOutChannels <= 0)
        return;

    // Drain the cross-thread pattern-release request FIRST: no
    // SequencerView raw pointers into recalledPattern_ exist between
    // blocks, so this is the one safe place to destroy it (see
    // requestWorkingPatternRelease).
    if (workingPatternReleaseRequested_.exchange (false, std::memory_order_acq_rel))
        recalledPattern_.reset();

    const double hostSampleRate = ctx.hostSampleRate;

    if (hostSampleRate <= 0.0 || transport.bpm <= 0.0)
        return;

    // --- derived per-block timing (one place, so callers cannot desync) ----
    const auto& sample = state.sample;
    const double srConversion = sample.sampleSampleRate > 0.0
        ? sample.sampleSampleRate / hostSampleRate
        : 0.0;
    const double repitch = tempo::repitchRatio (sample, transport.bpm);

    ctx.srConversionRatio = srConversion;
    ctx.playbackRate = srConversion * repitch;
    ctx.pitchRatio = std::pow (2.0, static_cast<double> (state.render.pitchShiftSemitones) / 12.0);
    ctx.timeStretchMode = state.render.pitchMode == state::PitchMode::timeStretch;
    ctx.grainSizeHostSamples = static_cast<double> (state.render.grainSizeMs) / 1000.0
                             * hostSampleRate;
    ctx.grainSpeed = static_cast<double> (state.render.grainSpeed);
    ctx.outputHopSamples = ctx.grainSizeHostSamples * 0.5;  // fixed 50% overlap
    ctx.sourceHopSamples = ctx.outputHopSamples * srConversion * repitch;
    ctx.grainWindowShape = state.render.grainWindowShape == state::GrainWindowShape::hann
        ? GranularStretcher::WindowShape::hann
        : GranularStretcher::WindowShape::triangular;
    ctx.fadeInSamplesRequested = static_cast<double> (state.render.fadeInMs) / 1000.0
                               * hostSampleRate;
    ctx.fadeOutSamplesRequested = static_cast<double> (state.render.fadeOutMs) / 1000.0
                                * hostSampleRate;

    Run r { state,
            state.generate,
            slices,
            sliceWeightsOverride,
            ctx,
            transport,
            (transport.bpm / 60.0) / hostSampleRate,
            outAdd,
            numOutChannels,
            numSamples };

    // Slice Length/Clock/Sequenced are meaningless without host transport
    // (their triggers ARE its beat/bar position). Performance and Control
    // are MIDI-driven: they must keep working while the transport is
    // stopped (auditioning notes, quantize-recall falls straight through).
    const bool needsTransport = state.triggerMode != state::TriggerMode::performance
                             && state.triggerMode != state::TriggerMode::control;

    if ((needsTransport && ! transport.playing) || slices.empty())
    {
        stopAndDisarm();
        return;
    }

    bailUntilBlockEnd_ = false;

    switch (state.triggerMode)
    {
        case state::TriggerMode::sliceLength:
            runSliceLength (r);
            break;

        case state::TriggerMode::clock:
            runClock (r);
            break;

        case state::TriggerMode::sequenced:
            runSequenced (r);
            break;

        case state::TriggerMode::performance:
            runPerformance (r);
            break;

        case state::TriggerMode::control:
            runControl (r);
            break;

        default:
            stopAndDisarm();
            return;
    }
}

void VoiceScheduler::runSliceLength (Run& r)
{
    const auto& generate = r.generate;

    if (armedMode_ != ArmedMode::sliceLength)
    {
        // Just entered the mode or transport just started: snap to the
        // reset window we're currently inside and force a fresh pick
        // aligned to it right away.
        const double windowBeats = resetWindowBeats (generate.resetBarsIndex);
        const auto windowIndex = static_cast<std::int64_t> (
            std::floor (r.transport.ppqStart / windowBeats));
        resetWindowEndPpq_ = static_cast<double> (windowIndex + 1) * windowBeats;
        armedMode_ = ArmedMode::sliceLength;
        renderer_.clearPick();
    }

    for (int i = 0; i < r.numSamples && ! bailUntilBlockEnd_; ++i)
    {
        const double samplePpq = r.transport.ppqStart + static_cast<double> (i) * r.ppqPerSample;

        // Periodic reset, checked every SAMPLE (the Step 6 bug discipline):
        // crossing the boundary cuts whatever is playing right here --
        // the chaining loop below then starts fresh on this very sample.
        // Live-reads the bar setting so a mid-stream change takes effect
        // at the next boundary.
        if (samplePpq >= resetWindowEndPpq_)
        {
            const double windowBeats = resetWindowBeats (generate.resetBarsIndex);
            const auto windowIndex = static_cast<std::int64_t> (
                std::floor (samplePpq / windowBeats));
            resetWindowEndPpq_ = static_cast<double> (windowIndex + 1) * windowBeats;
            renderer_.clearPick();
        }

        // Every fresh pick is capped by the time remaining until the next
        // reset boundary so its fade-out anticipates the cut instead of
        // clicking.
        const double samplesUntilReset = std::max (
            0.0, (resetWindowEndPpq_ - samplePpq) / r.ppqPerSample);

        int pickAttempts = 0;

        while (! renderer_.hasPick() || renderer_.finished (r.ctx))
        {
            const int sliceIndex = pickWeightedSlice (r);

            if (sliceIndex < 0)
            {
                renderer_.clearPick();
                break;
            }

            const Slice& slice = r.slices[static_cast<std::size_t> (sliceIndex)];
            const auto style = pickWeightedStyle (r);
            auto prepared = preparePick (style, slice, generate.styleParams,
                                         r.ctx.hostSampleRate, r.ctx.playbackRate,
                                         r.transport.bpm);

            double pickLength;

            if (style == state::PlaybackStyle::pingPong)
                pickLength = 2.0 * prepared.naturalLengthHostSamples;
            else if (style == state::PlaybackStyle::stretch)
                pickLength = prepared.naturalLengthHostSamples
                           * static_cast<double> (generate.styleParams.grainSpeed);
            else if (style == state::PlaybackStyle::scratch)
                pickLength = prepared.scratchCycleHostSamples;
            else
                pickLength = prepared.naturalLengthHostSamples;

            // Tape Stop runs on its own time-based duration (its read may
            // never reach the slice's end); everyone else ignores this.
            double tapeStopDuration = prepared.naturalLengthHostSamples;

            // Beat-quantize: Slice Length only, Forward/Ping-Pong only.
            // Both pitch modes' toggles share one target calculation; only
            // where the resulting ratio gets applied differs (inside the
            // renderer, per pitch path).
            bool beatQuantized = false;
            double quantizedRatio = 1.0;
            const bool beatQuantizeWanted = r.ctx.timeStretchMode
                ? r.state.render.beatQuantizeTimeStretch
                : r.state.render.beatQuantizeRepitch;

            if (beatQuantizeWanted
                && (style == state::PlaybackStyle::forward
                    || style == state::PlaybackStyle::pingPong)
                && slice.lengthFrames() > 0)
            {
                const auto target = tempo::computeBeatQuantizeTarget (
                    slice.lengthFrames(), style == state::PlaybackStyle::pingPong,
                    r.state.sample.sampleSampleRate,
                    tempo::calculatedOriginalBpm (r.state.sample),
                    r.transport.bpm);

                if (target.quantized)
                {
                    beatQuantized = true;
                    quantizedRatio = target.stretchRatio;
                    pickLength = target.targetHostSeconds * r.ctx.hostSampleRate;

                    if (style == state::PlaybackStyle::pingPong)
                        prepared.midpointHostSamples = pickLength * 0.5;
                }
            }

            // Reset cap applied LAST -- it wins over any other chosen
            // duration, same "whichever comes first" precedence everywhere
            // else in this engine.
            pickLength = std::min (pickLength, samplesUntilReset);
            tapeStopDuration = std::min (tapeStopDuration, samplesUntilReset);

            commitPick (slice, generate.styleParams, prepared, style,
                        pickLength, tapeStopDuration,
                        /*filterWholeWindow*/ false, /*flangerWholeWindow*/ false,
                        beatQuantized, quantizedRatio, PickExtras {});

            if (++pickAttempts > 1000)
            {
                bailUntilBlockEnd_ = true;  // safety bail: silence for the rest of the block
                break;
            }
        }

        renderSampleInto (r, i);
    }
}

void VoiceScheduler::runClock (Run& r)
{
    const auto& generate = r.generate;

    if (armedMode_ != ArmedMode::clock)
    {
        // Just entered the mode or transport just started: snap to the
        // window we're currently inside and force an immediate pick on
        // the very first sample below.
        const double windowBeats = clockWindowBeats (generate.clockReferenceIndex);
        const auto windowIndex = static_cast<std::int64_t> (
            std::floor (r.transport.ppqStart / windowBeats));
        clockWindowEndPpq_ = static_cast<double> (windowIndex + 1) * windowBeats;
        clockNextTickPpq_ = r.transport.ppqStart;
        clockPickValid_ = false;
        armedMode_ = ArmedMode::clock;
    }

    for (int i = 0; i < r.numSamples && ! bailUntilBlockEnd_; ++i)
    {
        const double samplePpq = r.transport.ppqStart + static_cast<double> (i) * r.ppqPerSample;

        if (samplePpq >= clockNextTickPpq_)
        {
            const bool newWindow = ! clockPickValid_ || samplePpq >= clockWindowEndPpq_;

            if (newWindow)
            {
                // One weighted draw of slice + subdivision + style PER
                // WINDOW; every tick inside retriggers the same combo.
                clockSliceIndex_ = pickWeightedSlice (r);
                clockSubdivisionIndex_ = pickWeightedIndex (
                    generate.subdivisionWeights.data(), state::kNumNoteValues);
                clockStyleOrdinal_ = pickWeightedIndex (
                    generate.styleWeights.data(), state::kNumPlaybackStyles);
                clockPickValid_ = true;

                const double windowBeats = clockWindowBeats (generate.clockReferenceIndex);
                const auto windowIndex = static_cast<std::int64_t> (
                    std::floor (samplePpq / windowBeats));
                clockWindowEndPpq_ = static_cast<double> (windowIndex + 1) * windowBeats;
                clockWindowLengthHostSamples_ = windowBeats * (60.0 / r.transport.bpm)
                                              * r.ctx.hostSampleRate;

                // Whole-window sweeps ride this clock across every tick
                // in the window.
                renderer_.startWindow (clockWindowLengthHostSamples_);
            }

            bool windowIsOneContinuousRender = false;

            if (clockSliceIndex_ >= 0
                && clockSliceIndex_ < static_cast<int> (r.slices.size()))
            {
                const Slice& slice = r.slices[static_cast<std::size_t> (clockSliceIndex_)];
                const auto style = styleFromOrdinal (clockStyleOrdinal_);
                auto prepared = preparePick (style, slice, generate.styleParams,
                                             r.ctx.hostSampleRate, r.ctx.playbackRate,
                                             r.transport.bpm);

                const double tickBeats = noteValueBeats (clockSubdivisionIndex_);
                const double tickLengthHostSamples = tickBeats * (60.0 / r.transport.bpm)
                                                   * r.ctx.hostSampleRate;

                double pickLength;
                double tapeStopDuration = prepared.naturalLengthHostSamples;

                if (style == state::PlaybackStyle::tapeStop)
                {
                    // Entirely scope-driven -- deliberately NOT capped by
                    // the slice's own natural length: the point is that
                    // the read may never reach the slice's actual end
                    // before the rate hits zero.
                    tapeStopDuration = generate.tapeStopScope == state::WindowScope::wholeWindow
                        ? clockWindowLengthHostSamples_
                        : tickLengthHostSamples;
                    pickLength = tapeStopDuration;
                }
                else if (style == state::PlaybackStyle::stretch)
                {
                    // Always overrides subdivision (no per-tick option),
                    // capped by whichever comes first: the full Grain
                    // Speed-x natural length, or the window boundary.
                    pickLength = std::min (
                        prepared.naturalLengthHostSamples
                            * static_cast<double> (generate.styleParams.grainSpeed),
                        clockWindowLengthHostSamples_);
                }
                else
                {
                    // The fade-out anticipates whichever comes first --
                    // the natural (round-trip) end or the forced retrigger
                    // at the next tick -- otherwise every retrigger clicks.
                    const double roundTripLengthHostSamples =
                        style == state::PlaybackStyle::pingPong
                            ? 2.0 * prepared.naturalLengthHostSamples
                        : style == state::PlaybackStyle::scratch
                            ? prepared.scratchCycleHostSamples
                            : prepared.naturalLengthHostSamples;
                    pickLength = std::min (roundTripLengthHostSamples, tickLengthHostSamples);
                }

                commitPick (slice, generate.styleParams, prepared, style,
                            pickLength, tapeStopDuration,
                            generate.filterSweepScope == state::WindowScope::wholeWindow,
                            /*flangerWholeWindow*/ true,
                            /*beatQuantized*/ false, /*quantizedStretchRatio*/ 1.0,
                            PickExtras {});

                windowIsOneContinuousRender =
                    (style == state::PlaybackStyle::tapeStop
                     && generate.tapeStopScope == state::WindowScope::wholeWindow)
                    || style == state::PlaybackStyle::stretch;
            }
            else
            {
                renderer_.clearPick();
            }

            if (windowIsOneContinuousRender)
            {
                // Whole-window Tape Stop and Stretch span the entire
                // window: jumping straight to its end makes the next event
                // the boundary itself, naturally a fresh new-window pick.
                clockNextTickPpq_ = clockWindowEndPpq_;
            }
            else
            {
                const double subdivisionBeats = noteValueBeats (clockSubdivisionIndex_);
                clockNextTickPpq_ += std::max (subdivisionBeats, 1.0e-6);
                clockNextTickPpq_ = std::min (clockNextTickPpq_, clockWindowEndPpq_);
            }
        }

        // The window clock advances every sample (wall-clock tracking for
        // whole-window sweep progress), then the voice renders.
        renderer_.tickWindowClock();
        renderSampleInto (r, i);
    }
}

void VoiceScheduler::requestPatternSwitch (int midiNote) noexcept
{
    if (midiNote >= 0 && midiNote < state::kNumMidiNotes)
        pendingPatternNote_ = midiNote;
}

VoiceScheduler::SequencerView VoiceScheduler::effectiveSequencer (
    const state::PluginState& state) const noexcept
{
    SequencerView view;

    if (recalledPattern_)
    {
        const auto& pattern = *recalledPattern_;
        view.stepResolutionIndex = pattern.stepResolutionIndex;
        view.rows = pattern.rows;
        view.columns = pattern.columns;
        view.grid = &pattern.grid;
        view.overrides = &pattern.overrides;
        view.extensions = &pattern.extensions;
    }
    else
    {
        const auto& seq = state.sequencer;
        view.stepResolutionIndex = seq.stepResolutionIndex;
        view.rows = seq.rows;
        view.columns = seq.columns;
        view.grid = &seq.grid;
        view.overrides = &seq.overrides;
        view.extensions = &seq.extensions;
    }

    return view;
}

void VoiceScheduler::applyPatternRecallFromState (const state::PluginState& state, int note)
{
    pendingPatternNote_ = -1;
    patternIntervalArmed_ = false;

    if (note < 0 || note >= state::kNumMidiNotes)
        return;

    const auto& slot = state.sequencer.patternBank[static_cast<std::size_t> (note)];

    if (! slot.populated)
        return;  // empty slot -- current pattern keeps playing undisturbed

    recalledPattern_ = slot;
    // Force the step tracker to re-sync: the currently-inside step
    // re-triggers against the new grid on this very sample.
    sequencedLastStepIndex_ = -1;
}

void VoiceScheduler::startSequencedPick (Run& r, const SequencerView& view,
                                         int row, int step, std::size_t sliceIndex)
{
    const auto cell = static_cast<std::uint32_t> (row) * static_cast<std::uint32_t> (view.columns)
                    + static_cast<std::uint32_t> (step);

    // fallbackParams + this cell's overrides. The fallback always comes
    // from the WORKING sequencer state -- a recalled bank snapshot swaps
    // grid/dimensions only (the original's recall never touched global
    // parameter values either).
    state::StyleParameters merged = r.state.sequencer.fallbackParams;

    if (const auto it = view.overrides->find (cell); it != view.overrides->end())
        for (const auto& [id, value] : it->second)
            merged.set (id, value);

    const Slice& slice = r.slices[sliceIndex];
    const auto style = styleFromOrdinal ((*view.grid)[cell]);

    auto prepared = preparePick (style, slice, merged, r.ctx.hostSampleRate,
                                 r.ctx.playbackRate, r.transport.bpm);

    const double stepBeats = noteValueBeats (view.stepResolutionIndex);
    const double stepHostSamples = stepBeats * (60.0 / r.transport.bpm)
                                 * r.ctx.hostSampleRate;

    // Declared length in steps: this row's natural length (slice duration
    // quantized to the step resolution), raised by the cell's Shift+drag
    // extension when one exists.
    int naturalSteps = 1;

    {
        const double originalBpm = tempo::calculatedOriginalBpm (r.state.sample);

        if (slice.lengthFrames() > 0 && r.state.sample.sampleSampleRate > 0.0
            && originalBpm > 0.0 && stepBeats > 0.0)
        {
            const double sliceSeconds =
                static_cast<double> (slice.lengthFrames()) / r.state.sample.sampleSampleRate;
            const double naturalBeats = sliceSeconds * (originalBpm / 60.0);
            naturalSteps = std::max (1, static_cast<int> (
                std::lround (naturalBeats / stepBeats)));
        }
    }

    int extensionSteps = 0;

    if (const auto it = view.extensions->find (cell); it != view.extensions->end())
        extensionSteps = static_cast<int> (it->second);

    const double declaredLengthHostSamples =
        static_cast<double> (std::max (naturalSteps, extensionSteps)) * stepHostSamples;

    // Anticipatory fade: whichever active column comes next anywhere in
    // the grid WILL cut this note off, so cap its length to that moment.
    const int rowsEffective = std::min (view.rows, static_cast<int> (r.slices.size()));
    int stepsUntilNextActive = view.columns;

    for (int offset = 1; offset <= view.columns; ++offset)
    {
        const int checkColumn = (step + offset) % view.columns;
        bool columnHasActive = false;

        for (int row2 = 0; row2 < rowsEffective; ++row2)
        {
            const auto checkCell = static_cast<std::size_t> (row2) * static_cast<std::size_t> (view.columns)
                                 + static_cast<std::size_t> (checkColumn);

            if ((*view.grid)[checkCell] >= 0)
            {
                columnHasActive = true;
                break;
            }
        }

        if (columnHasActive)
        {
            stepsUntilNextActive = offset;
            break;
        }
    }

    const double samplesUntilNextActiveStep =
        static_cast<double> (stepsUntilNextActive) * stepHostSamples;

    double pickLength;
    double tapeStopDuration = prepared.naturalLengthHostSamples;  // unused unless tape stop

    if (style == state::PlaybackStyle::tapeStop)
    {
        tapeStopDuration = std::min (declaredLengthHostSamples, samplesUntilNextActiveStep);
        pickLength = tapeStopDuration;
    }
    else
    {
        pickLength = std::min (declaredLengthHostSamples, samplesUntilNextActiveStep);
    }

    // The window clock spans THIS STEP'S whole declared duration (before
    // Subdivide slices it into ticks) -- whole-window sweeps and the
    // Subdivide retrigger grid share this one clock.
    renderer_.startWindow (style == state::PlaybackStyle::tapeStop ? tapeStopDuration : pickLength);

    subdivisionActive_ = false;
    subdivisionRow_ = -1;

    const int subdivideOption = static_cast<int> (std::lround (
        merged.get (state::StyleParamId::subdivide)));

    if (subdivideOption > 0)
    {
        const double subBeats = noteValueBeats (subdivideOption - 1);
        subdivisionTickLengthSamples_ = std::max (
            1.0, subBeats * (60.0 / r.transport.bpm) * r.ctx.hostSampleRate);
        nextSubdivisionOffsetSamples_ = subdivisionTickLengthSamples_;

        // This very trigger IS the first subdivision slot: cap its length
        // so its fade-out completes before the next retrigger.
        pickLength = std::min (pickLength, subdivisionTickLengthSamples_);

        if (style == state::PlaybackStyle::tapeStop)
            tapeStopDuration = std::min (tapeStopDuration, subdivisionTickLengthSamples_);

        subdivisionActive_ = true;
        subdivisionRow_ = static_cast<int> (sliceIndex);
    }

    PickExtras extras;
    extras.halfSliceFold = style == state::PlaybackStyle::pingPong;
    extras.useDurationGate = true;
    extras.volumeRampActive = true;
    extras.volumeWholeWindow = subdivisionActive_;

    commitPick (slice, merged, prepared, style,
                pickLength, tapeStopDuration,
                /*filterWholeWindow*/ subdivisionActive_,
                /*flangerWholeWindow*/ subdivisionActive_,
                /*beatQuantized*/ false, /*quantizedStretchRatio*/ 1.0, extras);
}

void VoiceScheduler::runSequenced (Run& r)
{
    if (armedMode_ != ArmedMode::sequenced)
    {
        // Just entered the mode or transport just started: force the very
        // first per-sample check below to treat whichever step we're
        // currently inside as new, so it triggers immediately rather than
        // waiting for the next boundary. No stale Subdivide schedule is
        // carried in.
        sequencedLastStepIndex_ = -1;
        subdivisionActive_ = false;
        subdivisionRow_ = -1;
        armedMode_ = ArmedMode::sequenced;
        renderer_.clearPick();
    }

    SequencerView view = effectiveSequencer (r.state);

    for (int i = 0; i < r.numSamples && ! bailUntilBlockEnd_; ++i)
    {
        const double samplePpq = r.transport.ppqStart + static_cast<double> (i) * r.ppqPerSample;

        double stepBeats = noteValueBeats (view.stepResolutionIndex);
        int totalSteps = view.columns;

        if (stepBeats > 0.0 && totalSteps > 0)
        {
            std::int64_t absoluteStepIndex = static_cast<std::int64_t> (
                std::floor (samplePpq / stepBeats));
            int currentStepIndex = static_cast<int> (
                ((absoluteStepIndex % totalSteps) + totalSteps) % totalSteps);

            // --- deferred pattern switch, checked every SAMPLE ----------
            if (pendingPatternNote_ >= 0)
            {
                bool boundaryCrossed = false;

                const auto timing = r.state.sequencer.patternSwitchTiming;

                if (timing == state::PatternSwitchTiming::immediate)
                {
                    // The original applies immediate recalls synchronously
                    // in its MIDI handler; here they land on the very next
                    // per-sample pass -- at most one sample later.
                    boundaryCrossed = true;
                }
                else if (timing == state::PatternSwitchTiming::setInterval)
                {
                    if (! patternIntervalArmed_)
                    {
                        // Snap to the next interval grid point from wherever
                        // ppq is right now (the Clock/Reset "windowIndex + 1"
                        // shape).
                        const double intervalBeats = std::max (
                            noteValueBeats (r.state.sequencer.patternSwitchIntervalIndex),
                            1.0e-6);
                        const auto intervalIndex = static_cast<std::int64_t> (
                            std::floor (samplePpq / intervalBeats));
                        patternIntervalBoundaryPpq_ =
                            static_cast<double> (intervalIndex + 1) * intervalBeats;
                        patternIntervalArmed_ = true;
                    }

                    boundaryCrossed = samplePpq >= patternIntervalBoundaryPpq_;
                }
                else if (timing == state::PatternSwitchTiming::endOfPattern)
                {
                    // A genuine wrap of the CURRENTLY playing pattern
                    // (step totalSteps-1 -> step 0), not just "step index
                    // happens to be 0".
                    boundaryCrossed = currentStepIndex == 0
                                   && sequencedLastStepIndex_ == totalSteps - 1;
                }

                if (boundaryCrossed)
                {
                    applyPatternRecallFromState (r.state, pendingPatternNote_);

                    // Re-derive against the just-recalled pattern's own
                    // resolution/length so the lookup below reads the NEW
                    // grid at the correct width.
                    view = effectiveSequencer (r.state);
                    stepBeats = noteValueBeats (view.stepResolutionIndex);
                    totalSteps = view.columns;

                    if (stepBeats > 0.0 && totalSteps > 0)
                    {
                        absoluteStepIndex = static_cast<std::int64_t> (
                            std::floor (samplePpq / stepBeats));
                        currentStepIndex = static_cast<int> (
                            ((absoluteStepIndex % totalSteps) + totalSteps) % totalSteps);
                    }
                }
            }

            // --- step boundary ------------------------------------------
            if (stepBeats > 0.0 && totalSteps > 0
                && currentStepIndex != sequencedLastStepIndex_)
            {
                sequencedLastStepIndex_ = currentStepIndex;
                playingStepIndex_ = currentStepIndex;

                const int rowsEffective =
                    std::min (view.rows, static_cast<int> (r.slices.size()));

                for (int row = 0; row < rowsEffective; ++row)
                {
                    const auto cell = static_cast<std::size_t> (row)
                                        * static_cast<std::size_t> (totalSteps)
                                    + static_cast<std::size_t> (currentStepIndex);
                    const int styleOrdinal = (*view.grid)[cell];

                    if (styleOrdinal >= 0)
                    {
                        startSequencedPick (r, view, row, currentStepIndex,
                                            static_cast<std::size_t> (row));
                        break;  // structural monophony: one filled row per column
                    }
                }
            }

            // --- Subdivide retrigger, every SAMPLE -----------------------
            if (subdivisionActive_
                && subdivisionRow_ >= 0
                && subdivisionRow_ < static_cast<int> (r.slices.size()))
            {
                const double sinceWindowStart = renderer_.windowElapsedSamples();
                const double windowLength = renderer_.windowLengthSamples();

                if (sinceWindowStart < windowLength
                    && sinceWindowStart >= nextSubdivisionOffsetSamples_)
                {
                    const double remainingWindow =
                        std::max (0.0, windowLength - sinceWindowStart);
                    const double tickLength =
                        std::min (subdivisionTickLengthSamples_, remainingWindow);

                    // Restart the SAME pick without touching its DSP or the
                    // window clock (whole-window sweeps glide across the
                    // whole step underneath these retriggers).
                    const bool isTapeStop =
                        renderer_.currentPick().style == state::PlaybackStyle::tapeStop;
                    renderer_.retrigger (
                        r.slices[static_cast<std::size_t> (subdivisionRow_)].startFrame,
                        tickLength,
                        isTapeStop ? tickLength
                                   : renderer_.currentPick().tapeStopDurationHostSamples);
                    ++picksStarted_;

                    nextSubdivisionOffsetSamples_ += subdivisionTickLengthSamples_;
                }
            }
        }

        // The window clock advances every sample in clock AND sequenced
        // modes (it doubles as the Subdivide retrigger clock), then the
        // voice renders.
        renderer_.tickWindowClock();
        renderSampleInto (r, i);
    }
}

void VoiceScheduler::requestPerformanceRecall (const state::PluginState& state,
                                               int midiNote, bool hostTransportPlaying)
{
    if (midiNote < 0 || midiNote >= state::kNumMidiNotes)
        return;

    const auto& perf = state.performance;

    if (midiNote == perf.focusedSlot)
    {
        // The focused slot's key always plays the live working state
        // immediately (Quantize Recall never defers it).
        performancePlaybackIsFocused_ = true;
        performanceRecallPending_ = true;
        return;
    }

    const auto& slot = perf.bank[static_cast<std::size_t> (midiNote)];

    if (! slot.populated)
        return;  // empty, unfocused slot -- no-op

    if (perf.quantizeRecallEnabled && hostTransportPlaying)
    {
        // Defer to the next interval grid point; a newer note-on before
        // the boundary overwrites this pending note.
        pendingPerformanceNote_ = midiNote;
        performanceBoundaryArmed_ = false;
        return;
    }

    applyPerformanceRecallFromState (state, midiNote);
}

void VoiceScheduler::applyPerformanceRecallFromState (const state::PluginState& state,
                                                      int note)
{
    const auto& perf = state.performance;

    if (note == perf.focusedSlot)
    {
        performancePlaybackIsFocused_ = true;
        performanceRecallPending_ = true;
        return;
    }

    const auto& slot = perf.bank[static_cast<std::size_t> (note)];

    if (! slot.populated)
        return;  // emptied between arming and the boundary -- no-op

    // Freeze THIS block's snapshot: later bank edits can never disturb a
    // pick already sounding.
    performanceFrozenSnapshot_ = slot;
    performancePlaybackIsFocused_ = false;
    performanceRecallPending_ = true;
}

void VoiceScheduler::runPerformance (Run& r)
{
    if (armedMode_ != ArmedMode::performance)
    {
        // Just entered the mode or transport just started: silent until
        // the first note-on this session.
        renderer_.clearPick();
        armedMode_ = ArmedMode::performance;
    }

    for (int i = 0; i < r.numSamples && ! bailUntilBlockEnd_; ++i)
    {
        const double samplePpq = r.transport.ppqStart + static_cast<double> (i) * r.ppqPerSample;

        // --- Quantize Recall boundary, checked every SAMPLE -----------
        if (pendingPerformanceNote_ >= 0)
        {
            bool boundaryCrossed = ! r.transport.playing;   // no beat position to quantize against

            if (r.transport.playing)
            {
                if (! performanceBoundaryArmed_)
                {
                    const double intervalBeats =
                        std::max (noteValueBeats (
                                      r.state.performance.quantizeRecallIntervalIndex),
                                  1.0e-6);
                    const auto intervalIndex = static_cast<std::int64_t> (
                        std::floor (samplePpq / intervalBeats));
                    performanceBoundaryPpq_ =
                        static_cast<double> (intervalIndex + 1) * intervalBeats;
                    performanceBoundaryArmed_ = true;
                }

                boundaryCrossed = samplePpq >= performanceBoundaryPpq_;
            }

            if (boundaryCrossed)
            {
                applyPerformanceRecallFromState (r.state, pendingPerformanceNote_);
                pendingPerformanceNote_ = -1;
                performanceBoundaryArmed_ = false;
            }
        }

        bool needsFreshPick = performanceRecallPending_;
        performanceRecallPending_ = false;

        // Loop on rechains the SAME segment+style once it runs its course;
        // loop off goes silent until the next note-on.
        if (! needsFreshPick && renderer_.hasPick() && renderer_.finished (r.ctx))
        {
            const auto& activeState = performancePlaybackIsFocused_
                ? r.state.performance.workingState
                : performanceFrozenSnapshot_;

            if (activeState.loop)
                needsFreshPick = true;
            else
                renderer_.clearPick();
        }

        if (needsFreshPick)
        {
            const auto& activeState = performancePlaybackIsFocused_
                ? r.state.performance.workingState
                : performanceFrozenSnapshot_;

            if (activeState.populated)
            {
                // The focused segment IS the shared trim; any other slot
                // plays its own frozen trim.
                const Slice segment {
                    performancePlaybackIsFocused_ ? r.state.sample.trimStartFrame
                                                  : activeState.trimStartFrame,
                    performancePlaybackIsFocused_ ? r.state.sample.trimEndFrame
                                                  : activeState.trimEndFrame
                };

                const auto style = styleFromOrdinal (activeState.style);
                const bool nativeRate = ! activeState.sync;
                const double effectiveRate = nativeRate
                    ? r.ctx.srConversionRatio
                    : r.ctx.playbackRate;

                auto prepared = preparePick (style, segment, activeState.params,
                                             r.ctx.hostSampleRate, effectiveRate,
                                             r.transport.bpm);

                double pickLength;

                if (style == state::PlaybackStyle::pingPong)
                    pickLength = 2.0 * prepared.naturalLengthHostSamples;
                else if (style == state::PlaybackStyle::stretch)
                    pickLength = prepared.naturalLengthHostSamples
                               * static_cast<double> (activeState.params.grainSpeed);
                else if (style == state::PlaybackStyle::scratch)
                    pickLength = prepared.scratchCycleHostSamples;
                else
                    pickLength = prepared.naturalLengthHostSamples;

                // Bounce/fold styles never exhaust their slice by position
                // -- they end when their DECLARED window runs out (the
                // original gave them a finite schedule end the same way).
                const bool foldingStyle =
                    style == state::PlaybackStyle::pingPong
                 || style == state::PlaybackStyle::stretch
                 || style == state::PlaybackStyle::scratch;

                PickExtras extras;
                extras.nativeRate = nativeRate;
                extras.useDurationGate = foldingStyle;

                commitPick (segment, activeState.params, prepared, style,
                            pickLength, prepared.naturalLengthHostSamples,
                            /*filterWholeWindow*/ false, /*flangerWholeWindow*/ false,
                            /*beatQuantized*/ false, /*quantizedStretchRatio*/ 1.0,
                            extras);
            }
        }

        renderSampleInto (r, i);
    }
}

void VoiceScheduler::controlNoteOn (int noteNumber, float velocity01, int baseNote,
                                    int numAvailableSlices)
{
    // Keyswitches sit below base, slices at or above it; the ranges never
    // overlap by construction.
    const int keyswitchStyle = baseNote - 1 - noteNumber;

    if (keyswitchStyle >= 0 && keyswitchStyle < state::kNumPlaybackStyles)
    {
        controlActiveStyleOrdinal_ = keyswitchStyle;
        return;  // keyswitches never make sound
    }

    const int sliceIndex = noteNumber - baseNote;

    if (sliceIndex < 0 || sliceIndex >= numAvailableSlices)
        return;  // outside both ranges -- no-op

    controlNoteOnPending_ = true;
    controlPendingSliceIndex_ = sliceIndex;
    controlPendingStyleOrdinal_ = controlActiveStyleOrdinal_;
    controlPendingVelocityGain_ = std::clamp (velocity01, 0.0f, 1.0f);
    controlPendingNoteNumber_ = noteNumber;
}

void VoiceScheduler::controlNoteOff (int noteNumber, bool gateModeActive) noexcept
{
    if (! gateModeActive)
        return;  // Trigger mode ignores note-off entirely

    if (noteNumber != controlSoundingNote_ || ! renderer_.hasPick())
        return;  // not the note that's actually still sounding

    renderer_.beginGateRelease();
}

void VoiceScheduler::runControl (Run& r)
{
    if (armedMode_ != ArmedMode::control)
    {
        // Just entered the mode or transport just started: silent until a
        // note-on. The owned keyswitch style re-seeds from state here --
        // afterwards only keyswitches move it.
        renderer_.clearPick();
        controlSoundingNote_ = -1;
        controlActiveStyleOrdinal_ = r.state.control.activeStyle;
        armedMode_ = ArmedMode::control;
    }

    for (int i = 0; i < r.numSamples && ! bailUntilBlockEnd_; ++i)
    {
        bool needsFreshPick = controlNoteOnPending_;
        controlNoteOnPending_ = false;

        // One-shot: when a pick runs its course it goes silent and stays
        // silent until the next note-on (no loop concept in Control mode).
        if (! needsFreshPick && renderer_.hasPick() && renderer_.finished (r.ctx))
            renderer_.clearPick();

        if (needsFreshPick && controlPendingSliceIndex_ >= 0
            && controlPendingSliceIndex_ < static_cast<int> (r.slices.size()))
        {
            controlSoundingNote_ = controlPendingNoteNumber_;

            const auto& slice = r.slices[static_cast<std::size_t> (controlPendingSliceIndex_)];
            const auto style = styleFromOrdinal (controlPendingStyleOrdinal_);

            auto prepared = preparePick (style, slice, r.state.control.styleParams,
                                         r.ctx.hostSampleRate, r.ctx.playbackRate,
                                         r.transport.bpm);

            double pickLength;

            if (style == state::PlaybackStyle::pingPong)
                pickLength = 2.0 * prepared.naturalLengthHostSamples;
            else if (style == state::PlaybackStyle::stretch)
                pickLength = prepared.naturalLengthHostSamples
                           * static_cast<double> (r.state.control.styleParams.grainSpeed);
            else if (style == state::PlaybackStyle::scratch)
                pickLength = prepared.scratchCycleHostSamples;
            else
                pickLength = prepared.naturalLengthHostSamples;

            // Same folding-style duration gate as Performance mode.
            const bool foldingStyle =
                style == state::PlaybackStyle::pingPong
             || style == state::PlaybackStyle::stretch
             || style == state::PlaybackStyle::scratch;

            PickExtras extras;
            extras.velocityGain = controlPendingVelocityGain_;
            extras.useDurationGate = foldingStyle;

            commitPick (slice, r.state.control.styleParams, prepared, style,
                        pickLength, prepared.naturalLengthHostSamples,
                        /*filterWholeWindow*/ false, /*flangerWholeWindow*/ false,
                        /*beatQuantized*/ false, /*quantizedStretchRatio*/ 1.0,
                        extras);
        }

        renderSampleInto (r, i);
    }
}

} // namespace nedit::engine
