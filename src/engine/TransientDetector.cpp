#include "TransientDetector.h"

#include <algorithm>
#include <cmath>

namespace nedit::engine {

namespace {

    [[nodiscard]] float oneSampleCoeff (float timeMs, double sampleRate) noexcept
    {
        if (timeMs <= 0.0f)
            return 1.0f;

        const double timeSeconds = static_cast<double> (timeMs) / 1000.0;
        return static_cast<float> (1.0 - std::exp (-1.0 / (timeSeconds * sampleRate)));
    }

    [[nodiscard]] std::int64_t clampFrame (std::int64_t value, std::int64_t lo,
                                           std::int64_t hi) noexcept
    {
        return value < lo ? lo : (value > hi ? hi : value);
    }

} // namespace

void TransientDetector::analyze (const float* const* channels, int numChannels,
                                 std::int64_t frames, double sampleRate)
{
    envelope.clear();
    derivative.clear();
    globalMaxDerivative = 0.0f;
    noiseFloor = 0.0f;

    numFrames = frames;
    analyzedSampleRate = sampleRate;

    if (frames <= 0 || numChannels <= 0 || channels == nullptr)
    {
        numFrames = 0;
        return;
    }

    const auto count = static_cast<std::size_t> (frames);

    // Mono-sum of |x| across channels -- detection only; playback keeps
    // the full multichannel buffer.
    std::vector<float> monoSum (count, 0.0f);

    for (int ch = 0; ch < numChannels; ++ch)
    {
        const float* channelData = channels[ch];

        for (std::size_t i = 0; i < count; ++i)
            monoSum[i] += std::abs (channelData[i]);
    }

    if (numChannels > 1)
    {
        const float scale = 1.0f / static_cast<float> (numChannels);
        for (auto& s : monoSum)
            s *= scale;
    }

    // --- 1) Rectified envelope follower (asymmetric attack/release) ---
    envelope.resize (count);

    const float attackCoeff = oneSampleCoeff (kAttackTimeMs, sampleRate);
    const float releaseCoeff = oneSampleCoeff (kReleaseTimeMs, sampleRate);

    float env = 0.0f;

    for (std::size_t i = 0; i < count; ++i)
    {
        const float rectified = monoSum[i];
        const float coeff = (rectified > env) ? attackCoeff : releaseCoeff;
        env += coeff * (rectified - env);
        envelope[i] = env;
    }

    // --- 2) Positive derivative of the envelope ---
    derivative.assign (count, 0.0f);

    double sumPositiveDerivative = 0.0;
    std::int64_t numPositiveDerivative = 0;

    for (std::size_t i = 1; i < count; ++i)
    {
        const float d = envelope[i] - envelope[i - 1];
        const float positiveD = std::max (0.0f, d);
        derivative[i] = positiveD;

        if (positiveD > globalMaxDerivative)
            globalMaxDerivative = positiveD;

        if (positiveD > 0.0f)
        {
            sumPositiveDerivative += static_cast<double> (positiveD);
            ++numPositiveDerivative;
        }
    }

    // Noise floor: mean of the positive derivative values. A global
    // estimate -- if quiet-vs-loud-passage material needs a windowed/local
    // floor, this is the spot to revisit (same note as the original).
    noiseFloor = (numPositiveDerivative > 0)
                     ? static_cast<float> (sumPositiveDerivative
                                           / static_cast<double> (numPositiveDerivative))
                     : 0.0f;
}

std::vector<Slice> TransientDetector::detectSlices (float sensitivity, float holdoffMs,
                                                    std::int64_t rangeStart,
                                                    std::int64_t rangeEnd) const
{
    std::vector<Slice> slices;

    if (! hasAnalysis() || numFrames == 0)
        return slices;

    // Negative sentinel = "the whole analysed buffer".
    if (rangeStart < 0)
        rangeStart = 0;
    if (rangeEnd < 0)
        rangeEnd = numFrames;

    rangeStart = clampFrame (rangeStart, 0, numFrames);
    rangeEnd = clampFrame (rangeEnd, rangeStart, numFrames);

    sensitivity = std::clamp (sensitivity, 0.0f, 1.0f);

    std::vector<std::int64_t> onsets;

    // sensitivity == 0 is guaranteed zero transients (contract carried
    // over from the original prototype) -- skip straight to "whole range
    // is one slice".
    if (sensitivity > 0.0f)
    {
        const float threshold = globalMaxDerivative
                              - sensitivity * (globalMaxDerivative - noiseFloor);

        const auto holdoffFrames = static_cast<std::int64_t> (
            (static_cast<double> (holdoffMs) / 1000.0) * analyzedSampleRate);

        // Allow an onset right at the range start.
        std::int64_t lastOnset = rangeStart - holdoffFrames;

        // Start at max(1, rangeStart): the rising-edge check needs i-1.
        for (std::int64_t i = std::max<std::int64_t> (1, rangeStart); i < rangeEnd; ++i)
        {
            const auto idx = static_cast<std::size_t> (i);

            if (derivative[idx] > threshold
                && derivative[idx] >= derivative[idx - 1]
                && (i - lastOnset) >= holdoffFrames)
            {
                onsets.push_back (i);
                lastOnset = i;
            }
        }
    }

    // Nothing before the first detected onset gets orphaned -- the range
    // start plays the role position 0 played pre-trim.
    if (onsets.empty() || onsets.front() > rangeStart)
        onsets.insert (onsets.begin(), rangeStart);

    for (std::size_t i = 0; i < onsets.size(); ++i)
    {
        Slice slice;
        slice.startFrame = onsets[i];
        slice.endFrame = (i + 1 < onsets.size()) ? onsets[i + 1] : rangeEnd;

        if (slice.lengthFrames() > 0)
            slices.push_back (slice);
    }

    return slices;
}

std::int64_t TransientDetector::findNearestPeak (std::int64_t targetFrame,
                                                 std::int64_t searchRadiusFrames,
                                                 std::int64_t rangeStart,
                                                 std::int64_t rangeEnd) const
{
    if (! hasAnalysis() || numFrames == 0)
        return targetFrame;

    if (rangeStart < 0)
        rangeStart = 0;
    if (rangeEnd < 0)
        rangeEnd = numFrames;

    rangeStart = clampFrame (rangeStart, 0, numFrames);
    rangeEnd = clampFrame (rangeEnd, rangeStart, numFrames);

    if (rangeEnd <= rangeStart)
        return clampFrame (targetFrame, 0, numFrames - 1);  // degenerate range

    const std::int64_t rangeLastIndex = rangeEnd - 1;
    const std::int64_t lo = clampFrame (targetFrame - searchRadiusFrames, rangeStart, rangeLastIndex);
    const std::int64_t hi = clampFrame (targetFrame + searchRadiusFrames, rangeStart, rangeLastIndex);

    std::int64_t bestIndex = clampFrame (targetFrame, rangeStart, rangeLastIndex);
    float bestValue = -1.0f;

    for (std::int64_t i = lo; i <= hi; ++i)
    {
        const auto idx = static_cast<std::size_t> (i);

        if (derivative[idx] > bestValue)
        {
            bestValue = derivative[idx];
            bestIndex = i;
        }
    }

    return bestIndex;
}

} // namespace nedit::engine
