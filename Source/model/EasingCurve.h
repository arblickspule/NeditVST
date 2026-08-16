#pragma once

//==============================================================================
/** Scratch v2: a small, shared "shape a 0..1 progress value" utility --
    standard easing-function definitions, the same category of thing every
    animation/UI toolkit calls "easing curves." Deliberately its own
    header (not folded into SlicerEngine.cpp's anonymous-namespace
    applyCurveShape(), the older Linear/Exponential curve used by Tape
    Stop's decel and Ping-Pong's turnaround fade) because it needs to be
    usable from GranularStretcher.cpp too -- Scratch's granular (Time-
    Stretch pitch mode) render path calls GranularStretcher::foldPosition()
    directly, which needs these same shapes to warp grain-start positions
    the identical way the engine's own direct-read (Repitch) path
    warps its read position, keeping the two pitch modes sounding alike
    the same way foldPosition() already always has. No JUCE dependency on
    purpose, so either translation unit can include this cheaply.

    Deliberately simple closed-form quadratic/cubic shapes rather than a
    general cubic-bezier system -- four clearly distinguishable shapes,
    cheap enough to evaluate every single host sample, not a general-
    purpose animation curve library.

    Retrofitting this onto Tape Stop/Ping-Pong's own existing Linear/
    Exponential curve (SlicerEngine.cpp's applyCurveShape()) is
    explicitly future work, not done here -- the two systems stay
    independent for this pass. */
enum class EasingCurve
{
    linear,        // identity -- no shaping; today's constant-rate behaviour
    easeIn,        // slow start, accelerating toward the end
    easeOut,       // fast start, decelerating toward the end
    easeInEaseOut  // S-curve -- slow-fast-slow, physically realistic for a real hand motion
};

constexpr int numEasingCurveOptions = 4;

inline const char* getEasingCurveName (EasingCurve curve)
{
    switch (curve)
    {
        case EasingCurve::easeIn:        return "Ease In";
        case EasingCurve::easeOut:       return "Ease Out";
        case EasingCurve::easeInEaseOut: return "Ease In-Out";
        default:                         return "Linear";
    }
}

// Maps a plain option index (as stored in a per-step parameter override,
// or offered by a right-click menu -- see SlicerModel::
// getSequencerCellParameterOptionName()) to its EasingCurve value. An
// out-of-range index falls back to Linear rather than asserting, matching
// this codebase's existing defensive convention for similar lookups (see
// SlicerEngine::indexToPlaybackStyle()).
inline EasingCurve easingCurveFromIndex (int index)
{
    if (index == 1) return EasingCurve::easeIn;
    if (index == 2) return EasingCurve::easeOut;
    if (index == 3) return EasingCurve::easeInEaseOut;
    return EasingCurve::linear;
}

// progress: linear (time-based) 0..1 fraction through some span. Returns
// the shaped 0..1 fraction -- e.g. "how far through the span's DISTANCE
// we should be" given "how far through its TIME" progress represents. The
// derivative of this (with respect to progress) is what gives each shape
// its distinct velocity character:
//   linear        -- constant derivative (1) -- constant speed throughout.
//   easeIn        -- derivative 2*progress -- speed rises monotonically
//                     from 0 to a maximum right at the end (a "smoother
//                     pull" -- gentle start, building toward the end).
//   easeOut       -- derivative 2*(1-progress) -- speed starts at its
//                     maximum and falls to 0 by the end (a "sharp flick"
//                     -- fast start, tapering off).
//   easeInEaseOut -- (smoothstep, 3t^2-2t^3) derivative 6*progress*(1-
//                     progress) -- a symmetric parabola, 0 at both ends,
//                     peaking exactly at progress=0.5 -- the "slow-fast-
//                     slow" hump a real scratch stroke's speed traces.
inline double applyEasingCurve (double progress, EasingCurve curve)
{
    progress = progress < 0.0 ? 0.0 : (progress > 1.0 ? 1.0 : progress);

    switch (curve)
    {
        case EasingCurve::easeIn:
            return progress * progress;

        case EasingCurve::easeOut:
            return progress * (2.0 - progress); // == 1 - (1 - progress)^2

        case EasingCurve::easeInEaseOut:
            return progress * progress * (3.0 - 2.0 * progress); // smoothstep

        default:
            return progress;
    }
}
