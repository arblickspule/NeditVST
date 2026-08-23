// Nedit -- Engine layer, DSP primitive.
//
// Feedback comb flanger: per-channel circular delay line. The freshly
// rendered DRY sample plus Feedback's share of what the line held
// delaySamples ago is written into the line (the feedback recursion is
// what makes Feedback genuinely resonant rather than a one-shot echo);
// the OUTPUT is a plain wet/dry crossfade between the dry sample and that
// same delayed value, independent of Feedback.
//
// The write position advances once per OUTPUT sample (advance()), not per
// channel -- both channels read the same distance back, they just carry
// different content. Feedback is expected to be clamped well short of
// 1.0 by the caller (the state layer's kMaxFlangerFeedback = 0.88), so
// the recursion is always stable.
//
// prepare() allocates; everything on the render path is allocation-free.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace nedit::engine::dsp {

class Flanger
{
public:
    static constexpr int kMaxChannels = 2;

    // maxDelayMs: the largest delay the line must hold (the state layer's
    // kMaxFlangerDelayMs). NOT audio-thread safe -- call at setup.
    void prepare (double sampleRate, float maxDelayMs)
    {
        const auto frames = static_cast<int> (
            std::ceil ((static_cast<double> (maxDelayMs) / 1000.0) * sampleRate)) + 4;
        bufferLength = frames < 4 ? 4 : frames;

        for (auto& line : lines)
            line.assign (static_cast<std::size_t> (bufferLength), 0.0f);

        writeIndex = 0;
    }

    void reset() noexcept
    {
        for (auto& line : lines)
            std::fill (line.begin(), line.end(), 0.0f);

        writeIndex = 0;
    }

    [[nodiscard]] int maxDelaySamples() const noexcept
    {
        return bufferLength > 2 ? bufferLength - 2 : 1;
    }

    // Call once per output sample BEFORE processing the channels: fixes
    // this sample's read position (delaySamples may be swept per-sample).
    void tick (int delaySamples) noexcept
    {
        if (bufferLength <= 0)
        {
            readIndex = 0;
            return;
        }

        const int clamped = delaySamples < 1 ? 1
                          : (delaySamples > maxDelaySamples() ? maxDelaySamples() : delaySamples);

        readIndex = ((writeIndex - clamped) % bufferLength + bufferLength) % bufferLength;
    }

    // Process one channel: writes dry + feedback*delayed into the line at
    // the current write position, returns dry crossfaded with delayed.
    [[nodiscard]] float process (int channel, float drySample, float mix, float feedback) noexcept
    {
        if (bufferLength <= 0)
            return drySample;

        const auto ch = static_cast<std::size_t> (
            channel < 0 ? 0 : (channel >= kMaxChannels ? kMaxChannels - 1 : channel));

        auto& line = lines[ch];
        const float delayed = line[static_cast<std::size_t> (readIndex)];

        line[static_cast<std::size_t> (writeIndex)] = drySample + feedback * delayed;

        return drySample + mix * (delayed - drySample);
    }

    // Call once per output sample AFTER processing the channels.
    void advance() noexcept
    {
        if (bufferLength > 0)
            writeIndex = (writeIndex + 1) % bufferLength;
    }

private:
    std::array<std::vector<float>, kMaxChannels> lines;
    int bufferLength = 0;
    int writeIndex = 0;
    int readIndex = 0;
};

} // namespace nedit::engine::dsp
