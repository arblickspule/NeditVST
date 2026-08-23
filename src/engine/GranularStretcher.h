// Nedit -- Engine layer.
//
// Lightweight overlap-add granular time-stretcher (no FFT/phase vocoder).
//
// Repitch mode (elsewhere) resamples a single read pointer at
// playbackRate, so pitch follows speed. This class is the alternative:
// short windowed grains play at their native, sample-rate-corrected-only
// rate (pitch fixed relative to the source) while the grains' START
// positions are spaced to track tempo. An optional pitchRatio shifts that
// fixed pitch without touching grain scheduling, keeping stretch amount
// and pitch independent.
//
// Owns no reference to the source audio and no scheduling state (which
// slice is picked, when the next pick triggers) -- that is the engine
// scheduler's job and is identical for both pitch modes. This class only
// turns "the pick in progress, from this source position" into one host
// output sample at a time.
//
// Pure C++, framework-free, allocation-free after construction --
// audio-thread safe.

#pragma once

#include "Fold.h"

#include <array>
#include <cstdint>

namespace nedit::engine {

class GranularStretcher
{
public:
    // hann/triangular: the user-facing Time-Stretch window shapes.
    // hardEdge: full gain across nearly the whole grain with a brief
    // linear ramp (10%) at each end -- grain boundaries stay audible,
    // which is the Stretch style's "seams become the sound" character.
    // Used only by Stretch, never exposed on the pitch-mode control.
    enum class WindowShape { hann, triangular, hardEdge };

    static constexpr int kMaxChannels = 2;

    // Call whenever a new pick begins (fresh slice, or a retrigger) --
    // clears every grain and queues one to spawn immediately at
    // startSourcePosition so sound starts with no gap.
    void reset (double startSourcePosition) noexcept;

    // Call once per host output sample while a pick is active in
    // Time-Stretch pitch mode. Spawns grains on schedule (every
    // outputHopSamples of host time; each start sourceHopSamples further
    // into the source than the last -- that marching position is folded
    // by foldPosition() at spawn time, so Ping-Pong bounces grain
    // PLACEMENT while each grain still reads forward internally), then
    // advances all active grains by srConversionRatio * pitchRatio and
    // accumulates this sample's summed windowed output into
    // channelSumsOut[0 .. numOutChannels-1].
    //
    // sourceChannels: numSourceChannels pointers to numSourceFrames each;
    // channels beyond kMaxChannels are ignored. channelSumsOut is
    // overwritten (not accumulated into).
    //
    // pitchRatio only scales each grain's internal read rate; it never
    // affects the hop scheduling -- that is what keeps stretch amount and
    // pitch independent. pitchRatio == 1 is a complete no-op.
    // forwardCurve/backwardCurve feed the internal foldPosition() call
    // (grain-start scheduling); only Scratch ever passes non-linear.
    void renderAndAdvance (const float* const* sourceChannels,
                           int numSourceChannels,
                           std::int64_t numSourceFrames,
                           double outputHopSamples,
                           double sourceHopSamples,
                           double sliceStartSample,
                           double sliceLength,
                           FoldStyle style,
                           double grainSizeHostSamples,
                           double srConversionRatio,
                           double pitchRatio,
                           WindowShape windowShape,
                           float* channelSumsOut,
                           state::EasingCurve forwardCurve = state::EasingCurve::linear,
                           state::EasingCurve backwardCurve = state::EasingCurve::linear) noexcept;

    // Window envelope gain at a 0..1 progress through a grain's life.
    // Public for testability and for any future visualization.
    [[nodiscard]] static float windowGain (double progress, WindowShape shape) noexcept;

private:
    struct Grain
    {
        bool active = false;
        double sourcePosition = 0.0;
        double hostSamplesPlayed = 0.0;  // drives the window envelope AND lifetime
    };

    void spawnGrain (double startSourcePosition) noexcept;

    // ~2 grains are concurrently active at the fixed 50% overlap; 4 gives
    // headroom (grain size can change live) rather than cutting it exact.
    static constexpr int kNumGrains = 4;
    std::array<Grain, kNumGrains> grains;

    double hopAccumulator = 0.0;
    double nextGrainSourceStart = 0.0;
    bool pendingImmediateSpawn = true;
};

} // namespace nedit::engine
