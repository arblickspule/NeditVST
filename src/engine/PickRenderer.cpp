#include "PickRenderer.h"
#include "Easing.h"
#include "Fold.h"

#include <algorithm>
#include <cmath>

namespace nedit::engine {

namespace {

    [[nodiscard]] double clamp01 (double v) noexcept
    {
        return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
    }

    // NaN/inf-safe double -> source read index. Clamp in DOUBLE space
    // FIRST: casting a non-representable double (NaN, +-inf, > int64 max)
    // to int64 is itself UB, so a clamp applied after the cast guards
    // nothing. The negated comparison routes NaN to 0 alongside negatives.
    [[nodiscard]] std::int64_t clampedSourceIndex (double pos,
                                                   std::int64_t numFrames) noexcept
    {
        if (! (pos >= 0.0))   // negatives AND NaN
            return 0;
        if (pos >= static_cast<double> (numFrames - 1))
            return numFrames - 1;
        return static_cast<std::int64_t> (pos);
    }

    // Shared Sweep In/Out interpolation (Bitcrush, Flanger): mode 0 static,
    // 1 = set value -> extreme, 2 = extreme -> set value.
    [[nodiscard]] float sweptValue (float setValue, state::SweepMode mode, float extreme,
                                    double progress) noexcept
    {
        if (mode == state::SweepMode::sweepIn)
            return static_cast<float> (setValue + (extreme - setValue) * progress);

        if (mode == state::SweepMode::sweepOut)
            return static_cast<float> (extreme + (setValue - extreme) * progress);

        return setValue;
    }

} // namespace

void PickRenderer::prepare (double hostSampleRate)
{
    filter.prepare (hostSampleRate);
    flanger.prepare (hostSampleRate, state::kMaxFlangerDelayMs);
    clearPick();
}

void PickRenderer::startPick (const PickParams& newPick)
{
    pick = newPick;
    active = true;

    currentPosition = static_cast<double> (pick.sliceStartFrame);
    samplesSincePick = 0.0;
    gateReleaseActive = false;
    gateReleaseElapsedSamples = 0.0;

    // pickJustStarted: every per-pick DSP scratchpad resets so nothing
    // bleeds from the previous pick (harmless when unused this pick).
    filter.reset();
    filter.setResonance (pick.params.filterResonance);
    filter.setType (pick.params.filterType);

    bitcrusher.reset();
    flanger.reset();
    stretcher.reset (static_cast<double> (pick.sliceStartFrame));
}

bool PickRenderer::finished (const BlockContext& ctx) const noexcept
{
    if (! active)
        return true;

    const bool tapeStopActive = pick.style == state::PlaybackStyle::tapeStop;
    const bool stretchActive = pick.style == state::PlaybackStyle::stretch;

    if (tapeStopActive)
        return samplesSincePick >= pick.tapeStopDurationHostSamples;

    if (stretchActive || pick.useDurationGate)
        return samplesSincePick >= pick.pickLengthHostSamples;

    const bool bounceActive = pick.style == state::PlaybackStyle::pingPong
                           || pick.style == state::PlaybackStyle::scratch;
    const bool extendedRange = bounceActive || stretchActive;
    const auto schedulingEnd = extendedRange
        ? pick.schedulingEndFrame
        : std::min (pick.schedulingEndFrame, ctx.sourceFrames);

    return currentPosition >= static_cast<double> (schedulingEnd - 1);
}

double PickRenderer::computeGain (const BlockContext& ctx, bool bounceActive,
                                  bool tapeStopActive, double tapeStopRateMultiplier) const noexcept
{
    // Fades clamp to half the pick's effective length so a very short
    // pick can't have overlapping/inverted fades silencing it entirely.
    const double halfPickLength = pick.pickLengthHostSamples * 0.5;
    const double fadeInSamples = std::min (ctx.fadeInSamplesRequested, halfPickLength);
    const double fadeOutSamples = std::min (ctx.fadeOutSamplesRequested, halfPickLength);
    const double samplesRemaining = pick.pickLengthHostSamples - samplesSincePick;

    double gain = 1.0;

    if (fadeInSamples > 0.0 && samplesSincePick < fadeInSamples)
        gain = samplesSincePick / fadeInSamples;

    if (tapeStopActive)
    {
        // Gain rides the decel curve, REPLACING the normal fade-out --
        // if rate hit 0 while gain stayed full the engine would hold a
        // single sample (a buzz) instead of fading. fadeIn unaffected.
        gain = std::min (gain, tapeStopRateMultiplier);
    }
    else
    {
        if (fadeOutSamples > 0.0 && samplesRemaining < fadeOutSamples)
            gain = std::min (gain, samplesRemaining / fadeOutSamples);

        // Bounce turnaround (Ping-Pong, and Scratch via the same flag):
        // the midpoint isn't a true edit, so it clicks like an unfaded
        // boundary would -- apply the same fade envelope around it,
        // curve-shaped like Tape Stop's decel. One midpoint per pick.
        if (bounceActive)
        {
            const double before = pick.midpointHostSamples - samplesSincePick;
            const double after = samplesSincePick - pick.midpointHostSamples;

            if (before >= 0.0 && before < fadeOutSamples)
                gain = std::min (gain, applyCurveShape (before / fadeOutSamples,
                                                        pick.params.curveShape));

            if (after >= 0.0 && after < fadeInSamples)
                gain = std::min (gain, applyCurveShape (after / fadeInSamples,
                                                        pick.params.curveShape));
        }
    }

    return clamp01 (gain);
}

bool PickRenderer::renderSample (const BlockContext& ctx, float* const* outAdd, int numOutChannels)
{
    if (! active || ctx.source == nullptr || ctx.sourceFrames <= 0 || ctx.sourceChannels <= 0)
        return false;

    const bool pingPongActive = pick.style == state::PlaybackStyle::pingPong;
    const bool tapeStopActive = pick.style == state::PlaybackStyle::tapeStop;
    const bool stretchActive = pick.style == state::PlaybackStyle::stretch;
    const bool filterSweepActive = pick.style == state::PlaybackStyle::filterDown
                                || pick.style == state::PlaybackStyle::filterUp;
    const bool bitcrushActive = pick.style == state::PlaybackStyle::bitcrush;
    const bool scratchActive = pick.style == state::PlaybackStyle::scratch;
    const bool flangerActive = pick.style == state::PlaybackStyle::flanger;
    const bool bounceActive = pingPongActive || scratchActive;

    // Performance Sync-off: substitute the native (unsynced) rate at
    // every site that would otherwise read playbackRate.
    const double effectivePickPlaybackRate = pick.nativeRate ? ctx.srConversionRatio
                                                             : ctx.playbackRate;

    // --- per-pick / whole-window progress fractions -----------------------
    const auto pickProgress = [this]() noexcept
    {
        return pick.pickLengthHostSamples > 0.0
             ? clamp01 (samplesSincePick / pick.pickLengthHostSamples)
             : 1.0;
    };

    const auto windowProgress = [this]() noexcept
    {
        return windowLengthHost > 0.0
             ? clamp01 (samplesSinceWindowStart / windowLengthHost)
             : 1.0;
    };

    // --- Bitcrush swept values (per-pick progress, linear -- these are
    // small integer counts, not a log frequency range) ---------------------
    int effectiveBitcrushHold = 1;
    int effectiveBitDepth = 1;

    if (bitcrushActive)
    {
        const double progress = pickProgress();

        const float holdValue = sweptValue (pick.params.srReduction,
                                            pick.params.srReductionMode,
                                            state::kMaxSrReduction, progress);
        effectiveBitcrushHold = std::max (1, static_cast<int> (std::lround (holdValue)));

        const float depthValue = sweptValue (pick.params.bitDepth,
                                             pick.params.bitDepthMode,
                                             state::kMinBitDepth, progress);
        effectiveBitDepth = std::clamp (static_cast<int> (std::lround (depthValue)), 1, 24);

        bitcrusher.tick (effectiveBitcrushHold);
    }

    // --- Flanger swept values (whole-window where a window exists) --------
    float effectiveFlangerMix = 0.0f;
    float effectiveFlangerFeedback = 0.0f;

    if (flangerActive)
    {
        const double progress = pick.flangerWholeWindow ? windowProgress() : pickProgress();

        const float delayMs = sweptValue (pick.params.flangerDelayMs,
                                          pick.params.flangerDelayMode,
                                          state::kMaxFlangerDelayMs, progress);
        const int delaySamples = std::clamp (
            static_cast<int> (std::lround ((static_cast<double> (delayMs) / 1000.0)
                                           * ctx.hostSampleRate)),
            1, flanger.maxDelaySamples());
        flanger.tick (delaySamples);

        effectiveFlangerMix = std::clamp (sweptValue (pick.params.flangerMix,
                                                      pick.params.flangerMixMode,
                                                      state::kMaxFlangerMix, progress),
                                          0.0f, 1.0f);
        effectiveFlangerFeedback = std::clamp (sweptValue (pick.params.flangerFeedback,
                                                           pick.params.flangerFeedbackMode,
                                                           state::kMaxFlangerFeedback, progress),
                                               0.0f, state::kMaxFlangerFeedback);
    }

    // --- Per-style volume (constant gain, captured at pick start) ----------
    // One independent value per PlaybackStyle (issue #7); the scheduler
    // captures the style's own volume into the pick params, so this is a
    // bare multiplier here.
    const double volumeGain = clamp01 (static_cast<double> (
        pick.params.getStyleVolume (pick.style)));

    // --- Control-mode gains -------------------------------------------------
    const double velocityGain = pick.velocityGain;
    const double gateReleaseGain = gateReleaseActive
        ? clamp01 (1.0 - gateReleaseElapsedSamples
                       / std::max (1.0, ctx.fadeOutSamplesRequested))
        : 1.0;

    // --- scheduling bounds ---------------------------------------------------
    // Bounce/Stretch ends can legitimately exceed the source (the read is
    // folded / stretcher-bounded); only Forward/Tape Stop read raw.
    const bool extendedRange = bounceActive || stretchActive;
    const auto schedulingEnd = extendedRange
        ? pick.schedulingEndFrame
        : std::min (pick.schedulingEndFrame, ctx.sourceFrames);

    const bool tapeStopPositionExhausted =
        tapeStopActive && currentPosition >= static_cast<double> (schedulingEnd - 1);

    const bool pickWithinSchedule = tapeStopActive
        ? (samplesSincePick < pick.tapeStopDurationHostSamples)
        : (stretchActive || pick.useDurationGate)
            ? (samplesSincePick < pick.pickLengthHostSamples)
            : (currentPosition < static_cast<double> (schedulingEnd - 1));

    if (! pickWithinSchedule)
        return false;

    // --- Tape Stop decel ------------------------------------------------------
    double tapeStopRateMultiplier = 1.0;

    if (tapeStopActive)
    {
        const double rawProgress = pick.tapeStopDurationHostSamples > 0.0
            ? clamp01 (samplesSincePick / pick.tapeStopDurationHostSamples)
            : 1.0;
        tapeStopRateMultiplier = 1.0 - applyCurveShape (rawProgress, pick.params.curveShape);
    }

    // --- envelope --------------------------------------------------------------
    double gain = computeGain (ctx, bounceActive, tapeStopActive, tapeStopRateMultiplier);
    gain *= volumeGain;
    gain *= velocityGain;
    gain *= gateReleaseGain;

    // --- Filter Down/Up cutoff (log-scale, once per sample) ---------------------
    if (filterSweepActive)
    {
        const double progress = pick.filterWholeWindow ? windowProgress() : pickProgress();

        const bool isUp = pick.style == state::PlaybackStyle::filterUp;
        const double sweepStartHz = isUp ? static_cast<double> (kFilterSweepEndHz)
                                         : static_cast<double> (kFilterSweepStartHz);
        const double sweepEndHz = isUp ? static_cast<double> (kFilterSweepStartHz)
                                       : static_cast<double> (kFilterSweepEndHz);

        filter.setCutoffFrequency (static_cast<float> (
            sweepStartHz * std::pow (sweepEndHz / sweepStartHz, progress)));
    }

    // --- fold configuration --------------------------------------------------
    // Tape Stop never folds (its freeze mechanism handles overrun);
    // bounce styles fold pingPong; everything else loops so an extended
    // step repeats its natural unit.
    const FoldStyle foldStyle = bounceActive ? FoldStyle::pingPong
                              : tapeStopActive ? FoldStyle::forward
                                               : FoldStyle::loop;

    // Sequenced-mode Ping-Pong reflects within HALF the slice content so
    // a full round trip fits one step at normal rate.
    const double pingPongFoldLength = pick.halfSliceFold
        ? static_cast<double> (pick.sliceLengthFrames) * 0.5
        : static_cast<double> (pick.sliceLengthFrames);

    // Scratch's fold length: one LEG of its Rate cycle, in source frames.
    const double scratchFoldLength = scratchActive
        ? pick.scratchCycleLengthHostSamples * 0.5 * effectivePickPlaybackRate
        : 0.0;

    const double bounceFoldLength = scratchActive ? scratchFoldLength : pingPongFoldLength;

    const auto forwardCurve = scratchActive ? pick.params.scratchForwardCurve
                                            : state::EasingCurve::linear;
    const auto backwardCurve = scratchActive ? pick.params.scratchBackwardCurve
                                             : state::EasingCurve::linear;

    const int usableSourceChannels = std::min (ctx.sourceChannels,
                                               GranularStretcher::kMaxChannels);

    // --- render ------------------------------------------------------------------
    if ((ctx.timeStretchMode && ! pick.nativeRate) || stretchActive)
    {
        // Granular path. Stretch always comes here (a character effect,
        // independent of the pitch-mode toggle) with its own parameters.
        double grainOutputHop = ctx.outputHopSamples;
        // Grain Speed (global): a re-granulation density divisor on the
        // rate-matching hop. At the default 1.0 the source hop marches at
        // the playback rate exactly (clean pitch-preserving granular);
        // higher values re-granulate the same source region more times
        // (choppier character). Tape-stop keeps its own decel; the Stretch
        // style's own grain speed overwrites below.
        double grainSourceHop = tapeStopPositionExhausted ? 0.0
                              : tapeStopActive ? (ctx.sourceHopSamples * tapeStopRateMultiplier)
                              : ((pick.beatQuantized ? (ctx.outputHopSamples * ctx.srConversionRatio
                                                        * pick.quantizedStretchRatio)
                                                     : ctx.sourceHopSamples)
                                 / ctx.grainSpeed);
        double grainSize = ctx.grainSizeHostSamples;
        double grainPitchRatio = tapeStopActive ? (ctx.pitchRatio * tapeStopRateMultiplier)
                                                : ctx.pitchRatio;
        auto windowShape = ctx.grainWindowShape;

        if (stretchActive)
        {
            grainSize = static_cast<double> (pick.params.grainSizeMs) / 1000.0
                      * ctx.hostSampleRate;
            grainOutputHop = grainSize * 0.5;  // same fixed 50% overlap convention

            // Grain Speed: a fixed character constant -- how fast grains
            // march through the source for ONE pass. Duration is governed
            // separately by the declared pick length + the loop fold.
            grainSourceHop = grainOutputHop
                           * (effectivePickPlaybackRate
                              / static_cast<double> (pick.params.grainSpeed));

            grainPitchRatio = 1.0;  // self-contained: no user pitch shift
            windowShape = GranularStretcher::WindowShape::hardEdge;
        }

        float channelSums[GranularStretcher::kMaxChannels] = {};
        stretcher.renderAndAdvance (ctx.source, usableSourceChannels, ctx.sourceFrames,
                                    grainOutputHop, grainSourceHop,
                                    static_cast<double> (pick.sliceStartFrame),
                                    bounceFoldLength, foldStyle,
                                    grainSize, ctx.srConversionRatio, grainPitchRatio,
                                    windowShape, channelSums,
                                    forwardCurve, backwardCurve);

        for (int outCh = 0; outCh < numOutChannels; ++outCh)
        {
            const int srcCh = std::min (std::min (outCh, ctx.sourceChannels - 1),
                                        GranularStretcher::kMaxChannels - 1);
            float sample = channelSums[srcCh] * static_cast<float> (gain);

            if (filterSweepActive)
                sample = filter.processSample (outCh, sample);

            if (bitcrushActive)
                sample = bitcrusher.process (outCh, sample, effectiveBitDepth);

            if (flangerActive)
                sample = flanger.process (outCh, sample, effectiveFlangerMix,
                                          effectiveFlangerFeedback);

            outAdd[outCh][0] += sample;
        }
    }
    else
    {
        // Direct-read (Repitch) path.
        double positionForRead = currentPosition;

        if (tapeStopPositionExhausted)
        {
            // "Stuck tape": loop a short window of REAL audio ending at
            // the slice boundary, its playback still driven by the
            // (decaying) position advance so the loop slows down with the
            // decel. A forward sawtooth loop -- the periodic click reads
            // as part of the character.
            const double freezeLoopLength = std::max (1.0,
                (kTapeStopFreezeLoopMs / 1000.0) * ctx.sourceSampleRate);

            const double freezeWindowEnd = static_cast<double> (schedulingEnd - 1);
            const double freezeWindowStart = std::max (
                static_cast<double> (pick.sliceStartFrame),
                freezeWindowEnd - freezeLoopLength);
            const double freezeWindowLength = std::max (1.0, freezeWindowEnd - freezeWindowStart);

            const double elapsedSinceFreeze = std::max (0.0, currentPosition - freezeWindowEnd);
            positionForRead = freezeWindowStart
                            + std::fmod (elapsedSinceFreeze, freezeWindowLength);
        }

        // Shared fold -- identical to the granular path's grain-start
        // scheduling, so Ping-Pong/Scratch behave the same in both pitch
        // modes. Identity for Forward/Tape Stop.
        const double folded = static_cast<double> (pick.sliceStartFrame)
                            + foldPosition (positionForRead
                                                - static_cast<double> (pick.sliceStartFrame),
                                            bounceFoldLength, foldStyle,
                                            forwardCurve, backwardCurve);

        const auto idx0 = clampedSourceIndex (folded, ctx.sourceFrames);
        const auto idx1 = std::min<std::int64_t> (idx0 + 1, ctx.sourceFrames - 1);
        const auto frac = static_cast<float> (folded - static_cast<double> (idx0));

        for (int outCh = 0; outCh < numOutChannels; ++outCh)
        {
            const int srcCh = std::min (outCh, ctx.sourceChannels - 1);
            const float s0 = ctx.source[srcCh][static_cast<std::size_t> (idx0)];
            const float s1 = ctx.source[srcCh][static_cast<std::size_t> (idx1)];
            float sample = (s0 + frac * (s1 - s0)) * static_cast<float> (gain);

            if (filterSweepActive)
                sample = filter.processSample (outCh, sample);

            if (bitcrushActive)
                sample = bitcrusher.process (outCh, sample, effectiveBitDepth);

            if (flangerActive)
                sample = flanger.process (outCh, sample, effectiveFlangerMix,
                                          effectiveFlangerFeedback);

            outAdd[outCh][0] += sample;
        }
    }

    if (flangerActive)
        flanger.advance();

    // --- advance the shared scheduling position -----------------------------
    // Tape Stop layers its rate multiplier here (this is what makes the
    // read actually slow down); beat-quantize substitutes its per-pick
    // stretch ratio in exactly the place playbackRate is built from
    // repitchRatio.
    const double effectivePlaybackRate = tapeStopActive
        ? (effectivePickPlaybackRate * tapeStopRateMultiplier)
        : pick.beatQuantized ? (ctx.srConversionRatio * pick.quantizedStretchRatio)
                             : effectivePickPlaybackRate;

    currentPosition += effectivePlaybackRate;
    samplesSincePick += 1.0;

    // Control-mode gate release: force-stop at silence.
    if (gateReleaseActive)
    {
        gateReleaseElapsedSamples += 1.0;

        if (gateReleaseElapsedSamples >= ctx.fadeOutSamplesRequested)
        {
            active = false;
            gateReleaseActive = false;
        }
    }

    return true;
}

} // namespace nedit::engine
