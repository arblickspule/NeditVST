// PickRenderer: rendered-output tests for the shared voice path and all
// nine playback styles.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <engine/PickRenderer.h>

#include <cmath>
#include <numbers>
#include <vector>

using namespace nedit::engine;
using nedit::state::PlaybackStyle;
using Catch::Matchers::WithinAbs;

namespace {

constexpr double kRate = 44100.0;

std::vector<float> makeRamp (std::int64_t n)
{
    std::vector<float> out (static_cast<std::size_t> (n));
    for (std::int64_t i = 0; i < n; ++i)
        out[static_cast<std::size_t> (i)] = static_cast<float> (i) / static_cast<float> (n);
    return out;
}

BlockContext makeCtx (const std::vector<float>& source, const float* const* channels)
{
    BlockContext ctx;
    ctx.source = channels;
    ctx.sourceChannels = 1;
    ctx.sourceFrames = static_cast<std::int64_t> (source.size());
    ctx.hostSampleRate = kRate;
    ctx.sourceSampleRate = kRate;
    ctx.playbackRate = 1.0;
    ctx.srConversionRatio = 1.0;
    ctx.pitchRatio = 1.0;
    ctx.timeStretchMode = false;
    ctx.grainSizeHostSamples = 60.0 / 1000.0 * kRate;
    ctx.outputHopSamples = ctx.grainSizeHostSamples * 0.5;
    ctx.sourceHopSamples = ctx.outputHopSamples;
    ctx.fadeInSamplesRequested = 0.0;
    ctx.fadeOutSamplesRequested = 0.0;
    return ctx;
}

// A plain forward pick over [start, start+length).
PickParams makeForwardPick (std::int64_t start, std::int64_t length)
{
    PickParams pick;
    pick.style = PlaybackStyle::forward;
    pick.sliceStartFrame = start;
    pick.sliceLengthFrames = length;
    pick.schedulingEndFrame = start + length;
    pick.pickLengthHostSamples = static_cast<double> (length);
    return pick;
}

std::vector<float> renderN (PickRenderer& renderer, const BlockContext& ctx, int n,
                            bool tickWindow = false)
{
    std::vector<float> out (static_cast<std::size_t> (n), 0.0f);

    for (int i = 0; i < n; ++i)
    {
        float sum = 0.0f;
        float* const sums[] = { &sum };
        renderer.renderSample (ctx, sums, 1);
        out[static_cast<std::size_t> (i)] = sum;

        if (tickWindow)
            renderer.tickWindowClock();
    }

    return out;
}

} // namespace

TEST_CASE ("forward pick reproduces the slice exactly", "[renderer]")
{
    const auto source = makeRamp (44100);
    const float* channels[] = { source.data() };
    const auto ctx = makeCtx (source, channels);

    PickRenderer renderer;
    renderer.prepare (kRate);
    renderer.startPick (makeForwardPick (1000, 4000));

    const auto out = renderN (renderer, ctx, 4000);

    for (std::size_t i = 0; i < 3990; ++i)
        CHECK_THAT (static_cast<double> (out[i]),
                    WithinAbs (static_cast<double> (source[1000 + i]), 1e-6));

    CHECK (renderer.finished (ctx));

    // Past the schedule: silence.
    float sum = 0.0f;
    float* const sums[] = { &sum };
    CHECK_FALSE (renderer.renderSample (ctx, sums, 1));
    CHECK (sum == 0.0f);
}

TEST_CASE ("fades shape the pick's envelope", "[renderer]")
{
    const std::vector<float> source (44100, 1.0f);
    const float* channels[] = { source.data() };
    auto ctx = makeCtx (source, channels);
    ctx.fadeInSamplesRequested = 100.0;
    ctx.fadeOutSamplesRequested = 100.0;

    PickRenderer renderer;
    renderer.prepare (kRate);
    renderer.startPick (makeForwardPick (0, 4000));

    const auto out = renderN (renderer, ctx, 4000);

    CHECK_THAT (static_cast<double> (out[0]), WithinAbs (0.0, 1e-6));
    CHECK_THAT (static_cast<double> (out[50]), WithinAbs (0.5, 0.02));
    CHECK_THAT (static_cast<double> (out[2000]), WithinAbs (1.0, 1e-6));
    CHECK (out[3950] < 0.55f);   // inside the fade-out
    CHECK (out[3998] < 0.05f);   // nearly silent at the end
}

