#pragma once

#include <JuceHeader.h>
#include <array>
#include "EasingCurve.h"

//==============================================================================
/** Step-17: lightweight overlap-add granular time-stretcher.

    Repitch mode (unchanged, elsewhere) resamples a single read pointer
    through the source at `playbackRate`, so pitch follows playback speed.
    This class is the alternative: it plays short, windowed "grains" of the
    source at their native, sample-rate-corrected-only rate (pitch stays
    fixed relative to the source), while spacing the grains' START
    positions apart to track tempo instead — classic overlap-add granular
    synthesis, no FFT/phase vocoder. An optional pitchRatio (Step 18) can
    then shift that fixed pitch up or down without touching how the grains
    are scheduled, so stretch amount and pitch stay independent.

    Owns no reference to the source buffer or to any scheduling state
    (which slice is picked, when the next one gets triggered) — that stays
    entirely the engine's job and is identical for both pitch modes.
    This only turns "the pick currently in progress, from this source
    position" into one host output sample at a time. */
class GranularStretcher
{
public:
    // hardEdge (Step 22): a short, hard-edged fade — full gain across
    // nearly the whole grain, with just a brief linear ramp at each end
    // to avoid a true instantaneous discontinuity. Deliberately NOT a
    // smooth taper like Hann/Triangular: grain boundaries stay audible
    // rather than being hidden, which is Stretch's whole "seams become
    // the sound" character. Not exposed via the user-facing Pitch Mode
    // Time-Stretch window-shape control — Stretch is the only style that
    // uses it, always, with its own fixed grain size to match.
    enum class WindowShape { hann, triangular, hardEdge };

    // loop (Step-extension fix, Stretch only): plain forward playback that
    // wraps back to the slice's own start every sliceLength, rather than
    // marching on unbounded (forward) or bouncing there-and-back
    // (pingPong) -- "keep actively repeating the same stretched pass"
    // rather than either running off the end or reversing direction. See
    // the engine's Stretch render branch for why this exists: once a
    // step's declared length exceeds one full stretched pass, the SAME
    // pass repeats to fill the remainder, and this is what makes that
    // happen automatically (for as long as the render-continue gate keeps
    // letting rendering happen) without any separate pass-boundary
    // bookkeeping in the caller.
    //
    // pingPong is also what the engine's Scratch style (v1) passes
    // here -- it's the same bounce fold, just with Scratch's own Rate-
    // driven cycle length standing in for Ping-Pong's slice-derived one
    // (see SlicerEngine.cpp's bounceFoldLengthSamples). Named for its
    // first use rather than renamed to something more generic, since it's
    // an internal implementation detail only the engine and this
    // class ever see.
    enum class PlaybackStyle { forward, pingPong, loop };

    static constexpr int maxChannels = 2;

    // Step 19: shared position-mapping, used identically by both pitch
    // modes' render paths — this class's own grain-start scheduling
    // below, and the engine's direct-read (Repitch) path, which
    // calls this same function directly. Forward is the identity (today's
    // behaviour, byte-for-byte). Ping-Pong folds an unbounded, ever-
    // increasing "elapsed since slice start" into [0, sliceLength):
    // counting up for the first sliceLength (the forward leg), then back
    // down for the next sliceLength (the backward leg), repeating. loop
    // folds the same unbounded input into [0, sliceLength) too, but with a
    // plain wraparound (modulo) instead of a bounce -- always counting
    // forward, restarting from 0 every sliceLength.
    //
    // forwardCurve/backwardCurve (Scratch v2): optional per-leg easing,
    // defaulting to EasingCurve::linear for both -- which reproduces
    // today's exact math (constant speed within each leg), so every
    // EXISTING caller (Ping-Pong, and this class's own pingPong-style use
    // for Stretch's loop mode never reaches this branch) is completely
    // unaffected. When either curve isn't linear, the otherwise-linear
    // position within whichever leg elapsedSourceSamples currently falls
    // in gets re-warped by that leg's own curve (forwardCurve for the
    // counting-up leg, backwardCurve for the counting-down one) -- see
    // applyEasingCurve()'s own doc comment for what each shape implies
    // about the resulting speed. Only ever non-default for Scratch, from
    // both the engine's own direct-read call and this class's
    // internal grain-start-scheduling call inside renderAndAdvance(), so
    // the two pitch modes keep behaving identically the same way they
    // already always have for every other style.
    static double foldPosition (double elapsedSourceSamples, double sliceLength, PlaybackStyle style,
                                 EasingCurve forwardCurve = EasingCurve::linear,
                                 EasingCurve backwardCurve = EasingCurve::linear);

    // Call whenever a new pick begins (fresh slice chosen, or a Clock-mode
    // retrigger) — clears every grain and queues one to spawn immediately
    // at startSourcePosition, so sound starts with no gap.
    void reset (double startSourcePosition);

    // Call once per host output sample while a pick is active and Time-
    // Stretch mode is selected. Spawns new grains on schedule (every
    // outputHopSamples, sourceHopSamples further into the source than the
    // previous grain's start — that marching-forward position gets run
    // through foldPosition() at the moment each grain spawns, so Ping-Pong
    // bounces grain PLACEMENT while every grain still reads forward
    // internally at its own rate), advances every active grain by
    // srConversionRatio * pitchRatio, and adds this sample's summed
    // windowed output into channelSumsOut[0 .. sourceChannels-1]
    // (sourceChannels clamped to maxChannels; caller must clear/own that
    // buffer's lifetime).
    //
    // pitchRatio (Step 18) only scales each grain's own internal
    // read-rate — it has no effect on outputHopSamples/sourceHopSamples,
    // which is what keeps stretch amount and pitch independently
    // controllable. pitchRatio == 1.0 is a complete no-op (matches
    // pre-Step-18 behaviour exactly).
    // forwardCurve/backwardCurve (Scratch v2): passed straight through to
    // this call's own internal foldPosition() use (grain-START scheduling
    // -- see the class doc comment above), defaulting to Linear/Linear
    // (today's exact behaviour) for every caller but Scratch's own
    // Time-Stretch-pitch-mode render path.
    void renderAndAdvance (const juce::AudioBuffer<float>& sourceBuffer,
                            int sourceChannels,
                            double outputHopSamples,
                            double sourceHopSamples,
                            double sliceStartSample,
                            double sliceLength,
                            PlaybackStyle style,
                            double grainSizeHostSamples,
                            double srConversionRatio,
                            double pitchRatio,
                            WindowShape windowShape,
                            float* channelSumsOut,
                            EasingCurve forwardCurve = EasingCurve::linear,
                            EasingCurve backwardCurve = EasingCurve::linear);

private:
    struct Grain
    {
        bool active = false;
        double sourcePosition = 0.0;
        double hostSamplesPlayed = 0.0; // drives both the window envelope and this grain's lifetime
    };

    void spawnGrain (double startSourcePosition);
    static float windowGain (double progress, WindowShape shape);

    // ~2 grains are ever concurrently active at a fixed 50% overlap; this
    // gives headroom rather than cutting it exactly to the minimum.
    static constexpr int numGrains = 4;
    std::array<Grain, numGrains> grains;

    double hopAccumulator = 0.0;
    double nextGrainSourceStart = 0.0;
    bool pendingImmediateSpawn = true;
};
