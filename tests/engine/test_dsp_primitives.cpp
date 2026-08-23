// DSP primitives: SweepFilter, Bitcrusher, Flanger, curve shapes.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <engine/Easing.h>
#include <engine/dsp/Bitcrusher.h>
#include <engine/dsp/Flanger.h>
#include <engine/dsp/SweepFilter.h>

#include <cmath>
#include <numbers>
#include <vector>

using namespace nedit::engine;
using Catch::Matchers::WithinAbs;

namespace {

constexpr double kRate = 44100.0;

double rms (const std::vector<float>& xs, std::size_t from, std::size_t to)
{
    double sum = 0.0;
    for (std::size_t i = from; i < to; ++i)
        sum += static_cast<double> (xs[i]) * static_cast<double> (xs[i]);
    return std::sqrt (sum / static_cast<double> (to - from));
}

std::vector<float> sine (double freq, int n)
{
    std::vector<float> out (static_cast<std::size_t> (n));
    for (int i = 0; i < n; ++i)
        out[static_cast<std::size_t> (i)] =
            static_cast<float> (std::sin (2.0 * std::numbers::pi * freq * i / kRate));
    return out;
}

} // namespace

TEST_CASE ("curve shape: linear identity, exponential t^2", "[dsp]")
{
    using nedit::state::CurveShape;

    CHECK (applyCurveShape (0.3, CurveShape::linear) == 0.3);
    CHECK_THAT (applyCurveShape (0.5, CurveShape::exponential), WithinAbs (0.25, 1e-12));
    CHECK (applyCurveShape (0.0, CurveShape::exponential) == 0.0);
    CHECK (applyCurveShape (1.0, CurveShape::exponential) == 1.0);
}

TEST_CASE ("sweep filter passes/blocks DC by type", "[dsp][filter]")
{
    dsp::SweepFilter filter;
    filter.prepare (kRate);
    filter.setCutoffFrequency (1000.0f);
    filter.setResonance (0.7071f);

    // Settle on DC.
    float lp = 0.0f;
    filter.setType (nedit::state::FilterType::lowPass);
    for (int i = 0; i < 4096; ++i)
        lp = filter.processSample (0, 1.0f);
    CHECK_THAT (static_cast<double> (lp), WithinAbs (1.0, 1e-3));

    filter.reset();
    filter.setType (nedit::state::FilterType::highPass);
    float hp = 0.0f;
    for (int i = 0; i < 4096; ++i)
        hp = filter.processSample (0, 1.0f);
    CHECK_THAT (static_cast<double> (hp), WithinAbs (0.0, 1e-3));

    filter.reset();
    filter.setType (nedit::state::FilterType::bandPass);
    float bp = 0.0f;
    for (int i = 0; i < 4096; ++i)
        bp = filter.processSample (0, 1.0f);
    CHECK_THAT (static_cast<double> (bp), WithinAbs (0.0, 1e-3));
}

TEST_CASE ("sweep filter frequency selectivity", "[dsp][filter]")
{
    const auto highSine = sine (10000.0, 8192);

    // LP at 500 Hz: a 10 kHz sine is strongly attenuated.
    dsp::SweepFilter lp;
    lp.prepare (kRate);
    lp.setType (nedit::state::FilterType::lowPass);
    lp.setCutoffFrequency (500.0f);

    std::vector<float> lpOut (highSine.size());
    for (std::size_t i = 0; i < highSine.size(); ++i)
        lpOut[i] = lp.processSample (0, highSine[i]);

    CHECK (rms (lpOut, 4096, 8192) < 0.05 * rms (highSine, 4096, 8192));

    // HP at 500 Hz: the same sine passes nearly untouched.
    dsp::SweepFilter hp;
    hp.prepare (kRate);
    hp.setType (nedit::state::FilterType::highPass);
    hp.setCutoffFrequency (500.0f);

    std::vector<float> hpOut (highSine.size());
    for (std::size_t i = 0; i < highSine.size(); ++i)
        hpOut[i] = hp.processSample (0, highSine[i]);

    CHECK (rms (hpOut, 4096, 8192) > 0.9 * rms (highSine, 4096, 8192));
}

TEST_CASE ("sweep filter channels are independent", "[dsp][filter]")
{
    dsp::SweepFilter filter;
    filter.prepare (kRate);
    filter.setType (nedit::state::FilterType::lowPass);
    filter.setCutoffFrequency (1000.0f);

    // Drive only channel 0; channel 1 must stay silent.
    float out1 = 0.0f;
    for (int i = 0; i < 512; ++i)
    {
        (void) filter.processSample (0, 1.0f);
        out1 = filter.processSample (1, 0.0f);
    }

    CHECK_THAT (static_cast<double> (out1), WithinAbs (0.0, 1e-6));
}