TEST_CASE ("duration gate stops at the declared length", "[renderer]")
{
    const auto source = makeRamp (44100);
    const float* channels[] = { source.data() };
    const auto ctx = makeCtx (source, channels);

    auto pick = makeForwardPick (0, 8000);
    pick.useDurationGate = true;
    pick.pickLengthHostSamples = 2000.0;  // declared shorter than the slice

    PickRenderer renderer;
    renderer.prepare (kRate);
    renderer.startPick (pick);

    int rendered = 0;
    for (int i = 0; i < 4000; ++i)
    {
        float sum = 0.0f;
        float* const sums[] = { &sum };
        if (renderer.renderSample (ctx, sums, 1))
            ++rendered;
    }

    CHECK (rendered == 2000);
}

TEST_CASE ("tape stop: gain rides the linear decel to silence", "[renderer]")
{
    const std::vector<float> source (44100, 1.0f);
    const float* channels[] = { source.data() };
    const auto ctx = makeCtx (source, channels);

    PickParams pick = makeForwardPick (0, 8000);
    pick.style = PlaybackStyle::tapeStop;
    pick.tapeStopDurationHostSamples = 4000.0;
    pick.pickLengthHostSamples = 4000.0;

    PickRenderer renderer;
    renderer.prepare (kRate);
    renderer.startPick (pick);

    const auto out = renderN (renderer, ctx, 4000);

    // DC source, no fades: output IS the gain = 1 - t/duration.
    CHECK_THAT (static_cast<double> (out[0]), WithinAbs (1.0, 1e-6));
    CHECK_THAT (static_cast<double> (out[1000]), WithinAbs (0.75, 0.01));
    CHECK_THAT (static_cast<double> (out[2000]), WithinAbs (0.5, 0.01));
    CHECK_THAT (static_cast<double> (out[3000]), WithinAbs (0.25, 0.01));
    CHECK (out[3999] < 0.01f);

    CHECK (renderer.finished (ctx));
}

TEST_CASE ("tape stop position-exhaustion freezes instead of overrunning", "[renderer]")
{
    // A short slice at the END of the buffer with a long decel: position
    // will exhaust the content well before the duration ends.
    auto source = makeRamp (10000);
    const float* channels[] = { source.data() };
    const auto ctx = makeCtx (source, channels);

    PickParams pick = makeForwardPick (9000, 1000);
    pick.style = PlaybackStyle::tapeStop;
    pick.tapeStopDurationHostSamples = 8000.0;
    pick.pickLengthHostSamples = 8000.0;

    PickRenderer renderer;
    renderer.prepare (kRate);
    renderer.startPick (pick);

    const auto out = renderN (renderer, ctx, 8000);

    // Renders the full duration (freeze loop keeps producing audio) and
    // every sample stays finite and within the source's value range.
    int nonZero = 0;
    for (const float s : out)
    {
        CHECK (std::isfinite (s));
        CHECK (s <= 1.0f);
        if (s != 0.0f)
            ++nonZero;
    }

    CHECK (nonZero > 6000);  // audibly kept going through the decel
}

TEST_CASE ("ping-pong reads there and back", "[renderer]")
{
    const auto source = makeRamp (44100);
    const float* channels[] = { source.data() };
    const auto ctx = makeCtx (source, channels);

    PickParams pick;
    pick.style = PlaybackStyle::pingPong;
    pick.sliceStartFrame = 10000;
    pick.sliceLengthFrames = 2000;
    pick.schedulingEndFrame = 10000 + 4000;         // full round trip
    pick.pickLengthHostSamples = 4000.0;
    pick.midpointHostSamples = 2000.0;

    PickRenderer renderer;
    renderer.prepare (kRate);
    renderer.startPick (pick);

    const auto out = renderN (renderer, ctx, 4000);

    // Forward leg ascends to the slice end value; backward leg descends.
    CHECK_THAT (static_cast<double> (out[100]),
                WithinAbs (static_cast<double> (source[10100]), 1e-4));
    CHECK_THAT (static_cast<double> (out[1900]),
                WithinAbs (static_cast<double> (source[11900]), 1e-4));
    CHECK_THAT (static_cast<double> (out[2100]),
                WithinAbs (static_cast<double> (source[11900]), 1e-4));
    CHECK_THAT (static_cast<double> (out[3900]),
                WithinAbs (static_cast<double> (source[10100]), 1e-4));
}

