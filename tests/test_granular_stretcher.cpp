#include <doctest/doctest.h>
#include "model/GranularStretcher.h"

using Style = GranularStretcher::PlaybackStyle;

TEST_CASE ("GranularStretcher::foldPosition forward is the identity")
{
    CHECK (GranularStretcher::foldPosition (0.0, 1000.0, Style::forward) == 0.0);
    CHECK (GranularStretcher::foldPosition (123.5, 1000.0, Style::forward) == 123.5);
    CHECK (GranularStretcher::foldPosition (5000.0, 1000.0, Style::forward) == 5000.0);
}

TEST_CASE ("GranularStretcher::foldPosition guards degenerate slice lengths")
{
    // sliceLength <= 0 short-circuits to the raw elapsed position for every style.
    CHECK (GranularStretcher::foldPosition (100.0, 0.0, Style::pingPong) == 100.0);
    CHECK (GranularStretcher::foldPosition (100.0, -5.0, Style::loop) == 100.0);
    CHECK (GranularStretcher::foldPosition (100.0, 0.0, Style::forward) == 100.0);
}

TEST_CASE ("GranularStretcher::foldPosition loop wraps with modulo")
{
    CHECK (GranularStretcher::foldPosition (0.0, 1000.0, Style::loop) == 0.0);
    CHECK (GranularStretcher::foldPosition (500.0, 1000.0, Style::loop) == 500.0);
    CHECK (GranularStretcher::foldPosition (1000.0, 1000.0, Style::loop) == 0.0);
    CHECK (GranularStretcher::foldPosition (2500.0, 1000.0, Style::loop) == 500.0);

    // Negative elapsed is corrected defensively into [0, sliceLength).
    CHECK (GranularStretcher::foldPosition (-500.0, 1000.0, Style::loop) == 500.0);
}

TEST_CASE ("GranularStretcher::foldPosition pingPong bounces there and back")
{
    // Forward leg: [0, sliceLength) counts up.
    CHECK (GranularStretcher::foldPosition (0.0, 1000.0, Style::pingPong) == 0.0);
    CHECK (GranularStretcher::foldPosition (500.0, 1000.0, Style::pingPong) == 500.0);

    // Backward leg: [sliceLength, 2*sliceLength) counts back down.
    CHECK (GranularStretcher::foldPosition (1000.0, 1000.0, Style::pingPong) == 1000.0);
    CHECK (GranularStretcher::foldPosition (1500.0, 1000.0, Style::pingPong) == 500.0);
    CHECK (GranularStretcher::foldPosition (1999.0, 1000.0, Style::pingPong) == 1.0);

    // Period wraps back to the start.
    CHECK (GranularStretcher::foldPosition (2000.0, 1000.0, Style::pingPong) == 0.0);
    CHECK (GranularStretcher::foldPosition (3500.0, 1000.0, Style::pingPong) == 500.0);
    CHECK (GranularStretcher::foldPosition (4500.0, 1000.0, Style::pingPong) == 500.0);

    // Negative elapsed is corrected defensively.
    CHECK (GranularStretcher::foldPosition (-100.0, 1000.0, Style::pingPong) == 100.0);
}

TEST_CASE ("GranularStretcher::foldPosition applies per-leg easing curves")
{
    // Mid-forward-leg at progress 0.5, sliceLength 1000.
    const double elapsed = 500.0;

    CHECK (GranularStretcher::foldPosition (elapsed, 1000.0, Style::pingPong, EasingCurve::linear) == 500.0);
    CHECK (doctest::Approx (GranularStretcher::foldPosition (elapsed, 1000.0, Style::pingPong, EasingCurve::easeIn)) == 250.0);
    CHECK (doctest::Approx (GranularStretcher::foldPosition (elapsed, 1000.0, Style::pingPong, EasingCurve::easeOut)) == 750.0);
    CHECK (doctest::Approx (GranularStretcher::foldPosition (elapsed, 1000.0, Style::pingPong, EasingCurve::easeInEaseOut)) == 500.0);

    // Backward leg, progress 0.5 of the way back: the curve is applied via
    // the BACKWARD curve argument -- 1000 - shaped*1000.
    CHECK (GranularStretcher::foldPosition (1500.0, 1000.0, Style::pingPong, EasingCurve::linear, EasingCurve::easeIn) == 750.0);
    CHECK (doctest::Approx (GranularStretcher::foldPosition (1500.0, 1000.0, Style::pingPong, EasingCurve::linear, EasingCurve::easeOut)) == 250.0);
}