TEST_CASE ("bitcrusher holds and quantizes", "[dsp][bitcrush]")
{
    dsp::Bitcrusher crusher;
    crusher.reset();

    // Slow ramp input, hold of 8: the output may only change on grab
    // ticks (every 8 samples).
    std::vector<float> out;
    int changes = 0;

    for (int i = 0; i < 256; ++i)
    {
        crusher.tick (8);
        const float x = static_cast<float> (i) / 256.0f;
        const float y = crusher.process (0, x, 16);

        if (! out.empty() && y != out.back())
            ++changes;

        out.push_back (y);
    }

    CHECK (changes <= 256 / 8);

    // Bit depth 4: quantized to multiples of 2/16.
    crusher.reset();
    constexpr float step = 2.0f / 16.0f;

    for (int i = 0; i < 64; ++i)
    {
        crusher.tick (1);
        const float y = crusher.process (0, static_cast<float> (i) / 64.0f, 4);
        const float remainder = std::abs (y / step - std::round (y / step));
        CHECK (remainder < 1e-4f);
    }
}

TEST_CASE ("bitcrusher stereo pair holds in lockstep", "[dsp][bitcrush]")
{
    dsp::Bitcrusher crusher;
    crusher.reset();

    float left = -1.0f, right = -1.0f;
    float prevLeft = -1.0f, prevRight = -1.0f;

    for (int i = 0; i < 64; ++i)
    {
        crusher.tick (4);
        left = crusher.process (0, static_cast<float> (i) * 0.01f, 16);
        right = crusher.process (1, static_cast<float> (i) * -0.01f, 16);

        // Both channels must grab on the same ticks.
        const bool leftChanged = left != prevLeft;
        const bool rightChanged = right != prevRight;

        if (i > 0)
            CHECK (leftChanged == rightChanged);

        prevLeft = left;
        prevRight = right;
    }
}

TEST_CASE ("flanger produces the delayed echo", "[dsp][flanger]")
{
    dsp::Flanger flanger;
    flanger.prepare (kRate, 10.0f);
    flanger.reset();

    constexpr int delaySamples = 100;

    // Impulse in, mix 1, feedback 0: output is ONLY the echo.
    std::vector<float> out;

    for (int i = 0; i < 300; ++i)
    {
        flanger.tick (delaySamples);
        const float dry = (i == 0) ? 1.0f : 0.0f;
        out.push_back (flanger.process (0, dry, 1.0f, 0.0f));
        flanger.advance();
    }

    CHECK_THAT (static_cast<double> (out[0]), WithinAbs (0.0, 1e-6));   // dry cancelled
    CHECK_THAT (static_cast<double> (out[delaySamples]), WithinAbs (1.0, 1e-6));  // the echo

    for (int i = 1; i < 300; ++i)
        if (i != delaySamples)
            CHECK_THAT (static_cast<double> (out[static_cast<std::size_t> (i)]),
                        WithinAbs (0.0, 1e-6));
}

TEST_CASE ("flanger feedback repeats and decays", "[dsp][flanger]")
{
    dsp::Flanger flanger;
    flanger.prepare (kRate, 10.0f);
    flanger.reset();

    constexpr int delaySamples = 50;
    constexpr float feedback = 0.5f;

    std::vector<float> out;

    for (int i = 0; i < 300; ++i)
    {
        flanger.tick (delaySamples);
        const float dry = (i == 0) ? 1.0f : 0.0f;
        out.push_back (flanger.process (0, dry, 1.0f, feedback));
        flanger.advance();
    }

    // Echoes at n*delay with amplitude feedback^(n-1).
    CHECK_THAT (static_cast<double> (out[50]), WithinAbs (1.0, 1e-6));
    CHECK_THAT (static_cast<double> (out[100]), WithinAbs (0.5, 1e-6));
    CHECK_THAT (static_cast<double> (out[150]), WithinAbs (0.25, 1e-6));
    CHECK_THAT (static_cast<double> (out[200]), WithinAbs (0.125, 1e-6));
}

TEST_CASE ("flanger dry/wet mix", "[dsp][flanger]")
{
    dsp::Flanger flanger;
    flanger.prepare (kRate, 10.0f);
    flanger.reset();

    // DC input, mix 0.5, feedback 0: before the delay fills, out = 0.5;
    // after, delayed == dry == 1 so out = 1.
    std::vector<float> out;

    for (int i = 0; i < 300; ++i)
    {
        flanger.tick (100);
        out.push_back (flanger.process (0, 1.0f, 0.5f, 0.0f));
        flanger.advance();
    }

    CHECK_THAT (static_cast<double> (out[50]), WithinAbs (0.5, 1e-6));
    CHECK_THAT (static_cast<double> (out[250]), WithinAbs (1.0, 1e-6));
}