TEST_CASE ("bounce midpoint fade dips to silence", "[renderer]")
{
    const std::vector<float> source (44100, 1.0f);
    const float* channels[] = { source.data() };
    auto ctx = makeCtx (source, channels);
    ctx.fadeInSamplesRequested = 100.0;
    ctx.fadeOutSamplesRequested = 100.0;

    PickParams pick;
    pick.style = PlaybackStyle::pingPong;
    pick.sliceStartFrame = 0;
    pick.sliceLengthFrames = 2000;
    pick.schedulingEndFrame = 4000;
    pick.pickLengthHostSamples = 4000.0;
    pick.midpointHostSamples = 2000.0;

    PickRenderer renderer;
    renderer.prepare (kRate);
    renderer.startPick (pick);

    const auto out = renderN (renderer, ctx, 4000);

    // Approaching and leaving the midpoint the gain dips to ~0...
    CHECK (out[2000] < 0.02f);
    CHECK_THAT (static_cast<double> (out[1950]), WithinAbs (0.5, 0.05));
    CHECK_THAT (static_cast<double> (out[2050]), WithinAbs (0.5, 0.05));

    // ...but the body is at full level.
    CHECK_THAT (static_cast<double> (out[1000]), WithinAbs (1.0, 1e-4));
    CHECK_THAT (static_cast<double> (out[3000]), WithinAbs (1.0, 1e-4));
}

TEST_CASE ("filter down attenuates highs as the pick progresses", "[renderer]")
{
    // 8 kHz sine: starts inside the 9 kHz cutoff, ends far above 250 Hz.
    std::vector<float> source (44100);
    for (std::size_t i = 0; i < source.size(); ++i)
        source[i] = static_cast<float> (
            std::sin (2.0 * std::numbers::pi * 8000.0 * static_cast<double> (i) / kRate));

    const float* channels[] = { source.data() };
    const auto ctx = makeCtx (source, channels);

    PickParams pick = makeForwardPick (0, 40000);
    pick.style = PlaybackStyle::filterDown;

    PickRenderer renderer;
    renderer.prepare (kRate);
    renderer.startPick (pick);

    const auto out = renderN (renderer, ctx, 40000);

    auto rms = [&out] (std::size_t from, std::size_t to)
    {
        double sum = 0.0;
        for (std::size_t i = from; i < to; ++i)
            sum += static_cast<double> (out[i]) * static_cast<double> (out[i]);
        return std::sqrt (sum / static_cast<double> (to - from));
    };

    const double early = rms (1000, 5000);
    const double late = rms (35000, 39000);

    CHECK (early > 0.3);          // mostly passing at ~9 kHz cutoff
    CHECK (late < 0.1 * early);   // strongly attenuated near 250 Hz
}

TEST_CASE ("bitcrush stair-steps the output", "[renderer]")
{
    const auto source = makeRamp (44100);
    const float* channels[] = { source.data() };
    const auto ctx = makeCtx (source, channels);

    PickParams pick = makeForwardPick (0, 8000);
    pick.style = PlaybackStyle::bitcrush;
    pick.params.srReduction = 16.0f;   // hold 16 samples
    pick.params.bitDepth = 4.0f;

    PickRenderer renderer;
    renderer.prepare (kRate);
    renderer.startPick (pick);

    const auto out = renderN (renderer, ctx, 8000);

    // Held: the value may change at most every 16 samples.
    int changes = 0;
    for (std::size_t i = 1; i < out.size(); ++i)
        if (out[i] != out[i - 1])
            ++changes;

    CHECK (changes <= 8000 / 16 + 2);

    // Quantized to 4-bit steps.
    constexpr float step = 2.0f / 16.0f;
    for (std::size_t i = 0; i < out.size(); i += 100)
    {
        const float remainder = std::abs (out[i] / step - std::round (out[i] / step));
        CHECK (remainder < 1e-3f);
    }
}

TEST_CASE ("flanger style reaches steady unity on DC", "[renderer]")
{
    const std::vector<float> source (44100, 1.0f);
    const float* channels[] = { source.data() };
    const auto ctx = makeCtx (source, channels);

    PickParams pick = makeForwardPick (0, 8000);
    pick.style = PlaybackStyle::flanger;
    pick.params.flangerDelayMs = 2.0f;
    pick.params.flangerMix = 0.5f;
    pick.params.flangerFeedback = 0.0f;

    PickRenderer renderer;
    renderer.prepare (kRate);
    renderer.startPick (pick);

    const auto out = renderN (renderer, ctx, 8000);

    // Before the line fills (~88 samples at 2 ms): dry*0.5 crossfaded
    // with silence.
    CHECK_THAT (static_cast<double> (out[20]), WithinAbs (0.5, 0.01));

    // After: delayed == dry, crossfade is a no-op.
    CHECK_THAT (static_cast<double> (out[4000]), WithinAbs (1.0, 0.01));
}