TEST_CASE ("GranularStretcher::renderAndAdvance linear forward read")
{
    juce::AudioBuffer<float> source (1, 5000);

    for (int i = 0; i < 5000; ++i)
        source.setSample (0, i, (float) i);

    float sums[2] = { 0.0f, 0.0f };

    // Renders `count` consecutive host samples (each call advances the grain
    // by one) and returns the sum written by the last one.
    const auto renderUntil = [&] (int count, double pitchRatio = 1.0)
    {
        GranularStretcher stretcher;
        stretcher.reset (0.0);

        float last = 0.0f;

        for (int n = 0; n < count; ++n)
        {
            sums[0] = sums[1] = 0.0f;
            stretcher.renderAndAdvance (source, 1,
                                        1000.0, // outputHopSamples -- far enough that only the immediate spawn happens
                                        1.0,    // sourceHopSamples
                                        0.0,    // sliceStartSample
                                        5000.0, // sliceLength
                                        Style::forward,
                                        10.0,   // grainSizeHostSamples
                                        1.0,    // srConversionRatio
                                        pitchRatio,
                                        GranularStretcher::WindowShape::hann,
                                        sums);
            last = sums[0];
        }

        return last;
    };

    // First output sample: the immediately-spawned grain at progress 0 has
    // zero Hann gain, so the sum is silent.
    CHECK (renderUntil (1) == 0.0f);

    // Second sample: progress 1/10, Hann gain = 0.5 - 0.5*cos(2*pi*0.1),
    // reading source[1] = 1.0 -> sum = gain.
    const float expectedGain = 0.5f - 0.5f * (float) std::cos (2.0 * juce::MathConstants<double>::pi * 0.1);
    CHECK (doctest::Approx (renderUntil (2)) == expectedGain);

    // Halfway through the grain (progress 0.5) the Hann gain is 1.0, and the
    // read pointer has marched forward to source[5] = 5.0.
    CHECK (doctest::Approx (renderUntil (6)) == 5.0f);

    // The grain deactivates once hostSamplesPlayed reaches grainSize, so by
    // the 11th sample the output is silent again.
    CHECK (renderUntil (11) == 0.0f);
}

TEST_CASE ("GranularStretcher::renderAndAdvance pitchRatio scales the read rate")
{
    juce::AudioBuffer<float> source (1, 5000);

    for (int i = 0; i < 5000; ++i)
        source.setSample (0, i, (float) i);

    float sums[2] = { 0.0f, 0.0f };

    const auto renderUntil = [&] (int count, double pitchRatio)
    {
        GranularStretcher stretcher;
        stretcher.reset (0.0);

        float last = 0.0f;

        for (int n = 0; n < count; ++n)
        {
            sums[0] = sums[1] = 0.0f;
            stretcher.renderAndAdvance (source, 1,
                                        1000.0, 1.0, 0.0, 5000.0, Style::forward,
                                        10.0, 1.0, pitchRatio,
                                        GranularStretcher::WindowShape::hann,
                                        sums);
            last = sums[0];
        }

        return last;
    };

    // First sample spawns the grain at source[0] (gain 0 -> silent).
    CHECK (renderUntil (1, 2.0) == 0.0f);

    // Second sample reads source[2] (read pointer advanced 2 per host sample)
    // at progress 0.1, instead of source[1] at pitchRatio 1.0.
    const float expectedGain = 0.5f - 0.5f * (float) std::cos (2.0 * juce::MathConstants<double>::pi * 0.1);
    CHECK (doctest::Approx (renderUntil (2, 2.0)) == 2.0f * expectedGain);
}
