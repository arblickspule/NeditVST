// Nedit -- State layer.
//
// Global playback-rendering settings: how picks are rendered regardless
// of which mode scheduled them (fades, pitch mode, granular engine
// settings, beat-quantize).

#pragma once

#include "Types.h"

namespace nedit::state {

struct RenderState
{
    // Per-pick declick fades, clamped by the engine to half the pick length.
    float fadeInMs = 5.0f;    // >= 0
    float fadeOutMs = 15.0f;  // >= 0

    // How picks are rendered.
    PitchMode pitchMode = PitchMode::repitch;

    // Time-Stretch pitch mode only.
    float grainSizeMs = 60.0f;                              // 20 .. 150
    GrainWindowShape grainWindowShape = GrainWindowShape::hann;
    float pitchShiftSemitones = 0.0f;                       // -24 .. +24

    // Beat-quantize pick lengths (Slice Length mode), one flag per pitch
    // mode -- deliberately separate settings with different defaults,
    // matching the original.
    bool beatQuantizeTimeStretch = true;
    bool beatQuantizeRepitch = false;

    static constexpr float kMinGrainSizeMs = 20.0f;
    static constexpr float kMaxGrainSizeMs = 150.0f;
    static constexpr float kMaxPitchShiftSemitones = 24.0f;

    void sanitize() noexcept
    {
        if (fadeInMs < 0.0f)
            fadeInMs = 0.0f;

        if (fadeOutMs < 0.0f)
            fadeOutMs = 0.0f;

        grainSizeMs = clampValue (grainSizeMs, kMinGrainSizeMs, kMaxGrainSizeMs);
        pitchShiftSemitones = clampValue (pitchShiftSemitones,
                                          -kMaxPitchShiftSemitones, kMaxPitchShiftSemitones);
    }

    bool operator== (const RenderState&) const = default;
};

} // namespace nedit::state
