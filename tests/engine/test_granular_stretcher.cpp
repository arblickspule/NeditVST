// GranularStretcher: window shapes, overlap-add reconstruction, stretch
// scheduling, pitch independence.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <engine/GranularStretcher.h>

#include <cmath>
#include <vector>

using namespace nedit::engine;
using Catch::Matchers::WithinAbs;

namespace {

// Renders `numOutput` host samples of a mono source through the
// stretcher with 50% overlap (outputHop = grain/2).
std::vector<float> renderMono (GranularStretcher& stretcher,
                               const std::vector<float>& source,
                               double outputHop,
                               double sourceHop,
                               double grainSize,
                               int numOutput,
                               double srConversionRatio = 1.0,
                               double pitchRatio = 1.0,
                               GranularStretcher::WindowShape shape = GranularStretcher::WindowShape::hann,
                               FoldStyle style = FoldStyle::forward,
                               double sliceStart = 0.0,
                               double sliceLength = 0.0)
{
    std::vector<float> output (static_cast<std::size_t> (numOutput), 0.0f);
    const float* channels[] = { source.data() };

    for (int i = 0; i < numOutput; ++i)
    {
        float sums[GranularStretcher::kMaxChannels] = { 0.0f, 0.0f };

        stretcher.renderAndAdvance (channels, 1,
                                    static_cast<std::int64_t> (source.size()),
                                    outputHop, sourceHop, sliceStart, sliceLength,
                                    style, grainSize, srConversionRatio, pitchRatio,
                                    shape, sums);

        output[static_cast<std::size_t> (i)] = sums[0];
    }

    return output;
}

} // namespace

TEST_CASE ("window shapes have the right form", "[stretcher]")
{
    using WS = GranularStretcher::WindowShape;

    // Hann: zero at the ends, unity at the centre.
    CHECK_THAT (GranularStretcher::windowGain (0.0, WS::hann), WithinAbs (0.0, 1e-6));
    CHECK_THAT (GranularStretcher::windowGain (0.5, WS::hann), WithinAbs (1.0, 1e-6));
    CHECK_THAT (GranularStretcher::windowGain (1.0, WS::hann), WithinAbs (0.0, 1e-6));

    // 50%-offset complementarity (what makes 50%-overlap sum to 1).
    for (int i = 0; i <= 50; ++i)
    {
        const double t = i / 100.0;
        CHECK_THAT (GranularStretcher::windowGain (t, WS::hann)
                        + GranularStretcher::windowGain (t + 0.5, WS::hann),
                    WithinAbs (1.0, 1e-6));
    }

    // Triangular: same complementarity.
    CHECK_THAT (GranularStretcher::windowGain (0.5, WS::triangular), WithinAbs (1.0, 1e-6));
    CHECK_THAT (GranularStretcher::windowGain (0.25, WS::triangular)
                    + GranularStretcher::windowGain (0.75, WS::triangular),
                WithinAbs (1.0, 1e-6));

    // hardEdge: unity through the middle, ramps only in the outer 10%.
    CHECK_THAT (GranularStretcher::windowGain (0.05, WS::hardEdge), WithinAbs (0.5, 1e-6));
    CHECK (GranularStretcher::windowGain (0.10, WS::hardEdge) == 1.0f);
    CHECK (GranularStretcher::windowGain (0.5, WS::hardEdge) == 1.0f);
    CHECK (GranularStretcher::windowGain (0.90, WS::hardEdge) == 1.0f);
    CHECK_THAT (GranularStretcher::windowGain (0.95, WS::hardEdge), WithinAbs (0.5, 1e-6));

    // Out-of-range progress is clamped.
    CHECK (GranularStretcher::windowGain (-1.0, WS::hann) == GranularStretcher::windowGain (0.0, WS::hann));
    CHECK (GranularStretcher::windowGain (2.0, WS::hann) == GranularStretcher::windowGain (1.0, WS::hann));
}

TEST_CASE ("DC source reconstructs to unity at 50% overlap", "[stretcher]")
{
    const std::vector<float> source (44100, 1.0f);
    GranularStretcher stretcher;
    stretcher.reset (0.0);

    constexpr double grain = 512.0;
    const auto output = renderMono (stretcher, source, grain / 2.0, grain / 2.0, grain, 4096);

    // After the first full grain (warm-up), Hann at 50% overlap sums to
    // exactly 1.
    for (std::size_t i = 600; i < output.size(); ++i)
    {
        CHECK_THAT (static_cast<double> (output[i]), WithinAbs (1.0, 1e-3));

        if (std::abs (output[i] - 1.0f) > 1e-3f)
            break;  // don't spam thousands of failures
    }
}

TEST_CASE ("no-stretch passthrough reproduces a ramp", "[stretcher]")
{
    // Source = linear ramp; equal hops and 1:1 read rate => output tracks
    // the ramp exactly after warm-up.
    std::vector<float> source (44100);
    for (std::size_t i = 0; i < source.size(); ++i)
        source[i] = static_cast<float> (i) / 44100.0f;

    GranularStretcher stretcher;
    stretcher.reset (0.0);

    constexpr double grain = 512.0;
    const auto output = renderMono (stretcher, source, grain / 2.0, grain / 2.0, grain, 8192);

    for (std::size_t i = 600; i < output.size(); i += 64)
    {
        const double expected = static_cast<double> (i) / 44100.0;
        CHECK_THAT (static_cast<double> (output[i]), WithinAbs (expected, 1e-3));
    }
}

