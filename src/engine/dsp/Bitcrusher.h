// Nedit -- Engine layer, DSP primitive.
//
// Sample-and-hold rate reducer + bit-depth quantizer, the classic
// stair-stepped bitcrusher structure (not two independent effects):
// quantization happens once per hold period on the freshly grabbed
// sample, and that quantized value repeats for the rest of the period.
//
// Hold timing is decided once per OUTPUT sample (tick()), not per
// channel, so a stereo pair holds/updates in lockstep. The hold length is
// re-read every grab, so a swept rate smoothly changes the grab cadence
// with no separate code path.

#pragma once

#include <array>
#include <cmath>

namespace nedit::engine::dsp {

class Bitcrusher
{
public:
    static constexpr int kMaxChannels = 2;

    void reset() noexcept
    {
        holdCounter = 0;
        heldSample.fill (0.0f);
    }

    // Call once per output sample BEFORE processing the channels:
    // decides whether this sample grabs fresh input (holdSamples is the
    // CURRENT effective hold length, which may be swept).
    void tick (int holdSamples) noexcept
    {
        if (holdCounter <= 0)
        {
            shouldGrab = true;
            holdCounter = (holdSamples < 1 ? 1 : holdSamples) - 1;
        }
        else
        {
            shouldGrab = false;
            --holdCounter;
        }
    }

    // Process one channel's sample at the CURRENT effective bit depth
    // (which may be swept). Grabs+quantizes on grab ticks, repeats the
    // held value otherwise.
    [[nodiscard]] float process (int channel, float rawSample, int bitDepth) noexcept
    {
        const auto ch = static_cast<std::size_t> (
            channel < 0 ? 0 : (channel >= kMaxChannels ? kMaxChannels - 1 : channel));

        if (shouldGrab)
        {
            const int bits = bitDepth < 1 ? 1 : (bitDepth > 24 ? 24 : bitDepth);
            const float quantStep = 2.0f / static_cast<float> (1 << bits);
            heldSample[ch] = quantStep * std::floor (rawSample / quantStep + 0.5f);
        }

        return heldSample[ch];
    }

private:
    int holdCounter = 0;
    bool shouldGrab = false;
    std::array<float, kMaxChannels> heldSample {};
};

} // namespace nedit::engine::dsp
