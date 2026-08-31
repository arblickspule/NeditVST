#include "GranularStretcher.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace nedit::engine {

namespace {

// NaN/inf-safe double -> source read index. Clamp in DOUBLE space FIRST:
// casting a non-representable double (NaN, +-inf, > int64 max) to int64
// is itself UB, so a clamp applied after the cast guards nothing. The
// negated comparison routes NaN to 0 alongside negatives.
[[nodiscard]] std::int64_t clampedSourceIndex (double pos,
                                               std::int64_t numFrames) noexcept
{
    if (! (pos >= 0.0))   // negatives AND NaN
        return 0;
    if (pos >= static_cast<double> (numFrames - 1))
        return numFrames - 1;
    return static_cast<std::int64_t> (pos);
}

} // namespace

void GranularStretcher::reset (double startSourcePosition) noexcept
{
    for (auto& grain : grains)
        grain.active = false;

    nextGrainSourceStart = startSourcePosition;
    hopAccumulator = 0.0;
    pendingImmediateSpawn = true;
}

void GranularStretcher::spawnGrain (double startSourcePosition) noexcept
{
    Grain* target = nullptr;

    for (auto& grain : grains)
    {
        if (! grain.active)
        {
            target = &grain;
            break;
        }
    }

    // Pool exhausted (shouldn't happen at 50% overlap with 4 slots, but
    // grain size can change live) -- steal whichever grain is furthest
    // into its life, since it has already faded closest to silence.
    if (target == nullptr)
    {
        for (auto& grain : grains)
            if (target == nullptr || grain.hostSamplesPlayed > target->hostSamplesPlayed)
                target = &grain;
    }

    target->active = true;
    target->sourcePosition = startSourcePosition;
    target->hostSamplesPlayed = 0.0;
}

float GranularStretcher::windowGain (double progress, WindowShape shape) noexcept
{
    progress = std::clamp (progress, 0.0, 1.0);

    if (shape == WindowShape::hann)
        return static_cast<float> (0.5 - 0.5 * std::cos (2.0 * std::numbers::pi * progress));

    if (shape == WindowShape::triangular)
        return static_cast<float> (progress < 0.5 ? (2.0 * progress)
                                                  : (2.0 * (1.0 - progress)));

    // hardEdge: full gain with a brief linear ramp (10%) at each end.
    constexpr double kEdgeFraction = 0.1;

    if (progress < kEdgeFraction)
        return static_cast<float> (progress / kEdgeFraction);

    if (progress > 1.0 - kEdgeFraction)
        return static_cast<float> ((1.0 - progress) / kEdgeFraction);

    return 1.0f;
}

void GranularStretcher::renderAndAdvance (const float* const* sourceChannels,
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
                                          state::EasingCurve forwardCurve,
                                          state::EasingCurve backwardCurve) noexcept
{
    numSourceChannels = std::clamp (numSourceChannels, 0, kMaxChannels);

    for (int ch = 0; ch < numSourceChannels; ++ch)
        channelSumsOut[ch] = 0.0f;

    // nextGrainSourceStart marches forward unbounded (the same "elapsed
    // since pick start, unfolded" quantity the direct-read path tracks) --
    // foldPosition() is applied only at the moment a grain spawns, so each
    // grain's OWN read still runs forward at its native rate; only where
    // consecutive grains START bounces for Ping-Pong / curve-shapes for
    // Scratch.
    const auto spawnAtCurrentMarch = [this, sliceStartSample, sliceLength, style,
                                      sourceHopSamples, forwardCurve, backwardCurve]() noexcept
    {
        const double folded = sliceStartSample
                            + foldPosition (nextGrainSourceStart - sliceStartSample,
                                            sliceLength, style, forwardCurve, backwardCurve);
        spawnGrain (folded);
        nextGrainSourceStart += sourceHopSamples;
    };

    if (pendingImmediateSpawn)
    {
        spawnAtCurrentMarch();
        pendingImmediateSpawn = false;
    }
    else if (outputHopSamples > 0.0)
    {
        hopAccumulator += 1.0;

        while (hopAccumulator >= outputHopSamples)
        {
            spawnAtCurrentMarch();
            hopAccumulator -= outputHopSamples;
        }
    }

    if (numSourceFrames == 0 || sourceChannels == nullptr)
        return;

    for (auto& grain : grains)
    {
        if (! grain.active)
            continue;

        const double progress = grainSizeHostSamples > 0.0
                              ? (grain.hostSamplesPlayed / grainSizeHostSamples)
                              : 1.0;
        const float gain = windowGain (progress, windowShape);

        const auto idx0 = clampedSourceIndex (grain.sourcePosition, numSourceFrames);
        const auto idx1 = std::min<std::int64_t> (idx0 + 1, numSourceFrames - 1);
        const auto frac = static_cast<float> (grain.sourcePosition
                                              - static_cast<double> (idx0));

        for (int ch = 0; ch < numSourceChannels; ++ch)
        {
            const float s0 = sourceChannels[ch][static_cast<std::size_t> (idx0)];
            const float s1 = sourceChannels[ch][static_cast<std::size_t> (idx1)];
            channelSumsOut[ch] += (s0 + frac * (s1 - s0)) * gain;
        }

        // pitchRatio only scales this per-grain read rate -- never the hop
        // scheduling above; that separation is what keeps stretch amount
        // and pitch independently controllable.
        grain.sourcePosition += srConversionRatio * pitchRatio;
        grain.hostSamplesPlayed += 1.0;

        if (grain.hostSamplesPlayed >= grainSizeHostSamples)
            grain.active = false;
    }
}

} // namespace nedit::engine
