// Nedit -- Engine layer, DSP primitive.
//
// TPT (topology-preserving transform) state-variable filter, the
// Zavalishin structure -- behaviourally equivalent to the original's
// juce::dsp::StateVariableTPTFilter. Low/high/band-pass, cutoff swept
// per-sample without artefacts (the whole point of the TPT structure),
// resonance as Q. Used by the Filter Down / Filter Up playback styles.
//
// Framework-free, allocation-free, per-channel state for up to
// kMaxFilterChannels.

#pragma once

#include <state/Types.h>

#include <array>
#include <cmath>

namespace nedit::engine::dsp {

class SweepFilter
{
public:
    static constexpr int kMaxFilterChannels = 2;

    void prepare (double newSampleRate) noexcept
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
        update();
        reset();
    }

    void reset() noexcept
    {
        for (auto& s : state)
            s = {};
    }

    void setType (state::FilterType newType) noexcept { type = newType; }

    void setCutoffFrequency (float cutoffHz) noexcept
    {
        // Legal range: strictly below Nyquist (same clamp intent as the
        // original's snapToLegalValue).
        const auto maxCutoff = static_cast<float> (sampleRate * 0.49);
        cutoff = cutoffHz < 10.0f ? 10.0f : (cutoffHz > maxCutoff ? maxCutoff : cutoffHz);
        update();
    }

    void setResonance (float q) noexcept
    {
        resonance = q < 0.05f ? 0.05f : q;
        update();
    }

    [[nodiscard]] float processSample (int channel, float x) noexcept
    {
        auto& s = state[static_cast<std::size_t> (channel < 0 ? 0
                        : (channel >= kMaxFilterChannels ? kMaxFilterChannels - 1 : channel))];

        const float v3 = x - s.ic2eq;
        const float v1 = a1 * s.ic1eq + a2 * v3;
        const float v2 = s.ic2eq + a2 * s.ic1eq + a3 * v3;

        s.ic1eq = 2.0f * v1 - s.ic1eq;
        s.ic2eq = 2.0f * v2 - s.ic2eq;

        switch (type)
        {
            case state::FilterType::highPass: return x - k * v1 - v2;
            case state::FilterType::bandPass: return v1;
            case state::FilterType::lowPass:  break;
        }

        return v2;
    }

private:
    void update() noexcept
    {
        g = static_cast<float> (std::tan (3.141592653589793 * static_cast<double> (cutoff)
                                          / sampleRate));
        k = 1.0f / resonance;
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }

    struct ChannelState
    {
        float ic1eq = 0.0f;
        float ic2eq = 0.0f;
    };

    std::array<ChannelState, kMaxFilterChannels> state {};
    double sampleRate = 44100.0;
    float cutoff = 1000.0f;
    float resonance = 0.70710678f;  // 1/sqrt(2), same default as the original
    state::FilterType type = state::FilterType::lowPass;

    float g = 0.0f, k = 1.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
};

} // namespace nedit::engine::dsp
