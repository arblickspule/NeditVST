#include <doctest/doctest.h>
#include "TransientDetector.h"

namespace
{
    juce::AudioBuffer<float> makeBuffer (int numSamples)
    {
        juce::AudioBuffer<float> buffer (1, numSamples);
        buffer.clear();
        return buffer;
    }

    // A short rectangular burst: the envelope follower's fast attack gives
    // its rising edge the largest derivative in the buffer, which is exactly
    // where detectSlices() should place the onset.
    void addBurst (juce::AudioBuffer<float>& buffer, int at, int width = 5, float amplitude = 1.0f)
    {
        for (int i = 0; i < width; ++i)
            buffer.setSample (0, at + i, amplitude);
    }

    TransientDetector analyzeBuffer (const juce::AudioBuffer<float>& buffer)
    {
        TransientDetector detector;
        detector.analyze (buffer, 44100.0);
        return detector;
    }
}

TEST_CASE ("TransientDetector with no analysis")
{
    TransientDetector detector;

    CHECK_FALSE (detector.hasAnalysis());
    CHECK (detector.getAnalyzedLengthInSamples() == 0);
    CHECK (detector.detectSlices (0.5f, 10.0f, 0, 100).empty());
    CHECK (detector.findNearestPeak (50, 500) == 50);

    // Analyzing an empty buffer is a no-op, not a crash.
    detector.analyze (juce::AudioBuffer<float>(), 44100.0);
    CHECK_FALSE (detector.hasAnalysis());
    CHECK (detector.detectSlices (0.5f, 10.0f).empty());
}

TEST_CASE ("TransientDetector silence yields one slice covering the range")
{
    auto buffer = makeBuffer (44100);
    auto detector = analyzeBuffer (buffer);

    CHECK (detector.hasAnalysis());
    CHECK (detector.getAnalyzedLengthInSamples() == 44100);

    // All-zero derivative: no onsets at any sensitivity, so the whole range
    // collapses to a single slice -- even at sensitivity 1.
    for (float sensitivity : { 0.0f, 0.5f, 1.0f })
    {
        auto slices = detector.detectSlices (sensitivity, 10.0f);
        REQUIRE (slices.size() == 1);
        CHECK (slices[0].startSample == 0);
        CHECK (slices[0].endSample == 44100);
    }

    // A trimmed range confines the single slice to the range.
    auto trimmed = detector.detectSlices (0.5f, 10.0f, 5000, 20000);
    REQUIRE (trimmed.size() == 1);
    CHECK (trimmed[0].startSample == 5000);
    CHECK (trimmed[0].endSample == 20000);
}

TEST_CASE ("TransientDetector burst places a boundary at its rising edge")
{
    auto buffer = makeBuffer (44100);
    addBurst (buffer, 10000);
    auto detector = analyzeBuffer (buffer);

    auto slices = detector.detectSlices (1.0f, 10.0f);
    REQUIRE (slices.size() == 2);
    CHECK (slices[0].startSample == 0);
    CHECK (slices[0].endSample == 10000);
    CHECK (slices[1].startSample == 10000);
    CHECK (slices[1].endSample == 44100);

    // Sensitivity 0 is guaranteed zero transients regardless of content.
    auto none = detector.detectSlices (0.0f, 10.0f);
    REQUIRE (none.size() == 1);
    CHECK (none[0].startSample == 0);
    CHECK (none[0].endSample == 44100);
}

TEST_CASE ("TransientDetector holdoff suppresses onsets closer than the gap")
{
    auto buffer = makeBuffer (44100);
    addBurst (buffer, 10000);
    addBurst (buffer, 11000);
    auto detector = analyzeBuffer (buffer);

    // 100ms holdoff = 4410 samples > the 1000-sample gap: second burst dies.
    auto wide = detector.detectSlices (1.0f, 100.0f);
    REQUIRE (wide.size() == 2);
    CHECK (wide[1].startSample == 10000);

    // 10ms holdoff = 441 samples < the gap: both bursts are detected.
    auto tight = detector.detectSlices (1.0f, 10.0f);
    REQUIRE (tight.size() == 3);
    CHECK (tight[1].startSample == 10000);
    CHECK (tight[1].endSample == 11000);
    CHECK (tight[2].startSample == 11000);
}

TEST_CASE ("TransientDetector mono-sums multichannel buffers")
{
    juce::AudioBuffer<float> stereo (2, 44100);
    stereo.clear();
    addBurst (stereo, 10000);
    addBurst (stereo, 10000);

    TransientDetector detector;
    detector.analyze (stereo, 44100.0);

    // Both channels carry the burst, so the mono sum matches the mono case
    // exactly (each channel is scaled by 1/numChannels).
    auto slices = detector.detectSlices (1.0f, 10.0f);
    REQUIRE (slices.size() == 2);
    CHECK (slices[1].startSample == 10000);
}

TEST_CASE ("TransientDetector findNearestPeak")
{
    auto buffer = makeBuffer (44100);
    addBurst (buffer, 10000);
    auto detector = analyzeBuffer (buffer);

    // Snaps a nearby target onto the burst's rising edge.
    CHECK (detector.findNearestPeak (10100, 500) == 10000);
    CHECK (detector.findNearestPeak (9800, 500) == 10000);

    // No transient within the search radius: the flat derivative makes the
    // strongest candidate the left edge of the search window, but never
    // outside it -- pin the window bounds rather than an exact position.
    const int flat = detector.findNearestPeak (5000, 1000);
    CHECK (flat >= 4000);
    CHECK (flat <= 6000);
}
