#include "SlicerEngine.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace
{

    // Step 46: shared 0..1 progress-curve remap for Curve Shape -- Linear
    // is the identity (today's behaviour, unchanged); Exponential eases
    // in (t*t -- slow start, fast finish), which reads as a more natural
    // "surge before stopping" for Tape Stop's decel and a snappier
    // turnaround for Ping-Pong, without needing two separate formulas per
    // style. First-pass curve, not meant to be the only shape ever
    // offered -- see setCurveShape()'s doc comment in SlicerModel.h.
    double applyCurveShape (double t, int curveShapeIndex)
    {
        if (curveShapeIndex == 1)
            return t * t;

        return t;
    }

}
int SlicerEngine::nearestNoteValueIndex (double targetBeats)
{
    int bestIndex = 0;
    double bestDistance = std::numeric_limits<double>::max();

    for (int i = 0; i < SlicerModel::numNoteValueOptions; ++i)
    {
        const double distance = std::abs (SlicerModel::getNoteValueBeats (i) - targetBeats);

        if (distance < bestDistance)
        {
            bestDistance = distance;
            bestIndex = i;
        }
    }

    return bestIndex;
}
SlicerEngine::BeatQuantizeResult SlicerEngine::computeBeatQuantizeTarget (
    int sliceLength, bool pingPong, double sampleSampleRate, double originalBpm, double hostBpm)
{
    BeatQuantizeResult result;

    if (sliceLength <= 0 || originalBpm <= 0.0 || hostBpm <= 0.0)
        return result;

    // Ping-Pong quantizes the FULL ROUND TRIP (there and back) as one
    // unit, before the fold -- same "whichever unit should land on the
    // beat grid" span the pick-length calculation elsewhere uses.
    const double sliceNaturalSourceSeconds = (double) (pingPong ? 2 * sliceLength : sliceLength) / sampleSampleRate;
    const double naturalBeats = sliceNaturalSourceSeconds * originalBpm / 60.0;

    // The note-value palette only goes up to 1n (4 beats, one bar) --
    // searching it for anything longer would wrongly clamp every
    // multi-bar-length slice down to a single bar, regardless of how many
    // bars it actually spans. For those, decompose into whole bars plus a
    // sub-bar remainder, and only run the (still fine-grained, unchanged)
    // palette search on the remainder -- rounding the WHOLE length to the
    // nearest bar (an earlier, cruder version of this fix) was accurate
    // only by coincidence for lengths already near a whole-bar boundary,
    // and crushed anything else (e.g. a 1.3-bar slice) down to a flat
    // whole-bar count. The palette search itself remains exactly as
    // before for anything within one bar to start with, where it was
    // never broken.
    double quantizedBeats;

    if (naturalBeats > 4.0)
    {
        const double wholeBars = std::floor (naturalBeats / 4.0);
        const double remainderBeats = naturalBeats - (wholeBars * 4.0);

        // Below half the smallest palette entry (128n, index 0 -- the
        // palette is sorted shortest to longest), treat as no remainder
        // at all -- otherwise a slice that's already almost exactly N
        // whole bars gets a spurious tiny addition tacked on for no
        // audible reason.
        const double smallestPaletteBeats = SlicerModel::getNoteValueBeats (0);
        const double quantizedRemainder = (remainderBeats > smallestPaletteBeats * 0.5)
            ? SlicerModel::getNoteValueBeats (nearestNoteValueIndex (remainderBeats))
            : 0.0;

        quantizedBeats = (wholeBars * 4.0) + quantizedRemainder;
    }
    else
    {
        quantizedBeats = SlicerModel::getNoteValueBeats (nearestNoteValueIndex (naturalBeats));
    }

    const double targetHostSeconds = quantizedBeats * (60.0 / hostBpm);

    if (targetHostSeconds <= 0.0)
        return result;

    result.quantized = true;
    result.stretchRatio = sliceNaturalSourceSeconds / targetHostSeconds;
    result.targetHostSeconds = targetHostSeconds;
    return result;
}
double SlicerEngine::computeScratchCycleLengthHostSamples (int rateIndex, int sliceLength,
                                                                     double hostBpm, double hostSampleRate, double playbackRate)
{
    if (sliceLength <= 0 || hostBpm <= 0.0 || hostSampleRate <= 0.0 || playbackRate <= 0.0)
        return 0.0;

    const double rateBeats = SlicerModel::getNoteValueBeats (rateIndex);
    const double desiredCycleHostSamples = rateBeats * (60.0 / hostBpm) * hostSampleRate;
    const double desiredCycleSourceSamples = desiredCycleHostSamples * playbackRate;

    // One LEG of the cycle (half of it, the "there" or "back" half) is
    // clamped to the slice's own content length -- see this function's
    // own doc comment in SlicerEngine.h for why.
    const double clampedLegSourceSamples = juce::jmin (desiredCycleSourceSamples * 0.5, (double) sliceLength);
    return 2.0 * clampedLegSourceSamples / playbackRate;
}

//==============================================================================
// Construction / teardown / sample-rate setup
//==============================================================================
SlicerEngine::SlicerEngine (SlicerModel& modelToUse)
    : model (modelToUse)
{
    // Filter Down/Filter Up (Step 29/30): fixed type, set once here rather
    // than per-sample -- only the cutoff frequency needs to change during
    // playback (see processBlock()). Resonance is no longer fixed (Step
    // 45) -- see currentPickFilterSweepResonance, captured once per pick
    // and applied in the shared pickJustStarted block instead of here.
    // Sample rate/channel count get set properly in prepare(); the
    // defaults here are harmless placeholders until then.
    filterSweepFilter.setType (juce::dsp::StateVariableTPTFilterType::lowpass);
    filterSweepFilter.setResonance (SlicerModel::defaultFilterSweepResonance);

    // Engine per-pick state must not read past a replaced buffer / stale
    // boundaries -- SlicerModel calls this under model.sampleLock whenever
    // it invalidates pick state (loadSample, slice rebuilds).
    model.onPickStateInvalidated = [this]
    {
        hasCurrentPick = false;
        clockModeInitialized = false;
        clockCurrentPickValid = false;
    };
}

SlicerEngine::~SlicerEngine()
{
}

void SlicerEngine::prepare (double sampleRate, int samplesPerBlock)
{
    hasCurrentPick = false;
    clockModeInitialized = false;
    clockCurrentPickValid = false;
    resetWindowInitialized = false; // Step 34

    // Filter Sweep (Step 29) -- must be prepared with the real sample rate
    // before setCutoffFrequency() means anything; 2 channels covers this
    // plugin's stereo-only output (isBusesLayoutSupported() requires it).
    filterSweepFilter.prepare ({ sampleRate, (juce::uint32) samplesPerBlock, 2 });

    // Flanger delay line -- same "must be sized against the real sample
    // rate, not a construction-time guess" reasoning as filterSweepFilter
    // just above. Sized to comfortably hold flangerDelayTimeExtremeMs (the
    // Sweep In/Out target, and the slider's own max -- see
    // getSequencerCellParameterMax()) plus a little headroom, so the
    // longest delay this style can ever request always fits without
    // wrapping into not-yet-written buffer content. 2 channels matches
    // filterSweepFilter's own stereo-only assumption above.
    const int flangerDelayBufferLength = juce::jmax (4, juce::roundToInt ((SlicerModel::flangerDelayTimeExtremeMs / 1000.0) * sampleRate) + 4);
    flangerDelayBuffer.setSize (GranularStretcher::maxChannels, flangerDelayBufferLength);
    flangerDelayBuffer.clear();
    flangerDelayWriteIndex = 0;
}

void SlicerEngine::setTriggerMode (TriggerMode mode)
{
    model.triggerMode.store (mode);
    clockModeInitialized = false; // force a fresh window/pick on next block
    clockCurrentPickValid = false;
    resetWindowInitialized = false; // Step 34 -- same "start fresh, aligned" guarantee entering Slice Length mode
    sequencedModeInitialized = false; // Step 37 -- same guarantee entering Sequenced mode
    performanceModeInitialized = false; // Pass 1 -- same guarantee entering Performance mode

    // Pattern bank MIDI Learn only makes sense while Sequenced mode is
    // selected (it's the only mode whose UI can arm it) -- leaving the
    // mode abandons any learn still armed, so it can never later capture
    // a stale snapshot into the wrong slot after the user's moved on.
    // Same reasoning for a pattern switch left pending (Pass 2) -- its
    // boundary check only ever runs in the sequencedMode branch below,
    // so leaving the mode would otherwise strand it, silently applying
    // whenever Sequenced mode is re-entered later.
    if (mode != TriggerMode::sequenced)
    {
        model.midiLearnArmed.store (false);
        pendingPatternSwitchNote.store (-1);
    }

    // Performance mode's own editing focus (model.focusedPerformanceStateSlot)
    // deliberately persists across a mode change -- unlike Sequenced
    // mode's MIDI Learn above, there's nothing here that would go stale
    // by staying armed, since focus isn't a transient "waiting for the
    // next note-on" state; it just remembers which slot to resume
    // editing next time Performance mode is selected again.
}

void SlicerEngine::setPitchMode (PitchMode mode)
{
    model.pitchMode.store (mode);
    granularNeedsReseed.store (true); // reseed the grain engine mid-pick, from wherever playback currently is
}

void SlicerEngine::setAuditionActive (bool active)
{
    model.setAuditionActive (active);

    // Always start fresh from the current trim, not wherever a stale
    // position was left -- the model's plain setAuditionActive() store can't
    // do this anymore since the cursor moved here with the rest of the
    // per-pick scheduling state.
    if (active)
        auditionPosition = (double) model.trimStartSample.load();
}

void SlicerEngine::setPatternSwitchTiming (PatternSwitchTiming timing)
{
    model.setPatternSwitchTiming (timing);
    pendingPatternSwitchNote.store (-1); // changing the timing mode abandons any switch already pending under the old one
}

void SlicerEngine::setPerformanceQuantizeRecallEnabled (bool enabled)
{
    model.setPerformanceQuantizeRecallEnabled (enabled);
    pendingPerformanceRecallNote.store (-1); // changing the setting abandons any recall already pending under the old one
}

bool SlicerEngine::getRandomizeParametersForStyle (int index) const
{
    const juce::ScopedLock sl (model.sampleLock);

    if (index < 0 || index >= (int) randomizeParametersForStyle.size())
        return false;

    return randomizeParametersForStyle[(size_t) index];
}

void SlicerEngine::setRandomizeParametersForStyle (int index, bool shouldRandomize)
{
    const juce::ScopedLock sl (model.sampleLock);

    if (index >= 0 && index < (int) randomizeParametersForStyle.size())
        randomizeParametersForStyle[(size_t) index] = shouldRandomize;
}

SlicerEngine::PickStyleValues SlicerEngine::capturePickStyleValues (PickValueSource source, int row, int step) const
{
    PickStyleValues v;

    if (source == PickValueSource::stepOverride)
    {
        // Sequenced mode: this step's own override if it has one, else the
        // global value. Modes/discrete parameters round to int -- the same
        // conversions each original block applied at its own capture site.
        v.stretchGrainSizeMs = model.getSequencerCellParameterOverride (row, step, "Grain Size", model.stretchGrainSizeMsValue.load());
        v.stretchSpeedMultiplier = model.getSequencerCellParameterOverride (row, step, "Grain Speed", model.stretchSpeedMultiplierValue.load());
        v.filterSweepResonance = model.getSequencerCellParameterOverride (row, step, "Resonance", model.filterSweepResonanceValue.load());
        v.filterSweepType = juce::roundToInt (model.getSequencerCellParameterOverride (row, step, "Filter Type", (float) model.filterSweepFilterTypeValue.load()));
        v.curveShape = juce::roundToInt (model.getSequencerCellParameterOverride (row, step, "Curve Shape", (float) model.curveShapeValue.load()));
        v.bitcrushRateValue = model.getSequencerCellParameterOverride (row, step, "Sample Rate Reduction", model.bitcrushRateReductionGlobalValue.load());
        v.bitcrushRateMode = juce::roundToInt (model.getSequencerCellParameterOverride (row, step, "Sample Rate Reduction Mode", (float) model.bitcrushRateReductionModeGlobalValue.load()));
        v.bitcrushBitDepthValue = model.getSequencerCellParameterOverride (row, step, "Bit Depth", model.bitcrushBitDepthGlobalValue.load());
        v.bitcrushBitDepthMode = juce::roundToInt (model.getSequencerCellParameterOverride (row, step, "Bit Depth Mode", (float) model.bitcrushBitDepthModeGlobalValue.load()));
        v.flangerDelayValue = model.getSequencerCellParameterOverride (row, step, "Delay Time", model.flangerDelayTimeGlobalValue.load());
        v.flangerDelayMode = juce::roundToInt (model.getSequencerCellParameterOverride (row, step, "Delay Time Mode", (float) model.flangerDelayTimeModeGlobalValue.load()));
        v.flangerMixValue = model.getSequencerCellParameterOverride (row, step, "Mix", model.flangerMixGlobalValue.load());
        v.flangerMixMode = juce::roundToInt (model.getSequencerCellParameterOverride (row, step, "Mix Mode", (float) model.flangerMixModeGlobalValue.load()));
        v.flangerFeedbackValue = model.getSequencerCellParameterOverride (row, step, "Feedback", model.flangerFeedbackGlobalValue.load());
        v.flangerFeedbackMode = juce::roundToInt (model.getSequencerCellParameterOverride (row, step, "Feedback Mode", (float) model.flangerFeedbackModeGlobalValue.load()));
        v.volumeValue = model.getSequencerCellParameterOverride (row, step, "Volume", 1.0f);
        v.volumeMode = juce::roundToInt (model.getSequencerCellParameterOverride (row, step, "Volume Mode", 0.0f));
        v.scratchRateIndex = juce::roundToInt (model.getSequencerCellParameterOverride (row, step, "Rate", (float) model.scratchRateGlobalValue.load()));
        v.scratchForwardCurveIndex = juce::roundToInt (model.getSequencerCellParameterOverride (row, step, "Forward Curve", (float) model.scratchForwardCurveGlobalValue.load()));
        v.scratchBackwardCurveIndex = juce::roundToInt (model.getSequencerCellParameterOverride (row, step, "Backward Curve", (float) model.scratchBackwardCurveGlobalValue.load()));
    }
    else
    {
        v.stretchGrainSizeMs = model.stretchGrainSizeMsValue.load();
        v.stretchSpeedMultiplier = model.stretchSpeedMultiplierValue.load();
        v.filterSweepResonance = model.filterSweepResonanceValue.load();
        v.filterSweepType = model.filterSweepFilterTypeValue.load();
        v.curveShape = model.curveShapeValue.load();
        v.bitcrushRateValue = model.bitcrushRateReductionGlobalValue.load();
        v.bitcrushRateMode = model.bitcrushRateReductionModeGlobalValue.load();
        v.bitcrushBitDepthValue = model.bitcrushBitDepthGlobalValue.load();
        v.bitcrushBitDepthMode = model.bitcrushBitDepthModeGlobalValue.load();
        v.flangerDelayValue = model.flangerDelayTimeGlobalValue.load();
        v.flangerDelayMode = model.flangerDelayTimeModeGlobalValue.load();
        v.flangerMixValue = model.flangerMixGlobalValue.load();
        v.flangerMixMode = model.flangerMixModeGlobalValue.load();
        v.flangerFeedbackValue = model.flangerFeedbackGlobalValue.load();
        v.flangerFeedbackMode = model.flangerFeedbackModeGlobalValue.load();
        v.scratchRateIndex = model.scratchRateGlobalValue.load();
        v.scratchForwardCurveIndex = model.scratchForwardCurveGlobalValue.load();
        v.scratchBackwardCurveIndex = model.scratchBackwardCurveGlobalValue.load();
        // volumeValue/volumeMode keep their struct defaults (1.0 / Static) --
        // there's no global dial, exactly the fallback the Sequenced-mode
        // override lookup itself uses for an absent Volume override.
    }

    return v;
}