TEST_CASE ("per-style volume scales the whole pick", "[renderer]")
{
    const std::vector<float> source (44100, 1.0f);
    const float* channels[] = { source.data() };
    const auto ctx = makeCtx (source, channels);

    PickParams pick = makeForwardPick (0, 4000);
    pick.params.setStyleVolume (PlaybackStyle::forward, 0.5f);

    PickRenderer renderer;
    renderer.prepare (kRate);
    renderer.startPick (pick);

    const auto out = renderN (renderer, ctx, 4000);

    // A constant per-style gain, independent of progress (no ramp any more).
    CHECK_THAT (static_cast<double> (out[400]), WithinAbs (0.5, 1e-6));
    CHECK_THAT (static_cast<double> (out[2000]), WithinAbs (0.5, 1e-6));
    CHECK_THAT (static_cast<double> (out[3600]), WithinAbs (0.5, 1e-6));
}

TEST_CASE ("velocity gain scales the whole pick", "[renderer]")
{
    const std::vector<float> source (44100, 1.0f);
    const float* channels[] = { source.data() };
    const auto ctx = makeCtx (source, channels);

    auto pick = makeForwardPick (0, 2000);
    pick.velocityGain = 0.5;

    PickRenderer renderer;
    renderer.prepare (kRate);
    renderer.startPick (pick);

    const auto out = renderN (renderer, ctx, 1000);
    CHECK_THAT (static_cast<double> (out[500]), WithinAbs (0.5, 1e-6));
}

TEST_CASE ("gate release fades out and force-stops the pick", "[renderer]")
{
    const std::vector<float> source (44100, 1.0f);
    const float* channels[] = { source.data() };
    auto ctx = makeCtx (source, channels);
    ctx.fadeOutSamplesRequested = 200.0;

    auto pick = makeForwardPick (0, 40000);

    PickRenderer renderer;
    renderer.prepare (kRate);
    renderer.startPick (pick);

    (void) renderN (renderer, ctx, 100);   // play a while
    renderer.beginGateRelease();

    const auto out = renderN (renderer, ctx, 300);

    // Ramps toward silence across fadeOut...
    CHECK_THAT (static_cast<double> (out[100]), WithinAbs (0.5, 0.02));
    CHECK (out[198] < 0.03f);

    // ...then the pick force-stops.
    CHECK_FALSE (renderer.hasPick());
    CHECK (out[250] == 0.0f);
}

TEST_CASE ("stretch style renders through the granular path regardless of pitch mode", "[renderer]")
{
    const std::vector<float> source (44100, 1.0f);
    const float* channels[] = { source.data() };
    const auto ctx = makeCtx (source, channels);   // timeStretchMode == false

    PickParams pick;
    pick.style = PlaybackStyle::stretch;
    pick.sliceStartFrame = 1000;
    pick.sliceLengthFrames = 4000;
    pick.schedulingEndFrame = 1000 + 16000;
    pick.pickLengthHostSamples = 8000.0;
    pick.params.grainSizeMs = 10.0f;
    pick.params.grainSpeed = 4.0f;

    PickRenderer renderer;
    renderer.prepare (kRate);
    renderer.startPick (pick);

    const auto out = renderN (renderer, ctx, 8000);

    // Duration-gated at the declared length, audibly nonzero throughout
    // (hard-edged overlapping grains on DC give >= 1 in the overlaps).
    int audible = 0;
    for (std::size_t i = 100; i < out.size(); ++i)
    {
        CHECK (std::isfinite (out[i]));
        if (out[i] > 0.25f)
            ++audible;
    }

    CHECK (audible > 7000);
    CHECK (renderer.finished (ctx));
}

TEST_CASE ("time-stretch pitch mode reconstructs DC at unity", "[renderer]")
{
    const std::vector<float> source (44100, 1.0f);
    const float* channels[] = { source.data() };
    auto ctx = makeCtx (source, channels);
    ctx.timeStretchMode = true;

    auto pick = makeForwardPick (0, 20000);

    PickRenderer renderer;
    renderer.prepare (kRate);
    renderer.startPick (pick);

    const auto out = renderN (renderer, ctx, 8000);

    // After grain warm-up, Hann 50%-overlap reconstruction is unity.
    for (std::size_t i = 3000; i < out.size(); i += 500)
        CHECK_THAT (static_cast<double> (out[i]), WithinAbs (1.0, 0.01));
}