TEST_CASE ("2x stretch reads the source at half speed", "[stretcher]")
{
    std::vector<float> source (44100);
    for (std::size_t i = 0; i < source.size(); ++i)
        source[i] = static_cast<float> (i) / 44100.0f;

    GranularStretcher stretcher;
    stretcher.reset (0.0);

    constexpr double grain = 512.0;

    // sourceHop = outputHop / 2: grain starts advance through the source
    // at half the output rate.
    const auto output = renderMono (stretcher, source, grain / 2.0, grain / 4.0, grain, 8192);

    for (std::size_t i = 1024; i < output.size(); i += 128)
    {
        const double expectedCentre = static_cast<double> (i) / 2.0 / 44100.0;

        // Each grain still reads forward at native rate internally, so the
        // instantaneous value wobbles around the half-speed trajectory by
        // up to about a grain length.
        CHECK_THAT (static_cast<double> (output[i]),
                    WithinAbs (expectedCentre, grain / 44100.0));
    }
}

TEST_CASE ("pitch ratio does not change the stretch trajectory", "[stretcher]")
{
    // A slowly-varying source (so pitch-shifted grain reads still sample
    // the same neighbourhood values).
    std::vector<float> source (44100);
    for (std::size_t i = 0; i < source.size(); ++i)
        source[i] = static_cast<float> (i) / 44100.0f;

    constexpr double grain = 512.0;

    GranularStretcher plain;
    plain.reset (0.0);
    const auto reference = renderMono (plain, source, grain / 2.0, grain / 2.0, grain, 8192,
                                       1.0, 1.0);

    GranularStretcher shifted;
    shifted.reset (0.0);
    const auto pitched = renderMono (shifted, source, grain / 2.0, grain / 2.0, grain, 8192,
                                     1.0, 1.5);

    // The pitched render reads faster WITHIN grains, but grain starts
    // march identically -- so the overall trajectory stays on the ramp,
    // deviating by at most about a grain's worth of read-ahead.
    for (std::size_t i = 1024; i < pitched.size(); i += 256)
        CHECK_THAT (static_cast<double> (pitched[i]),
                    WithinAbs (static_cast<double> (reference[i]), 1.5 * grain / 44100.0));
}

TEST_CASE ("reset spawns immediately at the requested position", "[stretcher]")
{
    // Impulse-free source with a distinctive value at the pick position.
    std::vector<float> source (44100, 0.0f);
    for (std::size_t i = 22050; i < 22562; ++i)
        source[i] = 1.0f;

    GranularStretcher stretcher;
    stretcher.reset (22050.0);

    constexpr double grain = 512.0;
    const auto output = renderMono (stretcher, source, grain / 2.0, grain / 2.0, grain, 64);

    // Sound appears from the very first samples (no gap) -- the Hann
    // window starts at zero but must be nonzero within a few samples.
    bool heardSomething = false;
    for (std::size_t i = 0; i < 16; ++i)
        if (output[i] > 0.0f)
            heardSomething = true;

    CHECK (heardSomething);
}

TEST_CASE ("loop fold repeats the same pass", "[stretcher]")
{
    // Slice = [1000, 1512): a short ramp segment; loop style must keep
    // grain starts inside the slice forever.
    std::vector<float> source (44100, 0.0f);
    for (std::size_t i = 1000; i < 1512; ++i)
        source[i] = 1.0f;  // plateau inside the slice, silence outside

    GranularStretcher stretcher;
    stretcher.reset (1000.0);

    constexpr double grain = 128.0;

    // Render far longer than one pass through the slice.
    const auto output = renderMono (stretcher, source, grain / 2.0, grain / 2.0, grain, 8192,
                                    1.0, 1.0, GranularStretcher::WindowShape::hann,
                                    FoldStyle::loop, 1000.0, 512.0);

    // Late in the render, grains must still be spawning inside the slice
    // (output stays near 1), not marching into the silence beyond it.
    double lateSum = 0.0;
    for (std::size_t i = 4096; i < 8192; ++i)
        lateSum += static_cast<double> (output[i]);

    CHECK (lateSum / 4096.0 > 0.5);
}

TEST_CASE ("degenerate inputs are safe", "[stretcher]")
{
    GranularStretcher stretcher;
    stretcher.reset (0.0);

    float sums[GranularStretcher::kMaxChannels] = { 9.9f, 9.9f };

    // Null source / zero frames: output cleared, no crash.
    stretcher.renderAndAdvance (nullptr, 0, 0, 256.0, 256.0, 0.0, 0.0,
                                FoldStyle::forward, 512.0, 1.0, 1.0,
                                GranularStretcher::WindowShape::hann, sums);

    // Zero grain size: grains die instantly, output finite.
    const std::vector<float> source (1024, 1.0f);
    const float* channels[] = { source.data() };

    for (int i = 0; i < 128; ++i)
        stretcher.renderAndAdvance (channels, 1, 1024, 0.0, 0.0, 0.0, 0.0,
                                    FoldStyle::forward, 0.0, 1.0, 1.0,
                                    GranularStretcher::WindowShape::hann, sums);

    CHECK (std::isfinite (sums[0]));

    // Pool exhaustion (tiny hop vs huge grain): steal path, no crash.
    stretcher.reset (0.0);
    for (int i = 0; i < 512; ++i)
        stretcher.renderAndAdvance (channels, 1, 1024, 2.0, 2.0, 0.0, 0.0,
                                    FoldStyle::forward, 8192.0, 1.0, 1.0,
                                    GranularStretcher::WindowShape::hann, sums);

    CHECK (std::isfinite (sums[0]));
}