void SlicerEngine::applyPickState (const PickStyleValues& values,
                                   int sliceStartSample, int sliceEndSample,
                                   PlaybackStyle style, int uiSliceIndex,
                                   bool styleEndSampleFormula,
                                   double rate, double hostBpm, double hostSampleRate,
                                   double& naturalLengthHostSamplesOut,
                                   double& roundTripLengthHostSamplesOut)
{
    const bool pingPong = (style == PlaybackStyle::pingPong);
    const bool stretch = (style == PlaybackStyle::stretch);
    const bool scratch = (style == PlaybackStyle::scratch);

    currentPlaybackStyle = style;
    currentSliceStartSample = sliceStartSample;
    currentSliceLength = sliceEndSample - sliceStartSample;
    currentPosition = (double) sliceStartSample;

    // Stretch grain settings -- captured before currentEndSample below, since
    // Grain Speed feeds directly into that calculation.
    currentPickStretchGrainSizeMs = values.stretchGrainSizeMs;
    currentPickStretchSpeedMultiplier = values.stretchSpeedMultiplier;

    // Scratch cycle (v1) + curves (v2) -- also captured before
    // currentEndSample below, since the cycle length feeds directly into it.
    currentPickScratchCycleLengthHostSamples = scratch
        ? computeScratchCycleLengthHostSamples (values.scratchRateIndex, currentSliceLength,
                                                 hostBpm, hostSampleRate, rate)
        : 0.0;
    currentPickScratchForwardCurve = easingCurveFromIndex (values.scratchForwardCurveIndex);
    currentPickScratchBackwardCurve = easingCurveFromIndex (values.scratchBackwardCurveIndex);

    currentEndSample = styleEndSampleFormula
        ? (pingPong ? (2 * sliceEndSample - sliceStartSample)
           : stretch ? (int) (sliceStartSample + (double) currentPickStretchSpeedMultiplier * currentSliceLength)
           : scratch ? (int) (sliceStartSample + currentPickScratchCycleLengthHostSamples * rate)
                     : sliceEndSample)
        : sliceEndSample;

    hasCurrentPick = true;
    model.currentlyPlayingSliceIndexForUI.store (uiSliceIndex);

    currentPickBeatQuantized = false;
    currentPickNativeRateActive = false; // Performance mode's Sync-off override never applies outside Performance mode's own picks

    currentPickFilterSweepResonance = values.filterSweepResonance;
    currentPickFilterSweepType = values.filterSweepType;
    currentPickCurveShape = values.curveShape;

    currentPickBitcrushRateValue = values.bitcrushRateValue;
    currentPickBitcrushRateMode = values.bitcrushRateMode;
    currentPickBitcrushBitDepthValue = values.bitcrushBitDepthValue;
    currentPickBitcrushBitDepthMode = values.bitcrushBitDepthMode;

    currentPickFlangerDelayValue = values.flangerDelayValue;
    currentPickFlangerDelayMode = values.flangerDelayMode;
    currentPickFlangerMixValue = values.flangerMixValue;
    currentPickFlangerMixMode = values.flangerMixMode;
    currentPickFlangerFeedbackValue = values.flangerFeedbackValue;
    currentPickFlangerFeedbackMode = values.flangerFeedbackMode;

    currentPickVolumeValue = values.volumeValue;
    currentPickVolumeMode = values.volumeMode;

    samplesSincePickStart = 0.0;
    const double naturalLengthHostSamples = (rate > 0.0) ? ((double) currentSliceLength / rate) : 0.0;
    currentPickMidpointHostSamples = scratch
        ? (currentPickScratchCycleLengthHostSamples * 0.5)
        : naturalLengthHostSamples; // where a Ping-Pong round trip reverses; unused otherwise. Scratch (v1) uses half its OWN cycle length instead.

    naturalLengthHostSamplesOut = naturalLengthHostSamples;
    roundTripLengthHostSamplesOut = pingPong ? (2.0 * naturalLengthHostSamples)
                                    : stretch ? ((double) currentPickStretchSpeedMultiplier * naturalLengthHostSamples)
                                    : scratch ? currentPickScratchCycleLengthHostSamples
                                              : naturalLengthHostSamples;
}