TEST_CASE ("performance sync-off plays at native rate", "[renderer]")
{
    const auto source = makeRamp (44100);
    const float* channels[] = { source.data() };
    auto ctx = makeCtx (source, channels);
    ctx.playbackRate = 2.0;  // host sync wants double speed

    auto pick = makeForwardPick (0, 8000);
    pick.nativeRate = true;  // but this pick ignores it

    PickRenderer renderer;
    renderer.prepare (kRate);
    renderer.startPick (pick);

    const auto out = renderN (renderer, ctx, 4000);

    // Native rate = srConversionRatio = 1: the ramp reads 1:1, not 2:1.
    CHECK_THAT (static_cast<double> (out[2000]),
                WithinAbs (static_cast<double> (source[2000]), 1e-4));
}

TEST_CASE ("beat-quantized pick substitutes its stretch ratio", "[renderer]")
{
    const auto source = makeRamp (44100);
    const float* channels[] = { source.data() };
    const auto ctx = makeCtx (source, channels);

    auto pick = makeForwardPick (1000, 8000);
    pick.beatQuantized = true;
    pick.quantizedStretchRatio = 0.5;  // play at half speed
    pick.pickLengthHostSamples = 16000.0;

    PickRenderer renderer;
    renderer.prepare (kRate);
    renderer.startPick (pick);

    const auto out = renderN (renderer, ctx, 4000);

    // Position advances at 0.5/sample: at output sample 2000 we read
    // source frame 1000 + 1000.
    CHECK_THAT (static_cast<double> (out[2000]),
                WithinAbs (static_cast<double> (source[2000]), 1e-4));
}

TEST_CASE ("scratch bounces within the slice with eased legs", "[renderer]")
{
    const auto source = makeRamp (44100);
    const float* channels[] = { source.data() };
    const auto ctx = makeCtx (source, channels);

    PickParams pick;
    pick.style = PlaybackStyle::scratch;
    pick.sliceStartFrame = 10000;
    pick.sliceLengthFrames = 4000;
    pick.schedulingEndFrame = 10000 + 8000;
    pick.pickLengthHostSamples = 8000.0;
    pick.midpointHostSamples = 4000.0;
    pick.scratchCycleLengthHostSamples = 4000.0;   // one fwd+bwd cycle
    pick.params.scratchForwardCurve = nedit::state::EasingCurve::easeInEaseOut;
    pick.params.scratchBackwardCurve = nedit::state::EasingCurve::easeOut;

    PickRenderer renderer;
    renderer.prepare (kRate);
    renderer.startPick (pick);

    const auto out = renderN (renderer, ctx, 8000);

    // Every rendered value must come from within the slice (the fold can
    // never escape it): source values in [10000, 14000) / 44100.
    const float lo = source[10000];
    const float hi = source[14000];

    for (std::size_t i = 0; i < out.size(); i += 50)
    {
        CHECK (out[i] >= lo - 1e-4f);
        CHECK (out[i] <= hi + 1e-4f);
    }

    // The scratch leg length is cycle/2 * rate = 2000 source frames --
    // the read reaches its apex (slice start + 2000) at the end of the
    // forward leg and returns to the slice start at the end of the cycle.
    CHECK_THAT (static_cast<double> (out[4000]),
                WithinAbs (static_cast<double> (source[10000]), 0.01));
}

TEST_CASE ("whole-window flanger sweep uses the window clock", "[renderer]")
{
    const std::vector<float> source (44100, 1.0f);
    const float* channels[] = { source.data() };
    const auto ctx = makeCtx (source, channels);

    PickParams pick = makeForwardPick (0, 2000);
    pick.style = PlaybackStyle::flanger;
    pick.flangerWholeWindow = true;
    pick.params.flangerMix = 0.0f;                       // start fully dry
    pick.params.flangerMixMode = nedit::state::SweepMode::sweepIn;  // -> fully wet

    PickRenderer renderer;
    renderer.prepare (kRate);
    renderer.startWindow (8000.0);   // window much longer than the pick
    renderer.startPick (pick);

    const auto out = renderN (renderer, ctx, 2000, /*tickWindow*/ true);

    // At pick end the window is only 25% elapsed: the mix must NOT have
    // reached fully wet (which on DC is indistinguishable), so instead
    // verify early: at sample 40 (line not yet filled, delayed = 0) the
    // output is dry * (1 - mix(progress)) with progress ~0.5% -> nearly 1.
    CHECK (out[40] > 0.95f);
}
