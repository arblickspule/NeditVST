// Shared test helper: synthetic audio with transients at known positions.

#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

namespace nedit::test {

// A decaying burst (click) starting at each given frame: instantaneous
// attack to `amplitude`, exponential decay. Silence elsewhere. This is
// the cleanest possible material for an envelope-derivative detector --
// each burst produces exactly one strong rising edge.
[[nodiscard]] inline std::vector<float> makeClickTrack (std::int64_t numFrames,
                                                        const std::vector<std::int64_t>& clickFrames,
                                                        double sampleRate,
                                                        float amplitude = 1.0f,
                                                        double decayMs = 20.0)
{
    std::vector<float> audio (static_cast<std::size_t> (numFrames), 0.0f);

    const double decayPerFrame = std::exp (-1.0 / (decayMs / 1000.0 * sampleRate));

    for (const auto clickFrame : clickFrames)
    {
        double level = static_cast<double> (amplitude);

        for (std::int64_t i = clickFrame; i < numFrames && level > 1.0e-4; ++i)
        {
            const auto idx = static_cast<std::size_t> (i);
            audio[idx] += static_cast<float> (level);
            level *= decayPerFrame;
        }
    }

    return audio;
}

} // namespace nedit::test