//==============================================================================
// The real-time render call. The processor computes position/hasPlayHead/
// hostTransportPlaying/hostSampleRate ahead of the lock and hands them in;
// this body is byte-for-byte the old SlicerAudioProcessor::processBlock()
// DSP, including taking model.sampleLock itself.
//==============================================================================
void SlicerEngine::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages,
                                const juce::Optional<juce::AudioPlayHead::PositionInfo>& position,
                                bool hasPlayHead, bool hostTransportPlaying, double hostSampleRate)
{
    const juce::ScopedLock sl (model.sampleLock);

    // Computed ahead of the MIDI dispatch just below (rather than after, as
    // before Quantize Recall) purely so handlePerformanceStateNoteOn() can
    // know whether the host transport is playing AT NOTE-ON TIME, to decide
    // immediate-vs-deferred -- everything else that reads these three still
    // does so no earlier than it did before, so this reordering is a no-op
    // for every other mode.

    // Dispatch note-ons before discarding the buffer below -- see
    // handleIncomingMidi()/dispatchNoteOn() for the routing layer. Done
    // before the trigger-mode init section further down (which runs once
    // per block, later in this same call) so a MIDI-triggered pattern
    // recall's dimension/state changes are already in place when that
    // section runs, giving an instant switch within the block the note-on
    // arrived in.
    handleIncomingMidi (midiMessages, hostTransportPlaying);
    midiMessages.clear(); // not a MIDI effect -- never produce MIDI out

    if (! model.sampleLoaded)
        return;


    // Performance mode (Pass 1) needs to be known before the gates below --
    // same "runs independent of host transport" precedent as Audition just
    // underneath, just reached via a condition on those gates rather than a
    // structural early-return, since Performance mode shares the rest of
    // this function's per-sample render loop with every other mode
    // (Audition doesn't -- it renders through its own separate function and
    // returns immediately, so it never needs to reach the gates at all).
    const bool performanceMode = (model.triggerMode.load() == TriggerMode::performance);

    // Audition (Step 25): takes priority over everything below — a raw,
    // generative-engine-bypassing loop of [trimStart, trimEnd), independent
    // of host transport (it has to work even while the transport's
    // stopped, since setting up a trim happens before worrying about
    // sync). Auto-stops the instant the host transport starts playing, so
    // it and the real engine below never talk over each other.
    if (model.auditionActive.load())
    {
        if (hostTransportPlaying)
        {
            model.auditionActive.store (false);
            model.auditionPlaybackPositionForUI.store (-1); // Step 28 -- the playhead indicator must vanish the instant this auto-stop fires, same as the audio itself
        }
        else
        {
            if (hostSampleRate > 0.0)
                renderAudition (buffer, hostSampleRate);

            return;
        }
    }

    if (model.slices.empty())
        return;

    // Performance mode (Pass 1) is exempt from both gates below, same
    // "independent of host transport" contract Audition already gets above
    // -- recalling a state (or hearing the one currently being edited) has
    // to work whether or not the host's transport is running, unlike Slice
    // Length/Clock/Sequenced, which are all genuinely meaningless without
    // it (their triggers ARE the transport's beat/bar position).
    if (! hasPlayHead && ! performanceMode)
        return;

    if ((! position.hasValue() || ! hostTransportPlaying) && ! performanceMode)
    {
        hasCurrentPick = false; // transport stopped — fresh chain next time it starts
        clockModeInitialized = false;
        clockCurrentPickValid = false;
        resetWindowInitialized = false; // Step 34 -- re-snap to the current reset window next time transport starts
        model.currentlyPlayingSliceIndexForUI.store (-1);
        return;
    }

    // position may be absent, or reporting "not playing," here -- only
    // possible for a Performance mode block, since the gate above already
    // returned for every other mode in that case. Same 120bpm/0ppq fallback
    // this line already used for a host that simply doesn't report BPM
    // while playing, extended to cover "doesn't report position at all."
    const double hostBpm = (position.hasValue() && position->getBpm().hasValue()) ? *position->getBpm() : 120.0;

    if (hostBpm <= 0.0 || hostSampleRate <= 0.0)
        return;

    // Repitch factor: how much faster/slower to play the source sample so
    // its `model.loopLengthBars` bars match the host's tempo. >1 = source is
    // slower than host (speed up to fit, pitch rises); <1 = source is
    // faster than host (slow down, pitch drops). Applies in both trigger
    // modes — it's purely a playback-speed/pitch thing, independent of
    // when triggers happen.
    const double loopLengthQuarterNotes = (double) model.loopLengthBars.load() * 4.0; // assumes 4/4
    const double sourceSpanSeconds = model.computeSourceSpanSeconds();
    const double hostLoopLengthSeconds = loopLengthQuarterNotes * (60.0 / hostBpm);
    const double repitchRatio = (hostLoopLengthSeconds > 0.0)
                                     ? (sourceSpanSeconds / hostLoopLengthSeconds)
                                     : 1.0;

    const double playbackRate = (model.sampleSampleRate / hostSampleRate) * repitchRatio;

    const int sourceLength = model.sampleBuffer.getNumSamples();
    const int sourceChannels = model.sampleBuffer.getNumChannels();
    const int outChannels = buffer.getNumChannels();
    const int numSamples = buffer.getNumSamples();

    if (sourceLength == 0 || sourceChannels == 0)
        return;

    const double fadeInSamplesRequested = (double) model.fadeInMs.load() / 1000.0 * hostSampleRate;
    const double fadeOutSamplesRequested = (double) model.fadeOutMs.load() / 1000.0 * hostSampleRate;

    const bool clockMode = (model.triggerMode.load() == TriggerMode::clock);
    const bool sequencedMode = (model.triggerMode.load() == TriggerMode::sequenced);
    // performanceMode was already determined above, ahead of the transport gates it needs to bypass

    // Time-Stretch (Step 17): grain scheduling derived from the same
    // playbackRate math above — a grain spawns every outputHopSamples
    // (host domain, half the grain length for the fixed 50% overlap),
    // sourceHopSamples further into the source than the last one, which
    // works out to exactly the same average source-consumption rate as
    // playbackRate. Each grain itself only gets srConversionRatio applied
    // (sample-rate matching, not a pitch effect) — never repitchRatio —
    // which is what keeps pitch fixed regardless of tempo.
    const bool timeStretchMode = (model.pitchMode.load() == PitchMode::timeStretch);
    const double srConversionRatio = model.sampleSampleRate / hostSampleRate;
    const double grainSizeHostSamples = (double) model.grainSizeMs.load() / 1000.0 * hostSampleRate;
    const double outputHopSamples = grainSizeHostSamples * 0.5; // fixed 50% overlap, not exposed
    const double sourceHopSamples = outputHopSamples * srConversionRatio * repitchRatio;
    const GranularStretcher::WindowShape grainWindowShapeForBlock =
        (model.grainWindowShape.load() == GrainWindowShape::hann) ? GranularStretcher::WindowShape::hann
                                                              : GranularStretcher::WindowShape::triangular;

    // Pitch control (Step 18): scales only each grain's own internal
    // read-rate (below, in renderAndAdvance) — never outputHopSamples or
    // sourceHopSamples above, which is what keeps stretch amount and
    // pitch independently controllable. 0 semitones -> pitchRatio == 1.0,
    // a complete no-op, same as before this existed.
    const double pitchRatio = std::pow (2.0, (double) model.pitchShiftSemitones.load() / 12.0);

    if (granularNeedsReseed.exchange (false))
        granularStretcher.reset (currentPosition); // pitch mode changed mid-pick — reseed from wherever we are now

    // Both modes need the host's beat position now (Step 34): Clock mode
    // for its own ticks/windows, and Slice Length mode for its lightweight
    // periodic-reset boundary tracking below -- previously Slice Length
    // mode paced itself purely from slice content length and never looked
    // at ppq at all.
    const double ppqStart = (position.hasValue() && position->getPpqPosition().hasValue()) ? *position->getPpqPosition() : 0.0;
    const double ppqPerSample = (hostBpm / 60.0) / hostSampleRate;

    if (clockMode)
    {
        if (! clockModeInitialized)
        {
            // Just entered Clock mode, or transport just started — snap to
            // the window we're currently inside and force an immediate
            // pick on the very first sample below.
            const double windowBeats = SlicerModel::getNoteValueBeats (model.clockReferenceIndex.load());
            const juce::int64 windowIndex = (juce::int64) std::floor (ppqStart / windowBeats);
            windowEndPpq = (double) (windowIndex + 1) * windowBeats;
            nextTickPpq = ppqStart;
            clockModeInitialized = true;
        }

        resetWindowInitialized = false; // the other modes' own init state is meaningless here; re-entering any of them later starts fresh
        sequencedModeInitialized = false;
        performanceModeInitialized = false;
    }
    else if (sequencedMode)
    {
        clockModeInitialized = false;
        resetWindowInitialized = false;
        performanceModeInitialized = false;

        // Sequenced Trigger Mode (Step 37) — same "just entered / transport
        // just started" treatment Clock mode gives itself above: force the
        // very first per-sample check below to treat whichever step we're
        // currently inside as new, so it triggers immediately rather than
        // waiting for the next step boundary.
        if (! sequencedModeInitialized)
        {
            sequencedLastStepIndex = -1;
            sequencedModeInitialized = true;
            sequencedSubdivisionActive = false; // Step 47 -- no stale retrigger schedule carried in from before this mode was (re-)entered
        }

        // Defensive: every dimension-changing setter already calls
        // model.resetSequencerGrid() under this same lock, so this should never
        // actually be needed, but self-heal rather than risk an
        // out-of-bounds read below if that invariant is ever violated.
        if ((int) model.sequencerGrid.size() != model.getSequencerNumRows() * model.getSequencerNumSteps())
            model.resetSequencerGrid();
    }
    else if (performanceMode)
    {
        clockModeInitialized = false;
        resetWindowInitialized = false;
        sequencedModeInitialized = false;

        // Just entered Performance mode, or transport just started --
        // unlike every other mode's own init above, this does NOT force an
        // immediate fresh pick: Performance mode has nothing to play until
        // a note-on recalls a state (performanceRecallPending, consumed in
        // the per-sample dispatch below), so it starts and stays silent.
        if (! performanceModeInitialized)
        {
            performanceModeInitialized = true;
            hasCurrentPick = false;
        }
    }
    else
    {
        clockModeInitialized = false; // so re-entering Clock mode later starts fresh
        sequencedModeInitialized = false;
        performanceModeInitialized = false; // so re-entering Performance mode later starts fresh (silent) again

        // Slice Length periodic reset (Step 34) — same "just entered /
        // transport just started, snap to the current window and force an
        // immediate pick" treatment Clock mode gives itself just above.
        if (! resetWindowInitialized)
        {
            const double resetWindowBeats = (double) SlicerModel::getResetBarsValue (model.resetBarsIndex.load()) * 4.0; // 4/4
            const juce::int64 windowIndex = (juce::int64) std::floor (ppqStart / resetWindowBeats);
            resetWindowEndPpq = (double) (windowIndex + 1) * resetWindowBeats;
            resetWindowInitialized = true;
            hasCurrentPick = false; // force a fresh pick aligned to this window right away
        }
    }

    for (int i = 0; i < numSamples; ++i)
    {
        // Set true whenever a fresh pick begins this sample (new slice
        // chosen, or a Clock-mode retrigger) — the cue to reseed the
        // granular engine so it starts a new grain right at the pick's
        // start rather than mid-grain from whatever the last pick left it
        // doing. Reset unconditionally, regardless of pitch mode, so it's
        // already in sync if the user switches mode later mid-stream.
        bool pickJustStarted = false;

        // Needed by both modes now (Step 34): Clock mode's own tick/window
        // checks below, and Slice Length mode's reset-boundary check
        // further down — computed once per sample, not once per block, on
        // purpose (see the reset-boundary comment below for why).
        const double samplePpq = ppqStart + (double) i * ppqPerSample;

        if (clockMode)
        {
            if (samplePpq >= nextTickPpq)
            {
                const bool newWindow = ! clockCurrentPickValid || samplePpq >= windowEndPpq;

                if (newWindow)
                {
                    clockCurrentSliceIndex = pickWeightedRandomSlice();
                    clockCurrentSubdivisionIndex = pickWeightedIndex (model.subdivisionProbabilities);
                    clockCurrentPlaybackStyle = indexToPlaybackStyle (pickWeightedIndex (model.playbackStyleProbabilities));
                    clockCurrentPickValid = true;

                    const double windowBeats = SlicerModel::getNoteValueBeats (model.clockReferenceIndex.load());
                    const juce::int64 windowIndex = (juce::int64) std::floor (samplePpq / windowBeats);
                    windowEndPpq = (double) (windowIndex + 1) * windowBeats;

                    // Filter Sweep's Whole Window scope (Step 30) -- reset
                    // ONLY here, on a genuine new-window event, never on an
                    // ordinary per-tick retrigger below, so the sweep stays
                    // continuous across every tick inside this window.
                    samplesSinceWindowStart = 0.0;
                    currentWindowLengthHostSamples = windowBeats * (60.0 / hostBpm) * hostSampleRate;
                }

                // Retrigger (or first-trigger) this window's slice from its
                // start — unconditionally, even if it hadn't naturally
                // finished yet. That's the whole point of Clock mode. Every
                // tick restarts the round trip from the beginning (forward
                // leg) even within the same window, same as Forward always
                // restarted from the slice's start on every tick.
                if (clockCurrentSliceIndex >= 0 && clockCurrentSliceIndex < (int) model.slices.size())
                {
                    const auto& slice = model.slices[(size_t) clockCurrentSliceIndex];
                    const bool tapeStop = (clockCurrentPlaybackStyle == PlaybackStyle::tapeStop);
                    const bool stretch = (clockCurrentPlaybackStyle == PlaybackStyle::stretch);

                    // Shared pick-start state (1.2): Clock mode always uses
                    // the global values; per-step overrides are Sequenced-
                    // mode-only. The shared helper also sets the style's
                    // own currentEndSample, midpoint, and natural/round-trip
                    // lengths.
                    double naturalLengthHostSamples = 0.0;
                    double roundTripLengthHostSamples = 0.0;
                    applyPickState (
                        capturePickStyleValues (PickValueSource::global, 0, 0),
                        slice.startSample, slice.endSample,
                        clockCurrentPlaybackStyle, clockCurrentSliceIndex, true,
                        playbackRate, hostBpm, hostSampleRate,
                        naturalLengthHostSamples, roundTripLengthHostSamples);

                    pickJustStarted = true;

                    const double tickBeats = SlicerModel::getNoteValueBeats (clockCurrentSubdivisionIndex);
                    const double tickLengthHostSamples = tickBeats * (60.0 / hostBpm) * hostSampleRate;

                    // Shared by Tape Stop's whole-window scope and Stretch
                    // (which always behaves like whole-window) below.
                    const double windowBeatsForDuration = SlicerModel::getNoteValueBeats (model.clockReferenceIndex.load());
                    const double windowLengthHostSamples = windowBeatsForDuration * (60.0 / hostBpm) * hostSampleRate;

                    if (tapeStop)
                    {
                        // Tape Stop's duration is entirely scope-driven —
                        // NOT capped by the slice's own natural length like
                        // Forward/Ping-Pong deliberately are just below,
                        // since the whole point is that read position may
                        // never reach the slice's actual end before the
                        // rate hits zero.
                        const bool wholeWindow = (model.tapeStopScope.load() == TapeStopScope::wholeWindow);
                        currentPickTapeStopDurationHostSamples = wholeWindow ? windowLengthHostSamples : tickLengthHostSamples;
                        currentPickLengthInHostSamples = currentPickTapeStopDurationHostSamples; // only used for fadeIn clamping below
                    }
                    else if (stretch)
                    {
                        // Stretch always overrides the whole window (no
                        // per-tick option) -- capped by whichever comes
                        // first: the full currentPickStretchSpeedMultiplier-x
                        // natural length, or the window's own boundary
                        // (mirrors Forward/Ping-Pong's own "whichever comes
                        // first" clamp against a tick, just against the
                        // window instead, since there's no tick to speak of
                        // here).
                        currentPickLengthInHostSamples = juce::jmin ((double) currentPickStretchSpeedMultiplier * naturalLengthHostSamples,
                                                                      windowLengthHostSamples);
                    }
                    else
                    {
                        // The fade-out needs to anticipate whichever comes
                        // first — the slice's own natural (round-trip, for
                        // Ping-Pong) end, or the forced retrigger at the next
                        // tick — otherwise a slice that gets cut short by the
                        // clock never gets a fade-out at all, and every
                        // retrigger clicks.
                        currentPickLengthInHostSamples = juce::jmin (roundTripLengthHostSamples, tickLengthHostSamples);
                    }
                }
                else
                {
                    hasCurrentPick = false;
                }

                if ((clockCurrentPlaybackStyle == PlaybackStyle::tapeStop
                     && model.tapeStopScope.load() == TapeStopScope::wholeWindow)
                    || clockCurrentPlaybackStyle == PlaybackStyle::stretch)
                {
                    // Whole-window Tape Stop, and Stretch (which always
                    // behaves this way, no per-tick option), override
                    // normal subdivision retriggering -- one continuous
                    // render spans the entire window, so there's nothing
                    // for a tick to do until the window itself changes.
                    // Jumping straight to the window's end means the next
                    // event that fires IS that boundary, which is
                    // naturally a fresh newWindow pick — no separate
                    // retrigger-skipping logic needed above.
                    nextTickPpq = windowEndPpq;
                }
                else
                {
                    const double subdivisionBeats = SlicerModel::getNoteValueBeats (clockCurrentSubdivisionIndex);
                    nextTickPpq += juce::jmax (subdivisionBeats, 1.0e-6); // guard against a zero-length tick
                    nextTickPpq = juce::jmin (nextTickPpq, windowEndPpq);
                }
            }
        }
        else if (sequencedMode)
        {
            // Sequenced Trigger Mode (Step 37): checked every SAMPLE, not
            // once per block -- same per-sample discipline (and same
            // reason) as Clock mode's own tick/window checks and the
            // mandatory Reset feature's boundary check, avoiding the exact
            // bug Step 6 introduced and fixed (a boundary computed once per
            // block from the block's start position silently misses
            // boundaries landing mid-block).
            double stepBeats = SlicerModel::getNoteValueBeats (model.stepResolutionIndex.load());
            int totalSteps = model.getSequencerNumSteps();

            if (stepBeats > 0.0 && totalSteps > 0)
            {
                juce::int64 absoluteStepIndex = (juce::int64) std::floor (samplePpq / stepBeats);
                int currentStepIndex = (int) (((absoluteStepIndex % totalSteps) + totalSteps) % totalSteps);

                // Pattern Switch Timing (Pass 2): Set Interval/End of
                // Pattern both defer an already-armed recall
                // (pendingPatternSwitchNote) to a boundary, checked every
                // SAMPLE right here alongside the step-boundary check just
                // below it -- same per-sample discipline as everything else
                // in this function, avoiding the Step 6 bug (a boundary
                // computed once per block silently missing ones that land
                // mid-block). Deliberately evaluated against THIS pattern's
                // stepBeats/totalSteps (captured above, before any swap
                // below) -- "reaches the end of its OWN Pattern Length"
                // means the currently playing pattern's own length, not the
                // incoming one's.
                const int pendingSwitchNote = pendingPatternSwitchNote.load();

                if (pendingSwitchNote >= 0)
                {
                    bool boundaryCrossed = false;

                    if (model.patternSwitchTiming.load() == PatternSwitchTiming::setInterval)
                    {
                        if (! patternSwitchIntervalBoundaryArmed)
                        {
                            // Just (re-)armed -- snap to the next grid point
                            // from wherever ppq is right now, same
                            // "windowIndex + 1" shape Clock mode/Reset use
                            // to find their own next boundary.
                            const double intervalBeats = juce::jmax (
                                SlicerModel::getNoteValueBeats (model.patternSwitchIntervalIndex.load()), 1.0e-6);
                            const juce::int64 intervalIndex = (juce::int64) std::floor (samplePpq / intervalBeats);
                            patternSwitchIntervalBoundaryPpq = (double) (intervalIndex + 1) * intervalBeats;
                            patternSwitchIntervalBoundaryArmed = true;
                        }

                        boundaryCrossed = (samplePpq >= patternSwitchIntervalBoundaryPpq);
                    }
                    else if (model.patternSwitchTiming.load() == PatternSwitchTiming::endOfPattern)
                    {
                        // The pattern's own existing loop-boundary logic --
                        // the same absoluteStepIndex/totalSteps wrap the
                        // step-trigger check just below already relies on
                        // -- not a separately calculated boundary. A
                        // genuine wrap (step totalSteps-1 -> step 0), not
                        // just "step index happens to be 0" (which is also
                        // true on the very first step ever, before
                        // sequencedLastStepIndex has taken any real value).
                        boundaryCrossed = (currentStepIndex == 0 && sequencedLastStepIndex == totalSteps - 1);
                    }

                    if (boundaryCrossed)
                    {
                        handleSequencerPatternRecallNoteOn (pendingSwitchNote);
                        pendingPatternSwitchNote.store (-1);
                        patternSwitchIntervalBoundaryArmed = false;

                        // handleSequencerPatternRecallNoteOn() just forced
                        // sequencedModeInitialized false expecting the
                        // usual once-per-block re-sync (see its own doc
                        // comment) -- but that block-level check already
                        // ran for THIS block, before this per-sample loop
                        // started, so left alone that flag would instead
                        // fire a redundant second re-sync at the START of
                        // NEXT block, re-triggering the same step again.
                        // Set it back to true here since this sample's own
                        // step alignment is being re-derived immediately
                        // below against the just-recalled pattern instead.
                        sequencedModeInitialized = true;

                        // Re-derive against the just-recalled pattern's own
                        // resolution/length so the step lookup below reads
                        // the NEW grid at the correct width, not the old
                        // pattern's.
                        stepBeats = SlicerModel::getNoteValueBeats (model.stepResolutionIndex.load());
                        totalSteps = model.getSequencerNumSteps();

                        if (stepBeats > 0.0 && totalSteps > 0)
                        {
                            absoluteStepIndex = (juce::int64) std::floor (samplePpq / stepBeats);
                            currentStepIndex = (int) (((absoluteStepIndex % totalSteps) + totalSteps) % totalSteps);
                        }
                    }
                }

                if (stepBeats > 0.0 && totalSteps > 0 && currentStepIndex != sequencedLastStepIndex)
                {
                    sequencedLastStepIndex = currentStepIndex;
                    model.currentlyPlayingStepIndexForUI.store (currentStepIndex);

                    const int numRows = model.getSequencerNumRows();
                    int activeRow = -1;
                    int activeStyle = -1;

                    for (int row = 0; row < numRows; ++row)
                    {
                        const int style = model.sequencerGrid[(size_t) (row * totalSteps + currentStepIndex)];

                        if (style >= 0)
                        {
                            activeRow = row;
                            activeStyle = style;
                            break;
                        }
                    }

                    // Structural monophony (Step 37) guarantees at most one
                    // active row per column -- if none is active here,
                    // there's nothing new to trigger; whatever's currently
                    // playing (or silence) just continues per its own
                    // existing completion logic below, same as Clock mode
                    // already does between ticks.
                    if (activeRow >= 0 && activeRow < (int) model.slices.size())
                    {
                        // Reuse the exact same single-voice render path
                        // every other mode already uses -- force a fresh
                        // start regardless of what's currently playing,
                        // same mechanic already proven in Clock mode's
                        // tick-retriggering and the mandatory Reset
                        // feature. Each note plays as whichever
                        // PlaybackStyle its own cell stores (Step 41) --
                        // selected directly, not via a weighted draw, since
                        // the whole point of this mode is that everything
                        // is explicitly placed by the user.
                        const auto& slice = model.slices[(size_t) activeRow];
                        const PlaybackStyle style = indexToPlaybackStyle (activeStyle);
                        const bool tapeStop = (style == PlaybackStyle::tapeStop);

                        // Shared pick-start state (1.2): this step's own
                        // overrides where it has them, else the global
                        // values -- the same per-step-override-else-global
                        // lookup every parameter below used to run
                        // individually (looked up unconditionally, harmless
                        // for styles that don't use a given parameter, since
                        // an override is only ever populated for cells whose
                        // style actually offers it). The shared helper also
                        // sets the style's own currentEndSample -- Sequenced
                        // mode deliberately keeps the plain slice end (Ping-
                        // Pong's half-content window and Stretch's duration-
                        // based gate make the doubled/extended endSample
                        // unnecessary here, same reasoning as the original
                        // code) -- plus the midpoint and the natural/round-
                        // trip lengths.
                        double roundTripLengthHostSamples = 0.0;
                        double naturalLengthHostSamples = 0.0;
                        applyPickState (
                            capturePickStyleValues (PickValueSource::stepOverride, activeRow, currentStepIndex),
                            slice.startSample, slice.endSample,
                            style, activeRow, false,
                            playbackRate, hostBpm, hostSampleRate,
                            naturalLengthHostSamples, roundTripLengthHostSamples);

                        pickJustStarted = true;

                        // Anticipatory fade (Step 37): cap this note's
                        // length to whichever comes first -- its own
                        // natural length, or the NEXT scheduled active step
                        // anywhere in the grid (structural monophony means
                        // that step, whenever it comes, WILL cut this note
                        // off) -- same established anticipatory-fade
                        // pattern already used for Clock's ticks, Tape
                        // Stop, Filter Sweep, and the mandatory Reset
                        // feature. A bounded forward scan (at most
                        // totalSteps * numRows checks), run once per NOTE
                        // START, not per sample.
                        int stepsUntilNextActive = totalSteps;

                        for (int offset = 1; offset <= totalSteps; ++offset)
                        {
                            const int checkColumn = (currentStepIndex + offset) % totalSteps;
                            bool columnHasActive = false;

                            for (int row2 = 0; row2 < numRows; ++row2)
                            {
                                if (model.sequencerGrid[(size_t) (row2 * totalSteps + checkColumn)] >= 0)
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
                            (double) stepsUntilNextActive * stepBeats * (60.0 / hostBpm) * hostSampleRate;

                        // Style-dependent duration (Step 41/43) -- mirrors
                        // Clock mode's own per-tick calc exactly (this
                        // mode's "next active step" plays the same role as
                        // Clock's tick/window boundary), since Sequenced
                        // mode has no "Whole Window" equivalent -- Tape
                        // Stop and Filter Sweep always behave as Per Tick
                        // here, which needs no extra code: Filter Sweep's
                        // own Whole Window check is already gated to
                        // clockMode elsewhere, and Tape Stop's duration
                        // below is driven entirely by the schedule, same as
                        // Clock's own Per Tick branch. Ping-Pong (Step 44)
                        // needs no special case here at all -- its
                        // half-content window (see the shared render code
                        // below) means its total duration is just a normal
                        // single pick's length, same as Forward/Filter
                        // Sweep share in the universal clamp below.
                        //
                        // Step-extension fix: every style's duration is
                        // capped by THIS step's own declared length --
                        // natural (Step-resolution-quantized) slice length,
                        // or its Step-extension override if Shift+drag set
                        // one -- the identical value SequencerGrid's
                        // piano-roll bar renders (see
                        // getSequencerCellDeclaredLengthSteps()),
                        // converted from steps to host samples via the
                        // same stepBeats/hostBpm/hostSampleRate used
                        // for samplesUntilNextActiveStep above. Still
                        // clamped against samplesUntilNextActiveStep,
                        // same "whichever comes first" precedence the
                        // bar's own computeBarLengthInSteps() already
                        // applies (its next-active-step clamp, run
                        // AFTER settling on desiredSteps) -- a step's
                        // own length is the ceiling, but a genuinely
                        // upcoming step still cuts it off early, same
                        // as what's actually drawn.
                        const int declaredLengthSteps = model.getSequencerCellDeclaredLengthSteps (activeRow, currentStepIndex);
                        const double declaredLengthHostSamples =
                            (double) declaredLengthSteps * stepBeats * (60.0 / hostBpm) * hostSampleRate;

                        if (tapeStop)
                        {
                            // Tape Stop's decel is capped by the declared
                            // length, and its duration IS that cap (see the
                            // fadeIn clamping below) -- unlike every other
                            // style, Tape Stop's read position may never
                            // reach the slice's actual end.
                            currentPickTapeStopDurationHostSamples = juce::jmin (declaredLengthHostSamples, samplesUntilNextActiveStep);
                            currentPickLengthInHostSamples = currentPickTapeStopDurationHostSamples; // only used for fadeIn clamping
                        }
                        else
                        {
                            // Stretch/Forward/Ping-Pong/Filter Down/Up share
                            // one duration path. Stretch: Grain Speed
                            // (currentPickStretchSpeedMultiplier) plays NO
                            // part in this -- it stays a fixed character
                            // constant (how fast ONE pass sweeps the
                            // slice), untouched by duration; if the
                            // declared length outlasts one pass, the SAME
                            // pass repeats to fill the remainder instead
                            // (see grainPlaybackStyle's loop mode in the
                            // granular render section below). Forward/
                            // Ping-Pong/Filter Down/Up: when the declared
                            // length exceeds the natural unit -- one raw
                            // slice playthrough for Forward and Filter
                            // Down/Up, one round trip for Ping-Pong (its
                            // half-content window, see
                            // pingPongFoldLengthSamples below, makes one
                            // round trip exactly naturalLengthHostSamples
                            // long here) -- that unit LOOPS to fill the
                            // remainder, the same "repeat the natural unit"
                            // principle just described for Stretch. No
                            // extra code needed for the loop itself:
                            // grainPlaybackStyle (loop for Forward/Filter
                            // Down/Up, the existing pingPong fold for
                            // Ping-Pong -- see below) already wraps
                            // unconditionally, a no-op whenever this
                            // duration never actually exceeds one natural
                            // unit in the first place. Filter Down/Up's
                            // sweep timeline (currentWindowLengthHostSamples,
                            // set below from this same value, before
                            // Subdivide splits it into individual ticks)
                            // rides along for free too -- one continuous
                            // glide across the whole declared length
                            // regardless of how many times the underlying
                            // audio loops underneath it, or how many
                            // Subdivide retriggers happen, exactly the same
                            // Whole Window principle Clock mode's own scope
                            // setting already proved.
                            currentPickTapeStopDurationHostSamples = naturalLengthHostSamples; // unused, harmless
                            currentPickLengthInHostSamples = juce::jmin (declaredLengthHostSamples, samplesUntilNextActiveStep);
                        }

                        // Subdivide (Step 47): per-step retrigger rate,
                        // general -- offered (and read) regardless of this
                        // step's own PlaybackStyle, unlike Resonance/Filter
                        // Type/Curve Shape/Grain Size/Grain Speed above.
                        // 0 (Off, the default/fallback) leaves everything
                        // exactly as it was above this point.
                        //
                        // "Whole Window" reuse for Filter Down/Up (Step 30's
                        // Filter Sweep scope, generalized here): this step's
                        // OWN total duration -- exactly what was just
                        // computed above, before Subdivide splits it into
                        // individual retrigger ticks below -- becomes the
                        // window samplesSinceWindowStart/currentWindowLength-
                        // HostSamples track, the same pair Clock mode's own
                        // Whole Window scope already uses (see useWholeWindow
                        // further down). Set unconditionally (not just when
                        // Subdivide is on): when it's off there's only ever
                        // one trigger occupying the whole step anyway, so
                        // Whole Window and Per Tick are numerically identical
                        // in that case -- no behaviour change for existing,
                        // non-subdivided steps.
                        const double stepWindowLengthHostSamples =
                            tapeStop ? currentPickTapeStopDurationHostSamples : currentPickLengthInHostSamples;
                        samplesSinceWindowStart = 0.0;
                        currentWindowLengthHostSamples = stepWindowLengthHostSamples;

                        const int subdivideOption = juce::roundToInt (model.getSequencerCellParameterOverride (
                            activeRow, currentStepIndex, "Subdivide", 0.0f));
                        sequencedSubdivisionActive = subdivideOption > 0;
                        sequencedSubdivisionRow = activeRow;

                        if (sequencedSubdivisionActive)
                        {
                            const double subdivisionBeats = SlicerModel::getNoteValueBeats (subdivideOption - 1);
                            sequencedSubdivisionTickLengthHostSamples =
                                juce::jmax (1.0, subdivisionBeats * (60.0 / hostBpm) * hostSampleRate);
                            sequencedNextSubdivisionOffsetHostSamples = sequencedSubdivisionTickLengthHostSamples;

                            // This step's own first trigger (just started
                            // above) IS the first subdivision slot -- cap its
                            // length/duration to one tick, same jmin pattern
                            // Clock mode's own ticks already use, so its
                            // fade-out completes before the next retrigger
                            // rather than spilling into it.
                            currentPickLengthInHostSamples =
                                juce::jmin (currentPickLengthInHostSamples, sequencedSubdivisionTickLengthHostSamples);

                            if (tapeStop)
                                currentPickTapeStopDurationHostSamples =
                                    juce::jmin (currentPickTapeStopDurationHostSamples, sequencedSubdivisionTickLengthHostSamples);
                        }
                    }
                }

                // Subdivide retrigger (Step 47): runs every SAMPLE,
                // independent of the step-boundary check above -- same
                // per-sample discipline as everything else here, so a
                // retrigger point landing mid-block is never missed.
                // samplesSinceWindowStart/currentWindowLengthHostSamples are
                // the exact same pair set at this note's own pick-start
                // above (and, in Clock mode, already drive the Whole Window
                // Filter Sweep) -- reusing them here for scheduling too
                // keeps a subdivided step's retrigger grid and its Filter
                // Down/Up sweep locked to the same clock, and needs no
                // separate ppq-based scheduler.
                if (sequencedSubdivisionActive
                    && sequencedSubdivisionRow >= 0 && sequencedSubdivisionRow < (int) model.slices.size()
                    && samplesSinceWindowStart < currentWindowLengthHostSamples
                    && samplesSinceWindowStart >= sequencedNextSubdivisionOffsetHostSamples)
                {
                    const auto& subdivisionSlice = model.slices[(size_t) sequencedSubdivisionRow];
                    currentPosition = (double) subdivisionSlice.startSample;
                    samplesSincePickStart = 0.0;
                    hasCurrentPick = true;
                    pickJustStarted = true;

                    const double remainingWindowHostSamples =
                        juce::jmax (0.0, currentWindowLengthHostSamples - samplesSinceWindowStart);
                    currentPickLengthInHostSamples =
                        juce::jmin (sequencedSubdivisionTickLengthHostSamples, remainingWindowHostSamples);

                    if (currentPlaybackStyle == PlaybackStyle::tapeStop)
                        currentPickTapeStopDurationHostSamples = currentPickLengthInHostSamples;

                    sequencedNextSubdivisionOffsetHostSamples += sequencedSubdivisionTickLengthHostSamples;
                }
            }
        }
        else if (performanceMode)
        {
            // Quantize Recall: mirrors Pattern Switch Timing's Set Interval
            // scheme above (see the sequencedMode branch's own boundary
            // check), reusing the same per-sample host-ppq
            // boundary-crossing detection -- checked every SAMPLE, not once
            // per block, the same discipline that avoids the Step 6 bug (a
            // boundary computed once per block silently missing one that
            // lands mid-block). A pending recall (pendingPerformanceRecallNote,
            // armed by handlePerformanceStateNoteOn() while Quantize Recall
            // is on and the transport was playing at note-on time) is
            // applied the instant its chosen grid point is reached; a newer
            // note-on before that point just overwrites the same atomic
            // there, so the newest press always wins.
            //
            // Falls back to applying immediately, right here, if the host
            // transport isn't playing -- there's no meaningful beat position
            // to quantize against without it, and this also rescues a
            // recall that was armed while playing but whose target transport
            // then stopped before the boundary was ever reached (ppq holds
            // still while stopped, so an unplayed boundary would otherwise
            // never arrive) -- both preserve the "auditionable without
            // pressing play" behaviour Performance mode already has.
            const int pendingRecallNote = pendingPerformanceRecallNote.load();

            if (pendingRecallNote >= 0)
            {
                bool boundaryCrossed = ! hostTransportPlaying;

                if (hostTransportPlaying)
                {
                    if (! performanceQuantizeRecallBoundaryArmed)
                    {
                        // Just (re-)armed -- snap to the next grid point
                        // from wherever ppq is right now, same
                        // "windowIndex + 1" shape Clock mode/Reset/Set
                        // Interval all use to find their own next boundary.
                        const double intervalBeats = juce::jmax (
                            SlicerModel::getNoteValueBeats (model.performanceQuantizeRecallIntervalIndex.load()), 1.0e-6);
                        const juce::int64 intervalIndex = (juce::int64) std::floor (samplePpq / intervalBeats);
                        performanceQuantizeRecallBoundaryPpq = (double) (intervalIndex + 1) * intervalBeats;
                        performanceQuantizeRecallBoundaryArmed = true;
                    }

                    boundaryCrossed = (samplePpq >= performanceQuantizeRecallBoundaryPpq);
                }

                if (boundaryCrossed)
                {
                    applyPerformanceStateRecall (pendingRecallNote);
                    pendingPerformanceRecallNote.store (-1);
                    performanceQuantizeRecallBoundaryArmed = false;
                }
            }

            // Performance mode: no probability engine, no model.slices -- one
            // hand-trimmed segment, one style, that style's own independent
            // parameter values, driven purely by MIDI note-on
            // (performanceRecallPending, set by handlePerformanceStateNoteOn()/
            // applyPerformanceStateRecall() in dispatchNoteOn()'s routing, or
            // by the Quantize Recall boundary check just above) rather than
            // by anything time/position-based. Physical MIDI is playback-only
            // (the on-screen keyboard is the only thing that ever changes
            // focus -- see setFocusedPerformanceStateSlot()): pressing the
            // FOCUSED slot's key plays model.performanceWorkingState itself (live
            // in-progress edits, via the shared trim atomics every other
            // mode also edits through); pressing any OTHER slot's key plays
            // a frozen copy of ITS saved snapshot
            // (currentlyPlayingPerformanceSnapshot, including its own saved
            // trim), captured at note-on time so auditioning it never
            // disturbs model.performanceWorkingState or focus. Starts and stays
            // silent (hasCurrentPick == false) until the first note-on this
            // session -- see the performanceMode block-level init above,
            // which is the one place that sets it false; nothing here does.
            bool needsFreshPick = performanceRecallPending;
            performanceRecallPending = false;

            // Whichever source governs the CURRENTLY sounding (or about to
            // start) pick -- resolved once here so every reference below,
            // whether checking for completion or starting a fresh pick,
            // agrees on the same one. Follows performancePlaybackIsFocused,
            // set once at the note-on that actually started this pick (see
            // handlePerformanceStateNoteOn()), not re-derived from whatever
            // has focus right now -- so a focus change made from the UI
            // thread while a non-focused slot's pick is still sounding can
            // never retroactively swap what that pick is playing out from
            // under it.
            const PerformanceStateSnapshot& performanceActiveState = performancePlaybackIsFocused
                ? model.performanceWorkingState : currentlyPlayingPerformanceSnapshot;

            if (! needsFreshPick && hasCurrentPick)
            {
                // Same per-style "has this pick run its course" condition
                // Slice Length's own while-loop uses just below -- re-derived
                // here, same convention this function already follows per
                // mode, rather than shared, since each mode's surrounding
                // context (loop vs. one-shot, retry-on-empty-slice vs. not)
                // differs enough that sharing it would need its own
                // indirection to paper over those differences.
                const bool pickFinished = (currentPlaybackStyle == PlaybackStyle::tapeStop)
                    ? (samplesSincePickStart >= currentPickTapeStopDurationHostSamples)
                    : (currentPosition >= (double) (((currentPlaybackStyle == PlaybackStyle::pingPong
                                                           || currentPlaybackStyle == PlaybackStyle::stretch
                                                           || currentPlaybackStyle == PlaybackStyle::scratch)
                                                          ? currentEndSample
                                                          : juce::jmin (currentEndSample, sourceLength)) - 1));

                if (pickFinished)
                {
                    if (performanceActiveState.loop)
                        needsFreshPick = true; // Loop on: rechain the SAME segment+style, Slice Length's own "finishing is the cue" mechanic
                    else
                        hasCurrentPick = false; // Loop off: go silent and stay silent until the next note-on retriggers it
                }
            }

            if (needsFreshPick && performanceActiveState.populated)
            {
                currentPlaybackStyle = indexToPlaybackStyle (performanceActiveState.style);
                const bool pingPong = (currentPlaybackStyle == PlaybackStyle::pingPong);
                const bool stretch = (currentPlaybackStyle == PlaybackStyle::stretch);
                const bool scratch = (currentPlaybackStyle == PlaybackStyle::scratch);

                // Sync toggle: on follows whichever global Pitch Mode is
                // currently selected (playbackRate, unchanged); off forces
                // native/unsynced playback for this pick. Captured once
                // here, consulted uniformly at every downstream render site
                // via currentPickNativeRateActive/effectivePickPlaybackRate,
                // rather than re-checked inline at each one.
                currentPickNativeRateActive = ! performanceActiveState.sync;
                const double perfPlaybackRate = currentPickNativeRateActive
                    ? (model.sampleSampleRate / hostSampleRate) : playbackRate;

                // Own independent parameter storage -- indexed exactly as
                // getSequencerCellParameterName() documents, NEVER the
                // global default atomics Slice Length/Clock read (those
                // three lines above) or Sequenced mode's per-cell overrides.
                currentPickStretchGrainSizeMs = performanceActiveState.parameterValues[3];
                currentPickStretchSpeedMultiplier = performanceActiveState.parameterValues[4];

                // The focused slot's segment IS the shared trim atomics
                // (edited live via the same waveform trim handles every
                // other mode uses); any OTHER slot's segment is its own
                // saved trim, captured independently in its snapshot --
                // never the shared atomics, which stay owned by whichever
                // slot actually has focus.
                const int performanceSegmentStart = performancePlaybackIsFocused
                    ? model.trimStartSample.load() : performanceActiveState.trimStartSample;
                const int performanceSegmentEnd = performancePlaybackIsFocused
                    ? model.trimEndSample.load() : performanceActiveState.trimEndSample;

                currentSliceStartSample = performanceSegmentStart;
                currentSliceLength = performanceSegmentEnd - performanceSegmentStart;
                currentPosition = (double) currentSliceStartSample;

                currentPickScratchCycleLengthHostSamples = scratch
                    ? computeScratchCycleLengthHostSamples (juce::roundToInt (performanceActiveState.parameterValues[10]),
                                                             currentSliceLength, hostBpm, hostSampleRate, perfPlaybackRate)
                    : 0.0;

                currentPickScratchForwardCurve = easingCurveFromIndex (juce::roundToInt (performanceActiveState.parameterValues[11]));
                currentPickScratchBackwardCurve = easingCurveFromIndex (juce::roundToInt (performanceActiveState.parameterValues[12]));

                currentEndSample = pingPong ? (2 * (currentSliceStartSample + currentSliceLength) - currentSliceStartSample)
                                 : stretch ? (int) (currentSliceStartSample + (double) currentPickStretchSpeedMultiplier * currentSliceLength)
                                 : scratch ? (int) (currentSliceStartSample + currentPickScratchCycleLengthHostSamples * perfPlaybackRate)
                                           : (currentSliceStartSample + currentSliceLength);

                hasCurrentPick = true;
                pickJustStarted = true;
                model.currentlyPlayingSliceIndexForUI.store (-1); // this pick isn't drawn from `model.slices` -- no index to report

                currentPickBeatQuantized = false; // Performance mode's own Sync toggle governs rate instead (currentPickNativeRateActive above)

                currentPickFilterSweepResonance = performanceActiveState.parameterValues[0];
                currentPickFilterSweepType = juce::roundToInt (performanceActiveState.parameterValues[1]);
                currentPickCurveShape = juce::roundToInt (performanceActiveState.parameterValues[2]);

                currentPickBitcrushRateValue = performanceActiveState.parameterValues[6];
                currentPickBitcrushRateMode = juce::roundToInt (performanceActiveState.parameterValues[7]);
                currentPickBitcrushBitDepthValue = performanceActiveState.parameterValues[8];
                currentPickBitcrushBitDepthMode = juce::roundToInt (performanceActiveState.parameterValues[9]);

                currentPickFlangerDelayValue = performanceActiveState.parameterValues[13];
                currentPickFlangerDelayMode = juce::roundToInt (performanceActiveState.parameterValues[14]);
                currentPickFlangerMixValue = performanceActiveState.parameterValues[15];
                currentPickFlangerMixMode = juce::roundToInt (performanceActiveState.parameterValues[16]);
                currentPickFlangerFeedbackValue = performanceActiveState.parameterValues[17];
                currentPickFlangerFeedbackMode = juce::roundToInt (performanceActiveState.parameterValues[18]);

                samplesSincePickStart = 0.0;
                const double naturalLengthHostSamples =
                    (perfPlaybackRate > 0.0) ? ((double) currentSliceLength / perfPlaybackRate) : 0.0;
                currentPickMidpointHostSamples = scratch
                    ? (currentPickScratchCycleLengthHostSamples * 0.5) : naturalLengthHostSamples;
                currentPickTapeStopDurationHostSamples = naturalLengthHostSamples;
                currentPickLengthInHostSamples = pingPong ? (2.0 * naturalLengthHostSamples)
                                                : stretch ? ((double) currentPickStretchSpeedMultiplier * naturalLengthHostSamples)
                                                : scratch ? currentPickScratchCycleLengthHostSamples
                                                          : naturalLengthHostSamples;
            }
        }
        else
        {
            // Slice Length mode: pick a fresh slice whenever nothing's
            // playing or the current one has run its course. Tape Stop's
            // "run its course" is time-based (its read position
            // deliberately never reaches currentEndSample -- see
            // currentPickTapeStopDurationHostSamples), unlike Forward/
            // Ping-Pong which are always position-based.

            // Periodic reset (Step 34): checked every SAMPLE, not once per
            // block from the block's start position -- the exact bug
            // Step 6 introduced and fixed was computing a cycle/window
            // boundary once per block, which silently missed boundaries
            // landing mid-block. This reuses Clock mode's own per-sample
            // newWindow check directly (same shape of problem, same fix)
            // rather than re-deriving it. Crossing a boundary cuts off
            // whatever's currently playing right here -- hasCurrentPick
            // false makes the while-loop below pick fresh, starting
            // exactly on this sample -- and advances to the NEXT boundary.
            if (samplePpq >= resetWindowEndPpq)
            {
                const double resetWindowBeatsNow = (double) SlicerModel::getResetBarsValue (model.resetBarsIndex.load()) * 4.0; // 4/4, live-read so a mid-stream dropdown change takes effect at the next boundary, same as Clock reference already does
                const juce::int64 windowIndex = (juce::int64) std::floor (samplePpq / resetWindowBeatsNow);
                resetWindowEndPpq = (double) (windowIndex + 1) * resetWindowBeatsNow;

                hasCurrentPick = false;
            }

            // How much host-sample time remains until the next reset
            // boundary -- capped into every fresh pick's currentPickLength-
            // InHostSamples (and, for Tape Stop, currentPickTapeStopDuration-
            // HostSamples) below: the same established anticipatory-fade
            // pattern already used for Clock's ticks, Tape Stop, and Filter
            // Sweep (all of which already cap their own duration/length to
            // "whichever comes first"), so a pick that's about to get cut
            // off by a reset always fades out cleanly instead of clicking.
            const double samplesUntilReset = juce::jmax (0.0, (resetWindowEndPpq - samplePpq) / ppqPerSample);

            int pickAttempts = 0;

            while (! hasCurrentPick
                   || (currentPlaybackStyle == PlaybackStyle::tapeStop
                           ? samplesSincePickStart >= currentPickTapeStopDurationHostSamples
                           : currentPosition >= (double) (((currentPlaybackStyle == PlaybackStyle::pingPong
                                                                 || currentPlaybackStyle == PlaybackStyle::stretch
                                                                 || currentPlaybackStyle == PlaybackStyle::scratch)
                                                                ? currentEndSample
                                                                : juce::jmin (currentEndSample, sourceLength)) - 1)))
            {
                currentSliceIndex = pickWeightedRandomSlice();

                if (currentSliceIndex < 0 || currentSliceIndex >= (int) model.slices.size())
                {
                    hasCurrentPick = false;
                    break;
                }

                const PlaybackStyle style = indexToPlaybackStyle (pickWeightedIndex (model.playbackStyleProbabilities));
                const bool pingPong = (style == PlaybackStyle::pingPong);

                // Shared pick-start state (1.2): Slice Length mode always
                // uses the global values; per-step overrides are Sequenced-
                // mode-only. The shared helper also sets the style's own
                // currentEndSample, the midpoint, and the natural/round-trip
                // lengths -- the round-trip value IS this mode's duration
                // (2x for Ping-Pong, speed-multiplied for Stretch, the
                // scratch cycle, else the natural length), so it's passed
                // straight through as currentPickLengthInHostSamples.
                const auto& slice = model.slices[(size_t) currentSliceIndex];
                double naturalLengthHostSamples = 0.0;
                applyPickState (
                    capturePickStyleValues (PickValueSource::global, 0, 0),
                    slice.startSample, slice.endSample,
                    style, currentSliceIndex, true,
                    playbackRate, hostBpm, hostSampleRate,
                    naturalLengthHostSamples, currentPickLengthInHostSamples);
                pickJustStarted = true;
                currentPickTapeStopDurationHostSamples = naturalLengthHostSamples; // the pick's own natural length; unused for Forward/Ping-Pong/Stretch/Scratch

                // Beat-quantized slice length (Step 24 for Time-Stretch,
                // Step 26 for Repitch) — Slice Length mode only,
                // Forward/Ping-Pong only (Tape Stop and Stretch already
                // deliberately override natural duration, so quantizing
                // would fight rather than serve them). Each pitch mode has
                // its own separate toggle/default, but shares the exact
                // same target-duration calculation (computeBeatQuantizeTarget)
                // — only what currentPickQuantizedStretchRatio gets applied
                // TO differs, and that difference needs no branching here:
                // the render step below already substitutes it for
                // repitchRatio in whichever path (granular hop schedule or
                // direct-read position advance) is actually active for the
                // current pitch mode. Computed once here, at pick-start,
                // and consulted for the rest of this pick's life.
                currentPickBeatQuantized = false;
                currentPickNativeRateActive = false; // Performance mode's Sync-off override never applies outside Performance mode's own picks

                const bool beatQuantizeWanted = timeStretchMode ? model.beatQuantizeSliceLengthEnabled.load()
                                                                 : model.beatQuantizeSliceLengthEnabledRepitch.load();

                if (beatQuantizeWanted
                    && (currentPlaybackStyle == PlaybackStyle::forward || pingPong)
                    && currentSliceLength > 0)
                {
                    const double originalBpm = model.getCalculatedOriginalBpm();
                    const auto quantizeResult = computeBeatQuantizeTarget (currentSliceLength, pingPong,
                                                                            model.sampleSampleRate, originalBpm, hostBpm);

                    if (quantizeResult.quantized)
                    {
                        currentPickBeatQuantized = true;
                        currentPickQuantizedStretchRatio = quantizeResult.stretchRatio;

                        const double quantizedLengthHostSamples = quantizeResult.targetHostSeconds * hostSampleRate;
                        currentPickLengthInHostSamples = quantizedLengthHostSamples;

                        if (pingPong)
                            currentPickMidpointHostSamples = quantizedLengthHostSamples * 0.5;
                    }
                }

                // Periodic reset (Step 34): applied LAST, after beat-quantize
                // may have already substituted a different target duration --
                // reset always wins if it would cut the pick shorter than
                // whatever duration was otherwise chosen, same "whichever
                // comes first" precedence Clock mode's own tick/window caps
                // already use. currentPickTapeStopDurationHostSamples is
                // capped unconditionally too (harmless no-op for every style
                // but Tape Stop, which is exactly how that variable already
                // behaves elsewhere in this function).
                currentPickLengthInHostSamples = juce::jmin (currentPickLengthInHostSamples, samplesUntilReset);
                currentPickTapeStopDurationHostSamples = juce::jmin (currentPickTapeStopDurationHostSamples, samplesUntilReset);

                if (++pickAttempts > 1000)
                    return; // safety bail — render the rest of this block as silence
            }
        }

        if (pickJustStarted)
        {
            granularStretcher.reset (currentPosition); // matches whichever mode's active; harmless if unused this pick
            filterSweepFilter.reset(); // Step 29 -- no bleed from a previous pick; harmless if unused this pick
            filterSweepFilter.setResonance (currentPickFilterSweepResonance); // Step 45 -- this pick's own value (global, or Sequenced mode's per-step override)

            // Filter type (Step 46) -- same per-pick value/override
            // treatment as resonance just above.
            filterSweepFilter.setType (currentPickFilterSweepType == 1 ? juce::dsp::StateVariableTPTFilterType::highpass
                                      : currentPickFilterSweepType == 2 ? juce::dsp::StateVariableTPTFilterType::bandpass
                                                                        : juce::dsp::StateVariableTPTFilterType::lowpass);

            // Bitcrush (Step 48) -- no bleed from a previous pick, same
            // reasoning as filterSweepFilter.reset() above: a fresh pick
            // starts its sample-and-hold phase from scratch (counter at 0
            // grabs a fresh value on this pick's very first sample) rather
            // than continuing whatever hold phase/value the previous pick
            // (of any style) happened to leave behind.
            bitcrushHoldCounter = 0;
            std::fill (std::begin (bitcrushHeldSample), std::end (bitcrushHeldSample), 0.0f);

            // Flanger -- same "no bleed from a previous pick" reasoning as
            // bitcrushHoldCounter/bitcrushHeldSample just above: a fresh
            // pick's comb character starts from a silent delay line rather
            // than continuing whatever content the previous pick (of any
            // style) happened to leave behind.
            flangerDelayBuffer.clear();
            flangerDelayWriteIndex = 0;
        }

        const bool pingPongActive = (currentPlaybackStyle == PlaybackStyle::pingPong);
        const bool tapeStopActive = (currentPlaybackStyle == PlaybackStyle::tapeStop);
        const bool stretchActive = (currentPlaybackStyle == PlaybackStyle::stretch);
        const bool filterSweepActive = (currentPlaybackStyle == PlaybackStyle::filterSweepDown
                                         || currentPlaybackStyle == PlaybackStyle::filterSweepUp);
        const bool bitcrushActive = (currentPlaybackStyle == PlaybackStyle::bitcrush);
        const bool scratchActive = (currentPlaybackStyle == PlaybackStyle::scratch);
        const bool flangerActive = (currentPlaybackStyle == PlaybackStyle::flanger);

        // Performance mode's Sync toggle (Pass 1): currentPickNativeRateActive
        // was captured once at this pick's own pick-start (false for every
        // other mode's picks) -- substitutes a native (unsynced) rate for
        // playbackRate at every downstream render site that would otherwise
        // read it directly, so a Sync-off Performance pick can never read as
        // synced at one site and unsynced at another.
        const double effectivePickPlaybackRate = currentPickNativeRateActive
            ? (model.sampleSampleRate / hostSampleRate) : playbackRate;

        // Scratch (v1) reuses Ping-Pong's exact bounce mechanism -- same
        // foldPosition() fold, same turnaround click-avoidance fade, same
        // "position marches on unbounded, only folded at render time" gate
        // -- just with its own Rate-driven cycle length (bounceFoldLength-
        // Samples below) standing in for pingPongFoldLengthSamples, so
        // every place that mechanism is consulted below treats the two
        // styles identically via this one shared flag.
        const bool bounceActive = pingPongActive || scratchActive;

        // Bitcrush Sweep In/Out (Step 49): plain linear interpolation from
        // this pick's own set value toward (Sweep In) or away from (Sweep
        // Out) the parameter's fixed extreme, driven by the SAME
        // samplesSincePickStart/currentPickLengthInHostSamples progress
        // fraction the fade envelope and Filter Sweep's own Per Tick
        // formula already use below -- deliberately NOT Filter Sweep's
        // log-scale remap, since these are small linear integer counts
        // (sample-hold length, bit count), not a wide perceptually-
        // logarithmic frequency range. Reusing that one shared progress
        // source is also what makes this automatically span a step's full
        // EXTENDED duration for free when step-extension has stretched
        // currentPickLengthInHostSamples past the pick's natural length --
        // no separate step-extension handling needed here, same as base
        // Bitcrush already required none for its render-continue gate.
        double bitcrushProgress = 0.0;

        if (bitcrushActive)
        {
            bitcrushProgress = (currentPickLengthInHostSamples > 0.0)
                ? juce::jlimit (0.0, 1.0, samplesSincePickStart / currentPickLengthInHostSamples)
                : 1.0;
        }

        // mode: 0 Static (unchanged), 1 Sweep In (set value -> extreme),
        // 2 Sweep Out (extreme -> set value) -- see currentPickBitcrushRateMode/
        // currentPickBitcrushBitDepthMode's own doc comment in SlicerEngine.h.
        const auto sweptBitcrushValue = [bitcrushProgress] (float setValue, int mode, float extreme) -> float
        {
            if (mode == 1)
                return (float) (setValue + (extreme - setValue) * bitcrushProgress);

            if (mode == 2)
                return (float) (extreme + (setValue - extreme) * bitcrushProgress);

            return setValue;
        };

        const int effectiveBitcrushHoldSamples = bitcrushActive
            ? juce::jmax (1, juce::roundToInt (sweptBitcrushValue (currentPickBitcrushRateValue, currentPickBitcrushRateMode, SlicerModel::bitcrushRateReductionExtreme)))
            : 1;
        const int effectiveBitcrushBitDepth = bitcrushActive
            ? juce::jlimit (1, 24, juce::roundToInt (sweptBitcrushValue (currentPickBitcrushBitDepthValue, currentPickBitcrushBitDepthMode, SlicerModel::bitcrushBitDepthExtreme)))
            : 1;

        // Bitcrush sample-and-hold timing (Step 48/49): decided once per
        // OUTPUT sample here (not per channel below), so a stereo pair
        // holds/updates in lockstep rather than drifting apart -- counts
        // down to 0, grabbing a fresh value on the 0 tick and restarting
        // the count using WHATEVER the swept hold length is AT THAT
        // MOMENT (Step 49) -- Static's fixed length is just the mode==0
        // case of this same formula, so grab cadence smoothly follows a
        // Sweep In/Out's ramp rather than needing a separate code path.
        bool bitcrushShouldGrab = false;

        if (bitcrushActive)
        {
            if (bitcrushHoldCounter <= 0)
            {
                bitcrushShouldGrab = true;
                bitcrushHoldCounter = effectiveBitcrushHoldSamples - 1;
            }
            else
            {
                --bitcrushHoldCounter;
            }
        }

        // Bitcrush post-processing (Step 48): applied in the SAME slot as
        // Filter Sweep's cutoff filter below -- after the fade-gain
        // calculation, right before the sample is written to the output
        // buffer -- so it's a no-op for every other style and, like Filter
        // Sweep, never needs to know how its input sample was generated
        // (granular grain sum or direct-read interpolation) or how long
        // this pick/step will run. Bit-depth quantization happens once per
        // hold period, on the freshly grabbed sample (at THAT sample's own
        // effective bit depth, Step 49), and that quantized value is what
        // repeats for the rest of the period -- the classic stair-stepped
        // bitcrusher structure, not two independent effects.
        const auto applyBitcrush = [this, bitcrushShouldGrab, effectiveBitcrushBitDepth] (int channel, float rawSample) -> float
        {
            const int ch = juce::jmin (channel, GranularStretcher::maxChannels - 1);

            if (bitcrushShouldGrab)
            {
                const float quantStep = 2.0f / (float) (1 << effectiveBitcrushBitDepth);
                bitcrushHeldSample[ch] = quantStep * std::floor (rawSample / quantStep + 0.5f);
            }

            return bitcrushHeldSample[ch];
        };

        // Flanger Sweep In/Out: plain linear interpolation from this
        // pick's own set value toward/away from the parameter's fixed
        // extreme (same interpolation shape as Bitcrush's own Sweep
        // In/Out above), but driven by Filter Sweep's Whole Window
        // progress fraction rather than Bitcrush's per-pick one --
        // samplesSinceWindowStart/currentWindowLengthHostSamples,
        // continuous across an entire Clock-mode window or Sequenced-mode
        // step regardless of how many Subdivide retriggers happen
        // underneath, exactly the mechanism documented alongside
        // useWholeWindow further below. Unlike Filter Sweep, Flanger has
        // no Per Tick/Whole Window user toggle -- it always PREFERS whole-
        // window timing wherever a window exists (Clock mode
        // unconditionally; Sequenced mode whenever the active step has
        // Subdivide on), and only falls back to the per-pick formula where
        // there genuinely is no window to measure against (Slice Length
        // mode, and non-subdivided Sequenced steps -- numerically
        // identical to whole-window there anyway, per Filter Sweep's own
        // comment below, since only one trigger occupies the whole window/
        // step either way). This is what makes a Flanger step's sweep
        // glide once across the WHOLE step while Subdivide's retriggers
        // happen underneath, rather than resetting on every retrigger.
        const bool flangerUseWholeWindow = clockMode || (sequencedMode && sequencedSubdivisionActive);

        double flangerProgress = 0.0;

        if (flangerActive)
        {
            flangerProgress = flangerUseWholeWindow
                ? ((currentWindowLengthHostSamples > 0.0)
                       ? juce::jlimit (0.0, 1.0, samplesSinceWindowStart / currentWindowLengthHostSamples)
                       : 1.0)
                : ((currentPickLengthInHostSamples > 0.0)
                       ? juce::jlimit (0.0, 1.0, samplesSincePickStart / currentPickLengthInHostSamples)
                       : 1.0);
        }

        // mode: 0 Static (unchanged), 1 Sweep In (set value -> extreme),
        // 2 Sweep Out (extreme -> set value) -- see currentPickFlangerDelayMode/
        // currentPickFlangerMixMode's own doc comment in SlicerEngine.h.
        const auto sweptFlangerValue = [flangerProgress] (float setValue, int mode, float extreme) -> float
        {
            if (mode == 1)
                return (float) (setValue + (extreme - setValue) * flangerProgress);

            if (mode == 2)
                return (float) (extreme + (setValue - extreme) * flangerProgress);

            return setValue;
        };

        const int flangerDelayBufferLength = flangerDelayBuffer.getNumSamples();
        const int effectiveFlangerDelaySamples = (flangerActive && flangerDelayBufferLength > 2)
            ? juce::jlimit (1, flangerDelayBufferLength - 2, juce::roundToInt (
                  (sweptFlangerValue (currentPickFlangerDelayValue, currentPickFlangerDelayMode, SlicerModel::flangerDelayTimeExtremeMs) / 1000.0) * hostSampleRate))
            : 1;
        const float effectiveFlangerMix = flangerActive
            ? juce::jlimit (0.0f, 1.0f, sweptFlangerValue (currentPickFlangerMixValue, currentPickFlangerMixMode, SlicerModel::flangerMixExtreme))
            : 0.0f;
        const float effectiveFlangerFeedback = flangerActive
            ? juce::jlimit (0.0f, SlicerModel::flangerFeedbackExtreme, sweptFlangerValue (currentPickFlangerFeedbackValue, currentPickFlangerFeedbackMode, SlicerModel::flangerFeedbackExtreme))
            : 0.0f;

        // Volume ramp: a style-independent gain multiplier, unlike
        // Bitcrush/Flanger above which only apply to their own style --
        // gated on `sequencedMode` alone (every PlaybackStyle, Forward
        // included), not a currentPlaybackStyle check, since it's a pure
        // gain stage layered onto whatever the active style's own DSP
        // already produced (see the fade-gain block below). Also gates
        // OUT Slice Length/Clock mode entirely: those trigger modes never
        // populate currentPickVolumeValue/Mode (Sequenced-mode-only, no
        // global dial -- see SlicerEngine.h), so without this gate a
        // value captured during a prior Sequenced-mode step could
        // otherwise bleed into playback after switching modes.
        const bool volumeRampActive = sequencedMode;

        // Same Whole Window progress source as Flanger's flangerUseWholeWindow/
        // flangerProgress just above -- prefers samplesSinceWindowStart/
        // currentWindowLengthHostSamples (continuous across this step's
        // Subdivide retriggers) whenever Subdivide is actually on, falling
        // back to the per-pick fraction otherwise (numerically identical
        // there anyway, since only one trigger occupies the whole step).
        // This is what makes "Subdivide to 16ths across a whole bar, with
        // Volume ramping smoothly across that entire bar" work.
        const bool volumeUseWholeWindow = sequencedMode && sequencedSubdivisionActive;

        double volumeProgress = 0.0;

        if (volumeRampActive)
        {
            volumeProgress = volumeUseWholeWindow
                ? ((currentWindowLengthHostSamples > 0.0)
                       ? juce::jlimit (0.0, 1.0, samplesSinceWindowStart / currentWindowLengthHostSamples)
                       : 1.0)
                : ((currentPickLengthInHostSamples > 0.0)
                       ? juce::jlimit (0.0, 1.0, samplesSincePickStart / currentPickLengthInHostSamples)
                       : 1.0);
        }

        // mode: 0 Static (constant at the set level), 1 Ramp Up (silence
        // -> set level), 2 Ramp Down (set level -> silence) -- directional
        // language rather than Sweep In/Out's, since volume has an
        // intuitive up/down sense the other swept parameters don't. Both
        // ramp toward/away from a fixed extreme of 0.0 (silence), the
        // Volume equivalent of Bitcrush/Flanger's own fixed sweep
        // extremes, rather than a second user-adjustable value.
        const auto sweptVolumeValue = [volumeProgress] (float setValue, int mode) -> float
        {
            if (mode == 1)
                return (float) (setValue * volumeProgress);

            if (mode == 2)
                return (float) (setValue * (1.0 - volumeProgress));

            return setValue;
        };

        const double volumeGain = volumeRampActive
            ? juce::jlimit (0.0, 1.0, (double) sweptVolumeValue (currentPickVolumeValue, currentPickVolumeMode))
            : 1.0;

        // Read position decided once per OUTPUT sample here (not per
        // channel below), same "stereo pair stays in lockstep" reasoning
        // as Bitcrush's shared hold counter above -- both channels read
        // the delay line the same number of samples back, they just carry
        // different content there.
        const int flangerReadIndex = (flangerActive && flangerDelayBufferLength > 0)
            ? (((flangerDelayWriteIndex - effectiveFlangerDelaySamples) % flangerDelayBufferLength + flangerDelayBufferLength) % flangerDelayBufferLength)
            : 0;

        // Flanger post-processing: applied in the SAME slot as Bitcrush/
        // Filter Sweep just above -- after the fade-gain calculation,
        // right before the sample is written to the output buffer -- so
        // it's a no-op for every other style. The freshly rendered DRY
        // sample plus Feedback's own share of whatever the line held
        // effectiveFlangerDelaySamples ago is what gets written into the
        // delay line (the classic feedback comb-filter structure -- this
        // is what makes Feedback actually resonant rather than just a
        // one-shot echo), while the OUTPUT stays a plain wet/dry crossfade
        // between the dry sample and that same delayed value, independent
        // of Feedback. effectiveFlangerFeedback is already clamped to
        // flangerFeedbackExtreme (well short of 1.0), so this recursion is
        // always stable -- never a runaway buildup.
        const auto applyFlanger = [this, flangerReadIndex, effectiveFlangerMix, effectiveFlangerFeedback] (int channel, float drySample) -> float
        {
            const int ch = juce::jmin (channel, GranularStretcher::maxChannels - 1);
            const int bufLen = flangerDelayBuffer.getNumSamples();

            if (bufLen <= 0)
                return drySample;

            float* delayData = flangerDelayBuffer.getWritePointer (ch);
            const float delayed = delayData[flangerReadIndex];

            delayData[flangerDelayWriteIndex % bufLen] = drySample + effectiveFlangerFeedback * delayed;

            return drySample + effectiveFlangerMix * (delayed - drySample);
        };

        // Ping-Pong's currentEndSample is a full round trip (2x slice
        // length), Stretch's is one stretched pass (startSample +
        // stretchSpeedMultiplier * slice length), and Scratch's
        // (v1) is its own Rate-driven cycle length — all three can
        // legitimately run past sourceLength when the slice sits at the
        // very end of the buffer. That's fine: the actual read position
        // below is always either the FOLDED one (Ping-Pong/Scratch) or
        // safely bounded to the true slice by GranularStretcher itself
        // (Stretch always renders through it), regardless. The sourceLength
        // clamp is only needed for Forward/Tape Stop, where the raw
        // (unfolded) position IS the read position.
        const bool extendedRangeActive = bounceActive || stretchActive;
        const int schedulingEndSample = extendedRangeActive ? currentEndSample : juce::jmin (currentEndSample, sourceLength);

        // Tape Stop position-exhaustion fix: a step-extended (or Whole
        // Window/long-tick Clock) decel can now legitimately ask for more
        // real time than the slice has actual source samples for -- the
        // ramping tapeStopRateMultiplier only covers HALF that time's
        // worth of source distance on average (a linear 1.0->0.0 ramp),
        // but even that can exceed the slice's own length once the
        // requested duration is more than roughly 2x its natural length.
        // Once currentPosition would run past the slice's real content,
        // freeze it right there (hold the last sample/grain position)
        // rather than either reading into whatever audio happens to follow
        // it in the buffer or, as this used to do, cutting the whole pick
        // to silence outright while the gain envelope was still very much
        // audible. See its use below (positionForRead, and the granular
        // path's grainSourceHopSamples override).
        const bool tapeStopPositionExhausted = tapeStopActive && currentPosition >= (double) (schedulingEndSample - 1);

        // Shared render step for both modes: only output a sample while
        // we're within the current pick's bounds. In Clock mode, once a
        // short slice naturally finishes before the next tick, this
        // condition goes false and we correctly render silence until the
        // next forced retrigger resets currentPosition. Tape Stop is
        // gated on its own decel timer instead (currentPickTapeStop-
        // DurationHostSamples/samplesSincePickStart) rather than position
        // -- see tapeStopPositionExhausted above for why position alone
        // can no longer be trusted to reach the timer's own end at the
        // same moment. Stretch is gated on currentPickLengthInHostSamples
        // the same way, always (harmless/numerically identical to the old
        // position-based gate for Clock/Slice Length modes' own Stretch,
        // whose currentEndSample still encodes the same multiplier-based
        // length those modes have always used -- position advancing at
        // the plain playbackRate reaches that boundary at exactly
        // currentPickLengthInHostSamples there too, so this was always a
        // like-for-like swap, not a behaviour change, there).
        //
        // Step-extension fix: Sequenced mode's Forward/Ping-Pong/Filter
        // Down/Up now need the same duration-based gate too -- their
        // duration is authoritative from the step's own declared length
        // there (see the pick-start branch above), decoupled from
        // currentEndSample/position entirely once a step is extended
        // past its natural unit. Scoped to sequencedMode specifically
        // (rather than applied unconditionally, the way Stretch's swap
        // safely was) since Clock mode's own tick-forced retriggering
        // makes the equivalence argument above murkier there -- this
        // avoids touching Clock/Slice Length mode behaviour at all,
        // which nothing here was asked to change.
        const bool pickWithinSchedule = tapeStopActive
            ? (samplesSincePickStart < currentPickTapeStopDurationHostSamples)
            : (stretchActive || sequencedMode)
                ? (samplesSincePickStart < currentPickLengthInHostSamples)
                : (currentPosition < (double) (schedulingEndSample - 1));

        if (hasCurrentPick && pickWithinSchedule)
        {
            // Fade gain: clamp each fade to at most half this pick's own
            // (effective) length, so a very short slice/tick can't have
            // overlapping/inverted fades that would silence it entirely.
            // Shared by both pitch modes — an overall envelope wrapped
            // around whichever render step produced the dry sample below.
            const double halfPickLength = currentPickLengthInHostSamples * 0.5;
            const double fadeInSamples = juce::jmin (fadeInSamplesRequested, halfPickLength);
            const double fadeOutSamples = juce::jmin (fadeOutSamplesRequested, halfPickLength);
            const double samplesRemaining = currentPickLengthInHostSamples - samplesSincePickStart;

            double gain = 1.0;

            if (fadeInSamples > 0.0 && samplesSincePickStart < fadeInSamples)
                gain = samplesSincePickStart / fadeInSamples;

            // Tape Stop (Step 21): an additional rate multiplier, ramping
            // from 1.0 to 0.0 across currentPickTapeStopDurationHostSamples
            // -- Linear (default) or Exponential per Curve Shape (Step 46,
            // see applyCurveShape() above), layered on top of whatever
            // Pitch Mode already produces below. Gain rides the SAME curve,
            // REPLACING (not stacking with) the normal model.fadeOutMs for this
            // style — if rate hit exactly 0 while gain stayed at full, the
            // engine would get stuck holding/repeating a single sample (a
            // buzz) instead of fading to silence. model.fadeInMs above is
            // unaffected.
            double tapeStopRateMultiplier = 1.0;

            if (tapeStopActive)
            {
                const double rawProgress = (currentPickTapeStopDurationHostSamples > 0.0)
                    ? juce::jlimit (0.0, 1.0, samplesSincePickStart / currentPickTapeStopDurationHostSamples)
                    : 1.0; // degenerate zero-length duration -- treat as already fully stopped
                const double progress = applyCurveShape (rawProgress, currentPickCurveShape);
                tapeStopRateMultiplier = 1.0 - progress;

                gain = juce::jmin (gain, tapeStopRateMultiplier);
            }
            else
            {
                if (fadeOutSamples > 0.0 && samplesRemaining < fadeOutSamples)
                    gain = juce::jmin (gain, samplesRemaining / fadeOutSamples);

                // Ping-Pong (Step 19), and Scratch (v1) via the same
                // bounceActive flag: the bounce point isn't a true edit --
                // audio isn't symmetric around any given sample, so
                // reversing direction there clicks just like the pick's own
                // start/end would without a fade. Treat the midpoint as an
                // internal boundary and apply the SAME model.fadeInMs/model.fadeOutMs
                // envelope around it: fading out approaching it (mirrors
                // the pick's own end-fade) and back in leaving it (mirrors
                // the start-fade). Layered into the same overall `gain`, so
                // it wraps whichever render path below produced the dry
                // sample -- Repitch or Time-Stretch alike. Curve Shape
                // (Step 46) applies here too, sharing applyCurveShape()
                // with Tape Stop's decel above rather than a separate
                // formula -- Linear (default) is the original behaviour.
                // Only ONE midpoint is faded per pick here, same as
                // Ping-Pong's own existing step-extension behaviour -- a
                // multi-cycle extended Scratch step gets this treatment at
                // its first turnaround only, not every subsequent bounce
                // (per-turnaround fading is deferred to v2, same as
                // per-direction curve shaping).
                if (bounceActive)
                {
                    const double distanceBeforeMidpoint = currentPickMidpointHostSamples - samplesSincePickStart;
                    const double distanceAfterMidpoint = samplesSincePickStart - currentPickMidpointHostSamples;

                    if (distanceBeforeMidpoint >= 0.0 && distanceBeforeMidpoint < fadeOutSamples)
                        gain = juce::jmin (gain, applyCurveShape (distanceBeforeMidpoint / fadeOutSamples, currentPickCurveShape));

                    if (distanceAfterMidpoint >= 0.0 && distanceAfterMidpoint < fadeInSamples)
                        gain = juce::jmin (gain, applyCurveShape (distanceAfterMidpoint / fadeInSamples, currentPickCurveShape));
                }
            }

            gain = juce::jlimit (0.0, 1.0, gain);

            // Volume ramp: an ADDITIONAL multiplier on top of the base
            // fade-in/out gain just clamped above, not a replacement for
            // it -- Volume handles the musical gesture across the whole
            // step/window, the base fade still handles click-avoidance at
            // the pick's own hard start/end boundaries (and, for
            // Ping-Pong/Scratch, its midpoint). Both are already clamped
            // to [0, 1] individually, so multiplying them together stays
            // in range with no further clamp needed. A no-op (1.0) outside
            // Sequenced mode -- see volumeRampActive above.
            gain *= volumeGain;

            // Filter Down/Filter Up (Step 29/30): cutoff computed once per
            // sample here (shared across every output channel below, not
            // recomputed per channel). Log-scale interpolation -- not
            // linear Hz -- between start/end frequency, since frequency
            // perception is logarithmic; this is what keeps the sweep
            // sounding musically even rather than front-loaded. Filter Down
            // sweeps filterSweepStartHz -> filterSweepEndHz (open to
            // closed, the classic breakbeat/DnB "filter close"); Filter Up
            // is the mirror image, filterSweepEndHz -> filterSweepStartHz.
            //
            // The progress fraction itself depends on Filter Sweep scope
            // (Clock mode only -- Slice Length mode has no concept of a
            // "window", so it always behaves like Per Tick regardless of
            // the scope setting, same as before this scope choice existed):
            //   Per Tick (default) -- samplesSincePickStart /
            //     currentPickLengthInHostSamples, resetting at every
            //     retrigger -- today's Step 29 behaviour, unchanged.
            //   Whole Window -- samplesSinceWindowStart / currentWindow-
            //     LengthHostSamples instead, continuous across every tick
            //     in the window (ticks keep retriggering normally --
            //     unlike Tape Stop's Whole Window, nothing here overrides
            //     that), only resetting when a new window begins.
            // Sequenced mode's Subdivide (Step 47) forces this same Whole
            // Window behaviour whenever the currently-playing step has a
            // subdivision rate set -- its own "window" is that one step's
            // total duration (see samplesSinceWindowStart/currentWindow-
            // LengthHostSamples set at that step's pick-start), so the
            // sweep glides continuously across the whole step while the
            // subdivision retriggers happen underneath, rather than each
            // retrigger getting its own reset sweep. A non-subdivided
            // Sequenced step still falls through to the Per Tick formula
            // below, but the two are numerically identical there (only one
            // trigger occupies the whole step either way), so this changes
            // nothing for existing, non-subdivided steps.
            float filterSweepCutoffHz = filterSweepStartHz;

            if (filterSweepActive)
            {
                const bool useWholeWindow = (clockMode && (model.filterSweepScope.load() == FilterSweepScope::wholeWindow))
                                             || (sequencedMode && sequencedSubdivisionActive);

                const double progress = useWholeWindow
                    ? ((currentWindowLengthHostSamples > 0.0)
                           ? juce::jlimit (0.0, 1.0, samplesSinceWindowStart / currentWindowLengthHostSamples)
                           : 1.0)
                    : ((currentPickLengthInHostSamples > 0.0)
                           ? juce::jlimit (0.0, 1.0, samplesSincePickStart / currentPickLengthInHostSamples)
                           : 1.0);

                const bool isUp = (currentPlaybackStyle == PlaybackStyle::filterSweepUp);
                const double sweepStartHz = isUp ? (double) filterSweepEndHz : (double) filterSweepStartHz;
                const double sweepEndHz = isUp ? (double) filterSweepStartHz : (double) filterSweepEndHz;

                filterSweepCutoffHz = (float) (sweepStartHz * std::pow (sweepEndHz / sweepStartHz, progress));
                filterSweepFilter.setCutoffFrequency (filterSweepCutoffHz);
            }

            // Tape Stop doesn't fold position at all (it decelerates a
            // plain forward read, then position-exhaustion freezes/holds
            // via its own entirely separate mechanism -- see
            // tapeStopPositionExhausted/positionForRead above) -- always
            // plain forward for it, deliberately excluded from looping so
            // Tape Stop's "grinding to a stop" character stays exactly
            // that, never repeating. Ping-Pong keeps its existing pingPong
            // bounce, which already loops additional round trips for free
            // once elapsed time exceeds one period -- no change needed
            // there. Forward and Filter Down/Up (Step-extension fix) get
            // the loop fold instead of plain forward: once a step's
            // declared duration exceeds one natural unit (one raw slice
            // playthrough -- see the shared Forward/Ping-Pong/Filter
            // Down/Up duration branch above), this is what makes that
            // unit repeat -- wrapping position back to the slice's own
            // start every sliceLength rather than marching on unbounded
            // -- for as long as the render-continue gate keeps letting
            // rendering happen, with no separate loop-boundary
            // bookkeeping needed here. Stretch gets the same loop fold,
            // for the same reason, just with a stretched pass (rather
            // than the raw slice) as the repeating unit -- see the
            // stretchActive branch below for how that pass itself is
            // computed. All three are no-ops whenever a pick's actual
            // duration never reaches a full natural unit/pass in the
            // first place (mathematically identical to plain forward for
            // that shorter span) -- true always in Clock/Slice Length
            // modes (their own duration formulas never let position
            // exceed one natural unit), and in Sequenced mode whenever a
            // step isn't extended past its natural length. Scratch (v1)
            // shares Ping-Pong's own pingPong fold via bounceActive -- its
            // "natural unit" is one Rate cycle rather than one round trip
            // through the slice (see bounceFoldLengthSamples below), but
            // the looping mechanism itself (foldPosition()'s own modulo,
            // once elapsed time exceeds one period) is identical.
            const GranularStretcher::PlaybackStyle grainPlaybackStyle =
                bounceActive ? GranularStretcher::PlaybackStyle::pingPong
              : tapeStopActive ? GranularStretcher::PlaybackStyle::forward
                               : GranularStretcher::PlaybackStyle::loop;

            // Sequenced-mode Ping-Pong half-content window (Step 44):
            // passing HALF the slice's length as the fold boundary (both
            // to GranularStretcher's own grain-start folding below and to
            // the direct-read foldPosition() call further down) makes the
            // round trip reflect within just the first half of the
            // slice's content, rather than the full slice -- a normal-
            // rate there-and-back cycle through half the content take
            // exactly one normal pick's natural length, which is what
            // lets it fit within a single step without needing any rate
            // change at all. Irrelevant (and harmless) for Forward, since
            // foldPosition()'s length argument is only consulted for
            // pingPong/loop -- Stretch (loop) DOES consult it, as the
            // point it wraps back to the slice's own start; unaffected by
            // this half-length special case since that's Ping-Pong-only.
            // Slice Length/Clock modes' own Ping-Pong pace is completely
            // unaffected since this only ever differs from the full slice
            // length when sequencedMode is true.
            const double pingPongFoldLengthSamples = (sequencedMode && pingPongActive)
                ? ((double) currentSliceLength * 0.5)
                : (double) currentSliceLength;

            // Scratch's own fold length (v1, source-domain samples): ONE
            // LEG of its Rate cycle, already clamped to the slice's own
            // content by computeScratchCycleLengthHostSamples() at
            // pick-start -- see currentPickScratchCycleLengthHostSamples's
            // own doc comment. Converted back from that captured host-
            // domain length via playbackRate, the same conversion used to
            // build it in the first place, so the two stay consistent
            // sample for sample.
            const double scratchFoldLengthSamples = scratchActive
                ? (currentPickScratchCycleLengthHostSamples * 0.5 * effectivePickPlaybackRate)
                : 0.0;

            // Shared fold length actually passed to foldPosition()/
            // renderAndAdvance() below, wherever pingPongFoldLengthSamples
            // used to be the only such value -- Ping-Pong's own (slice-
            // content-derived) length, or Scratch's (Rate-cycle-derived)
            // one, per bounceActive's own style check above.
            const double bounceFoldLengthSamples = scratchActive ? scratchFoldLengthSamples : pingPongFoldLengthSamples;

            // Forward/Backward Curve (Scratch v2) -- Linear/Linear for
            // every style but Scratch, which reproduces foldPosition()'s
            // own default (plain constant-speed bounce, byte-identical to
            // before this existed) -- Ping-Pong itself never passes
            // anything else, regardless of bounceActive.
            const EasingCurve scratchForwardCurveForRender = scratchActive ? currentPickScratchForwardCurve : EasingCurve::linear;
            const EasingCurve scratchBackwardCurveForRender = scratchActive ? currentPickScratchBackwardCurve : EasingCurve::linear;

            // Performance mode's Sync-off override (currentPickNativeRateActive)
            // excludes the Time-Stretch granular path the same way stretchActive
            // already forces INTO it regardless of the global Pitch Mode --
            // a per-pick override of which render path runs, not just of the
            // rate value used within one. Native/unsynced playback means
            // neither pitch-shifted nor tempo-matched, which the direct-read
            // path below already gives for free; the granular path's default
            // (non-Stretch) branch derives its own rate from block-level
            // sourceHopSamples/repitchRatio, which a per-pick rate override
            // can't reach by substitution alone. Stretch itself is
            // deliberately exempt (see its own doc comment just below) --
            // its character is independent of Sync, same as it's already
            // independent of Pitch Mode.
            if ((timeStretchMode && ! currentPickNativeRateActive) || stretchActive)
            {
                // Stretch (Step 22) always renders through GranularStretcher,
                // even in Repitch mode -- it's a deliberate character
                // effect, not something that should vanish depending on an
                // unrelated global toggle. It gets its OWN grain parameters
                // here (never the user-facing Pitch Mode Time-Stretch grain
                // size/window shape/pitch shift, none of which apply):
                // outputHopSamples/grainSize come from
                // currentPickStretchGrainSizeMs, and sourceHopSamples is
                // derived from playbackRate/currentPickStretchSpeedMultiplier
                // -- Grain Speed is a FIXED character constant (Step-
                // extension fix), unrelated to how long the pick actually
                // plays. That's governed separately, entirely by the
                // step's own declared length (currentPickLengthInHost-
                // Samples, set at pick-start) -- one full traversal of the
                // slice at Grain Speed's pace is just the "natural unit"
                // for a Stretch pick, the same role a plain slice playback
                // is for Forward; if the declared length is longer than
                // that one pass, grainPlaybackStyle (loop, below) is what
                // makes the SAME pass repeat to fill the remainder, rather
                // than this formula trying to stretch a single pass to
                // fit. Tape Stop's rate multiplier, by contrast,
                // is layered onto BOTH each grain's own internal read-rate
                // (the same slot pitchRatio already multiplies) AND the
                // source-domain distance between grain spawns -- so as the
                // decel progresses, new grains converge toward the same,
                // increasingly frozen position instead of continuing to
                // introduce fresh material at the original pace while only
                // their internal read slows. outputHopSamples (the
                // real-time spawn cadence) is deliberately left alone in
                // that case, same as pitchRatio never touches it either.
                double grainOutputHopSamples = outputHopSamples;
                // Beat-quantized slice length (Step 24): this pick's own
                // stretch ratio replaces the global repitchRatio here,
                // symmetrically with the scheduling-position advance rate
                // below -- currentPickBeatQuantized is only ever true for
                // Forward/Ping-Pong Slice-Length picks (never alongside
                // tapeStopActive), so these two overrides can't collide.
                // Position-exhaustion fix: once tapeStopPositionExhausted,
                // grain spawns stop marching forward entirely (0, rather
                // than the still-decaying-but-nonzero
                // sourceHopSamples * tapeStopRateMultiplier) -- new grains
                // keep spawning at outputHopSamples' normal real-time
                // cadence (that's untouched), but always at the SAME
                // (frozen) source position, holding that content while
                // gain keeps fading, instead of continuing to march the
                // grain engine's own internal position into whatever
                // audio follows the slice in the buffer.
                double grainSourceHopSamples = tapeStopPositionExhausted ? 0.0
                                              : tapeStopActive ? (sourceHopSamples * tapeStopRateMultiplier)
                                              : currentPickBeatQuantized ? (outputHopSamples * srConversionRatio * currentPickQuantizedStretchRatio)
                                                                         : sourceHopSamples;
                double grainGrainSizeHostSamples = grainSizeHostSamples;
                double grainPitchRatio = tapeStopActive ? (pitchRatio * tapeStopRateMultiplier) : pitchRatio;
                GranularStretcher::WindowShape grainWindowShapeToUse = grainWindowShapeForBlock;

                if (stretchActive)
                {
                    grainGrainSizeHostSamples = (double) currentPickStretchGrainSizeMs / 1000.0 * hostSampleRate;
                    grainOutputHopSamples = grainGrainSizeHostSamples * 0.5; // same fixed 50% overlap convention

                    // Grain Speed: a FIXED character constant (Step-
                    // extension fix reverted this back to its original
                    // role) -- how fast, relative to normal playback,
                    // grains march through the source material for ONE
                    // pass. Completely independent of the step's declared
                    // length; that's handled separately, entirely by
                    // grainPlaybackStyle (loop, above) repeating this same
                    // pass for as long as the render-continue gate keeps
                    // rendering.
                    grainSourceHopSamples = grainOutputHopSamples * (effectivePickPlaybackRate / (double) currentPickStretchSpeedMultiplier);

                    grainPitchRatio = 1.0; // no user pitch shift for this style -- fully self-contained/hardcoded
                    grainWindowShapeToUse = GranularStretcher::WindowShape::hardEdge;
                }

                float channelSums[GranularStretcher::maxChannels] = {};
                granularStretcher.renderAndAdvance (model.sampleBuffer, sourceChannels,
                                                     grainOutputHopSamples, grainSourceHopSamples,
                                                     (double) currentSliceStartSample, bounceFoldLengthSamples, grainPlaybackStyle,
                                                     grainGrainSizeHostSamples, srConversionRatio, grainPitchRatio,
                                                     grainWindowShapeToUse, channelSums,
                                                     scratchForwardCurveForRender, scratchBackwardCurveForRender);

                for (int outCh = 0; outCh < outChannels; ++outCh)
                {
                    const int srcCh = juce::jmin (juce::jmin (outCh, sourceChannels - 1), GranularStretcher::maxChannels - 1);
                    float outputSample = channelSums[srcCh] * (float) gain;

                    if (filterSweepActive)
                        outputSample = filterSweepFilter.processSample (outCh, outputSample);

                    if (bitcrushActive)
                        outputSample = applyBitcrush (outCh, outputSample);

                    if (flangerActive)
                        outputSample = applyFlanger (outCh, outputSample);

                    buffer.addSample (outCh, i, outputSample);
                }
            }
            else
            {
                // Position-exhaustion fix: once tapeStopPositionExhausted, a
                // literal frozen SINGLE sample has no waveform variation --
                // it's inaudible (or a single click, then silence) no
                // matter what the gain envelope is still doing, which
                // defeats the whole point of continuing to render (verified
                // via the debug GAIN-near-zero log: the ramp itself reaches
                // near-zero right at the pick's real end, ~98% of the way
                // through, not at the freeze point -- the frozen single
                // sample was just inaudible for that whole stretch).
                // Instead, loop a short (freezeLoopLengthMs) window of REAL
                // audio ending at the slice's own boundary -- genuinely
                // audible content for the gain envelope to keep fading
                // through. The loop's own playback position is driven by
                // currentPosition's continued (still-decaying)
                // advance past the boundary, not a separate real-time
                // clock, so the loop itself keeps slowing down right along
                // with the rest of the decel, consistent with Tape Stop's
                // character elsewhere. A forward sawtooth loop (jump back
                // to the window start every freezeWindowLength) rather than
                // a crossfaded one -- the small periodic click at each wrap
                // reads as part of the "stuck tape" character rather than a
                // bug, and keeping it simple matches this pass's own scope
                // (mechanism verification, not a polished new effect). A
                // no-op for every other style/case, where this is just
                // currentPosition itself, unchanged.
                double positionForRead = currentPosition;

                if (tapeStopPositionExhausted)
                {
                    constexpr double freezeLoopLengthMs = 25.0;
                    const double freezeLoopLengthSourceSamples =
                        juce::jmax (1.0, (freezeLoopLengthMs / 1000.0) * model.sampleSampleRate);

                    const double freezeWindowEnd = (double) (schedulingEndSample - 1);
                    const double freezeWindowStart = juce::jmax ((double) currentSliceStartSample,
                                                                   freezeWindowEnd - freezeLoopLengthSourceSamples);
                    const double freezeWindowLength = juce::jmax (1.0, freezeWindowEnd - freezeWindowStart);

                    const double elapsedSinceFreeze = juce::jmax (0.0, currentPosition - freezeWindowEnd);
                    const double loopedOffset = std::fmod (elapsedSinceFreeze, freezeWindowLength);
                    positionForRead = freezeWindowStart + loopedOffset;
                }

                // Shared position-mapping (Step 19): the same foldPosition()
                // GranularStretcher uses for its own grain-start scheduling,
                // so Ping-Pong behaves identically regardless of pitch mode.
                // For Forward this is the identity — foldedReadPosition ==
                // positionForRead exactly, same as before this existed.
                const double foldedReadPosition = (double) currentSliceStartSample
                    + GranularStretcher::foldPosition (positionForRead - (double) currentSliceStartSample,
                                                        bounceFoldLengthSamples, grainPlaybackStyle,
                                                        scratchForwardCurveForRender, scratchBackwardCurveForRender);

                const int idx0 = juce::jlimit (0, sourceLength - 1, (int) foldedReadPosition);
                const int idx1 = juce::jmin (idx0 + 1, sourceLength - 1);
                const float frac = (float) (foldedReadPosition - (double) idx0);

                for (int outCh = 0; outCh < outChannels; ++outCh)
                {
                    const int srcCh = juce::jmin (outCh, sourceChannels - 1);
                    const float s0 = model.sampleBuffer.getSample (srcCh, idx0);
                    const float s1 = model.sampleBuffer.getSample (srcCh, idx1);
                    float sample = (s0 + frac * (s1 - s0)) * (float) gain;

                    if (filterSweepActive)
                        sample = filterSweepFilter.processSample (outCh, sample);

                    if (bitcrushActive)
                        sample = applyBitcrush (outCh, sample);

                    if (flangerActive)
                        sample = applyFlanger (outCh, sample);

                    buffer.addSample (outCh, i, sample);
                }
            }

            // Flanger delay line write position: advanced once per OUTPUT
            // sample here (not per channel above), same lockstep reasoning
            // as the read index computed alongside effectiveFlangerDelay-
            // Samples earlier -- both channels' applyFlanger() calls above
            // already wrote this sample's dry content at the PRE-advance
            // index, so this just moves the line forward one slot for the
            // next sample to read/write against.
            if (flangerActive && flangerDelayBuffer.getNumSamples() > 0)
                flangerDelayWriteIndex = (flangerDelayWriteIndex + 1) % flangerDelayBuffer.getNumSamples();

            // Tape Stop layers its rate multiplier onto the SAME shared
            // advance both pitch modes' scheduling position relies on --
            // this is what makes the read position (and therefore
            // schedulingEndSample/foldedReadPosition above) actually slow
            // down and, as the spec calls out, deliberately fall short of
            // the slice's real end sample in real time. Beat-quantized
            // slice length substitutes this pick's own quantized stretch
            // ratio for repitchRatio here too, in exactly the same place
            // playbackRate itself is built from repitchRatio above -- in
            // Time-Stretch mode (Step 24) that keeps currentPosition
            // landing precisely on the quantized target duration, matching
            // the granular hop schedule above sample for sample; in Repitch
            // mode (Step 26) this line IS the varispeed playback rate for
            // the direct-read path below, so the same substitution is what
            // "adjusts the normal repitch-mode rate calculation" -- no
            // further pitch-mode-specific code needed anywhere else.
            // Sequenced-mode Ping-Pong (Step 44) needs no rate change at
            // all -- position advances at exactly the same rate as every
            // other style/mode; see the halved fold-length above/below
            // instead, which is what actually compresses the round trip
            // into a normal pick's timeframe.
            const double effectivePlaybackRate = tapeStopActive ? (effectivePickPlaybackRate * tapeStopRateMultiplier)
                                                : currentPickBeatQuantized ? ((model.sampleSampleRate / hostSampleRate) * currentPickQuantizedStretchRatio)
                                                                           : effectivePickPlaybackRate;
            currentPosition += effectivePlaybackRate;
            samplesSincePickStart += 1.0;
        }

        // Filter Sweep's Whole Window scope (Step 30) needs true window-
        // elapsed real time, not just time spent actively rendering a pick
        // -- unlike samplesSincePickStart above, this increments every
        // sample the window is open, including any silence between a
        // slice naturally finishing early and the next forced tick, so a
        // Whole Window sweep stays locked to the window's actual wall-
        // clock length rather than lagging behind it. Slice Length mode
        // has no concept of a window, so this is simply never consulted
        // there (see the useWholeWindow guard above, which already
        // requires clockMode).
        // Sequenced mode (Step 47) also needs this incrementing -- it's
        // reused there as the Subdivide retrigger scheduler's own clock
        // (see the sequencedMode branch above), not just the Filter Sweep
        // Whole Window progress fraction.
        if (clockMode || sequencedMode)
            samplesSinceWindowStart += 1.0;
    }
}

//==============================================================================
// Audition loop (Step 25) -- the raw, generative-engine-bypassing render.
//==============================================================================
void SlicerEngine::renderAudition (juce::AudioBuffer<float>& buffer, double hostSampleRate)
{
    const int trimStart = model.trimStartSample.load();
    const int trimEnd = model.trimEndSample.load();
    const int sourceLength = model.sampleBuffer.getNumSamples();
    const int sourceChannels = model.sampleBuffer.getNumChannels();
    const int outChannels = buffer.getNumChannels();
    const int numSamples = buffer.getNumSamples();

    if (trimEnd - trimStart <= 0 || sourceLength == 0 || sourceChannels == 0)
        return;

    // Native pitch/speed: sample-rate matching only (correcting for the
    // loaded file's own sample rate vs. the host's) — never repitchRatio,
    // which is what keeps this "exactly the source content" regardless of
    // model.loopLengthBars/tempo. No fades either — the loop seam is meant to be
    // audibly exposed, not hidden.
    const double auditionRate = model.sampleSampleRate / hostSampleRate;

    for (int i = 0; i < numSamples; ++i)
    {
        if (auditionPosition < (double) trimStart || auditionPosition >= (double) (trimEnd - 1))
            auditionPosition = (double) trimStart;

        const int idx0 = juce::jlimit (0, sourceLength - 1, (int) auditionPosition);
        const int idx1 = juce::jmin (idx0 + 1, sourceLength - 1);
        const float frac = (float) (auditionPosition - (double) idx0);

        for (int outCh = 0; outCh < outChannels; ++outCh)
        {
            const int srcCh = juce::jmin (outCh, sourceChannels - 1);
            const float s0 = model.sampleBuffer.getSample (srcCh, idx0);
            const float s1 = model.sampleBuffer.getSample (srcCh, idx1);
            buffer.addSample (outCh, i, s0 + frac * (s1 - s0));
        }

        auditionPosition += auditionRate;
    }

    // Audition playhead (Step 28) — once per block is plenty for a 30fps
    // UI poll (WaveformDisplay's timer), so this doesn't need to be a
    // per-sample store inside the loop above.
    model.auditionPlaybackPositionForUI.store ((int) auditionPosition);
}

//==============================================================================
// MIDI input dispatch layer + Sequencer pattern bank (Pass 1: immediate
// recall; Pass 2: Set Interval/End of Pattern switch timing)
//==============================================================================
void SlicerEngine::handleIncomingMidi (const juce::MidiBuffer& midiMessages, bool hostTransportPlaying)
{
    for (const auto metadata : midiMessages)
    {
        const auto message = metadata.getMessage();

        if (message.isNoteOn())
            dispatchNoteOn (message.getNoteNumber(), hostTransportPlaying);
    }
}
void SlicerEngine::dispatchNoteOn (int noteNumber, bool hostTransportPlaying)
{
    // The routing point: checks current context (TriggerMode) and calls
    // whichever handler applies. Sequenced mode's pattern bank and
    // Performance mode's state bank (Pass 1) are the two handlers today.
    switch (model.triggerMode.load())
    {
        case TriggerMode::sequenced:
            if (model.midiLearnArmed.load())
                model.completeMidiLearn (noteNumber);
            else
                handlePatternSwitchNoteOn (noteNumber);
            break;

        case TriggerMode::performance:
            handlePerformanceStateNoteOn (noteNumber, hostTransportPlaying);
            break;

        case TriggerMode::sliceLength:
        case TriggerMode::clock:
        default:
            break; // MIDI ignored entirely here
    }
}
void SlicerEngine::handlePatternSwitchNoteOn (int noteNumber)
{
    if (! model.patternBank[(size_t) noteNumber].populated)
        return; // empty slot -- no-op regardless of timing mode, same as Pass 1

    if (model.patternSwitchTiming.load() == PatternSwitchTiming::immediate)
    {
        handleSequencerPatternRecallNoteOn (noteNumber);
        return;
    }

    // Set Interval / End of Pattern: don't switch yet -- arm the pending
    // target and let the per-sample boundary check in processBlock()'s
    // sequencedMode branch apply it once the chosen boundary is crossed. A
    // newer note-on arriving before that boundary just overwrites this same
    // atomic, so the newest one always wins -- there's never more than one
    // pending switch to track.
    pendingPatternSwitchNote.store (noteNumber);
    patternSwitchIntervalBoundaryArmed = false; // force Set Interval's "next occurrence" to be recomputed fresh from wherever ppq is on the very next per-sample check
}

void SlicerEngine::handleSequencerPatternRecallNoteOn (int noteNumber)
{
    const auto& snapshot = model.patternBank[(size_t) noteNumber];

    if (! snapshot.populated)
        return; // empty slot -- no-op, current pattern keeps playing undisturbed

    model.sequencerGrid = snapshot.grid;
    model.sequencerCellParameterOverrides = snapshot.parameterOverrides;
    model.sequencerCellExtendedLengthSteps = snapshot.extendedLengthSteps;
    model.stepResolutionIndex.store (snapshot.stepResolutionIndex);
    model.patternLengthBarsIndex.store (snapshot.patternLengthBarsIndex);
    activePatternBankSlot.store (noteNumber);

    // Force the step-boundary tracker to re-sync against the newly recalled
    // grid on the very next check, below in processBlock() -- same re-init
    // setTriggerMode() already relies on when switching INTO Sequenced mode.
    sequencedModeInitialized = false;
}
void SlicerEngine::handlePerformanceStateNoteOn (int noteNumber, bool hostTransportPlaying)
{
    // Physical MIDI in Performance mode is playback-only -- the on-screen
    // keyboard's click handler (setFocusedPerformanceStateSlot(), below) is
    // the only thing that ever changes focus. Pressing the FOCUSED slot's
    // own key live-auditions whatever's currently being edited
    // (model.performanceWorkingState, played via the shared trim atomics --
    // exactly the same live objects the parameter panel/waveform trim
    // handles write into); pressing any OTHER slot's key plays back
    // whatever's already saved there, via a frozen copy of its own
    // independent snapshot (including its own saved trim), so auditioning
    // it can never disturb model.performanceWorkingState or the focused slot's
    // in-progress edits. An empty, unfocused slot is a no-op regardless of
    // Quantize Recall -- checked up front, before either the immediate or
    // deferred path below runs, same as Pass 1.
    if (noteNumber != model.focusedPerformanceStateSlot.load() && ! model.performanceStateBank[(size_t) noteNumber].populated)
        return; // empty, unfocused slot -- no-op, whatever's already playing keeps playing undisturbed

    // Quantize Recall (see its own doc comment above): defer to the next
    // grid point instead of switching right now, UNLESS the host transport
    // isn't playing -- there's no meaningful beat position to quantize
    // against without it, so this falls back to the immediate path below,
    // same as Quantize Recall being off entirely.
    if (model.performanceQuantizeRecallEnabled.load() && hostTransportPlaying)
    {
        // A newer note-on before the boundary just overwrites this same
        // atomic, so the newest one always wins -- there's never more than
        // one pending recall to track. Re-armed (false) so the per-sample
        // boundary check in processBlock() recomputes "next occurrence"
        // fresh from wherever ppq is right now, not from whenever an
        // earlier pending note-on arrived.
        pendingPerformanceRecallNote.store (noteNumber);
        performanceQuantizeRecallBoundaryArmed = false;
        return;
    }

    applyPerformanceStateRecall (noteNumber);
}
void SlicerEngine::applyPerformanceStateRecall (int noteNumber)
{
    if (noteNumber == model.focusedPerformanceStateSlot.load())
    {
        performancePlaybackIsFocused = true;
    }
    else
    {
        const auto& snapshot = model.performanceStateBank[(size_t) noteNumber];

        if (! snapshot.populated)
            return; // slot emptied (or lost focus) between the note-on that armed a deferred recall and this boundary -- no-op, same as the immediate path's own check

        currentlyPlayingPerformanceSnapshot = snapshot;
        performancePlaybackIsFocused = false;
    }

    // Force the very next per-sample check in processBlock()'s
    // performanceMode branch to start a fresh pick from whichever source
    // was just selected above -- same same-call, same-lock handoff
    // handleSequencerPatternRecallNoteOn() already uses via
    // sequencedModeInitialized.
    performanceRecallPending = true;
}

//==============================================================================
// Sequenced pattern randomization (Step 40/41)
//==============================================================================
void SlicerEngine::randomizeSequence()
{
    model.clearSequence(); // same wipe Clear Sequence itself uses (Step 41) -- juce::CriticalSection is re-entrant, so re-locking below is safe

    const juce::ScopedLock sl (model.sampleLock);

    const int rows = model.getSequencerNumRows();
    const int columns = model.getSequencerNumSteps();

    if (rows <= 0 || columns <= 0)
        return;

    // Each row's natural length in steps, computed once up front (Step 40)
    // -- the shared SlicerModel::getSequencerNaturalLengthSteps() accessor,
    // the same one SequencerGrid's piano-roll bar (and the engine's own
    // step-extension clamp) uses, so this can never drift from them. Needed
    // per-row on every pass below, so it's not worth recomputing from
    // scratch each time. The accessor's own lock is re-entrant -- we
    // already hold model.sampleLock (same as the model setters this loop
    // body calls).
    std::vector<int> naturalStepsPerRow ((size_t) rows, 1);

    for (int row = 0; row < rows; ++row)
        naturalStepsPerRow[(size_t) row] = model.getSequencerNaturalLengthSteps (row);

    // Tracks which columns are already claimed by some previously-placed
    // hit's FULL span, not just its starting column -- what stops a longer
    // bar from getting fragmented by another hit landing mid-span.
    std::vector<bool> columnOccupied ((size_t) columns, false);

    // Fair round-robin placement (Step 40): a single sequential row-by-row
    // sweep let early rows (or simply rows lucky enough to go first)
    // greedily claim most of the grid before later rows ever got a turn --
    // a row that never gets a placement opportunity obviously can't show
    // its own length either, which was the real cause of length-awareness
    // seeming broken for later rows. Instead, run repeated PASSES, each
    // pass offering every row exactly one placement opportunity in a
    // freshly reshuffled order, so no row (and no fixed position within a
    // pass) is systematically favoured. Stops once a whole pass places
    // nothing -- either the grid is full, or every remaining attempt lost
    // its random roll -- capped generously against runaway looping.
    std::vector<int> rowOrder ((size_t) rows);

    for (int i = 0; i < rows; ++i)
        rowOrder[(size_t) i] = i;

    constexpr int maxPasses = 4096;

    for (int pass = 0; pass < maxPasses; ++pass)
    {
        // Fisher-Yates shuffle, reshuffled every pass.
        for (int i = rows - 1; i > 0; --i)
        {
            const int j = random.nextInt (i + 1);
            std::swap (rowOrder[(size_t) i], rowOrder[(size_t) j]);
        }

        bool placedAnythingThisPass = false;

        for (const int row : rowOrder)
        {
            const int naturalSteps = naturalStepsPerRow[(size_t) row];

            // Randomized starting search position (Step 40) -- searching
            // from column 0 every time would otherwise bias placements
            // toward low column indices across the whole grid.
            const int startColumn = random.nextInt (columns);
            int foundColumn = -1;

            for (int offset = 0; offset < columns; ++offset)
            {
                const int column = (startColumn + offset) % columns;
                const int spanEnd = juce::jmin (columns, column + naturalSteps);

                bool spanFree = true;

                for (int c = column; c < spanEnd; ++c)
                {
                    if (columnOccupied[(size_t) c])
                    {
                        spanFree = false;
                        break;
                    }
                }

                if (spanFree)
                {
                    foundColumn = column;
                    break;
                }
            }

            if (foundColumn < 0)
                continue; // no free span anywhere for this row right now

            if (random.nextFloat() < 0.35f)
            {
                // Style (Step 41) is drawn from the same weighted
                // model.playbackStyleProbabilities table Slice Length/Clock
                // modes already use -- NOT flat/uniform chance -- so
                // turning a style's weight down elsewhere also makes
                // Randomize reach for it less often here. Default weights
                // are Forward-only, so this reproduces exactly the old
                // all-Forward behaviour until those weights are touched.
                const int placedStyle = pickWeightedIndex (model.playbackStyleProbabilities);
                model.sequencerGrid[(size_t) (row * columns + foundColumn)] = placedStyle;

                // Per-style "randomize parameters" opt-in (see
                // getRandomizeParametersForStyle()'s own doc comment) --
                // unchecked (the default) leaves this step with no
                // override at all, identical to the behaviour above before
                // this feature existed. Checked rolls an independent
                // random value for every parameter this style actually
                // owns (getApplicableSequencerCellParameters() minus the
                // general Subdivide/Volume entries, same exclusion
                // PlaybackStyleParameterPanel uses), plus an independent
                // random Static/Sweep In/Sweep Out mode for whichever of
                // those are swept.
                if (placedStyle >= 0 && placedStyle < (int) randomizeParametersForStyle.size()
                    && randomizeParametersForStyle[(size_t) placedStyle])
                {
                    for (const int paramIndex : SlicerModel::getApplicableSequencerCellParameters (placedStyle))
                    {
                        if (paramIndex == 5 || paramIndex == 19) // Subdivide, Volume -- general, not this style's own
                            continue;

                        const juce::String paramName = SlicerModel::getSequencerCellParameterName (paramIndex);

                        // Discrete (Filter Type/Curve Shape/Rate/Forward
                        // Curve/Backward Curve/etc.) picks a random option
                        // index; everything else (Resonance/Grain Size/
                        // Grain Speed, and a swept parameter's own Value)
                        // picks a random point in its min/max range --
                        // isSequencerCellParameterSwept() and
                        // isSequencerCellParameterDiscrete() never overlap.
                        if (SlicerModel::isSequencerCellParameterDiscrete (paramIndex))
                        {
                            const int numOptions = SlicerModel::getSequencerCellParameterNumOptions (paramIndex);
                            const int randomOption = numOptions > 0 ? random.nextInt (numOptions) : 0;
                            model.setSequencerCellParameterOverride (row, foundColumn, paramName, (float) randomOption);
                        }
                        else
                        {
                            const float minValue = SlicerModel::getSequencerCellParameterMin (paramIndex);
                            const float maxValue = SlicerModel::getSequencerCellParameterMax (paramIndex);
                            model.setSequencerCellParameterOverride (row, foundColumn, paramName, minValue + random.nextFloat() * (maxValue - minValue));
                        }

                        if (SlicerModel::isSequencerCellParameterSwept (paramIndex))
                        {
                            const int modeIndex = paramIndex + 1;
                            const int numModes = SlicerModel::getSequencerCellParameterNumOptions (modeIndex);
                            const int randomMode = numModes > 0 ? random.nextInt (numModes) : 0;
                            model.setSequencerCellParameterOverride (row, foundColumn, SlicerModel::getSequencerCellParameterName (modeIndex), (float) randomMode);
                        }
                    }
                }

                const int spanEnd = juce::jmin (columns, foundColumn + naturalSteps);

                for (int c = foundColumn; c < spanEnd; ++c)
                    columnOccupied[(size_t) c] = true;

                placedAnythingThisPass = true;
            }
        }

        if (! placedAnythingThisPass)
            break;
    }
}


