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
    void addBurst (juce::AudioBuffer<float>& buffer, int at, int width = 5, float amplitude = 1.0f, int channel = 0)
    {
        for (int i = 0; i < width; ++i)
            buffer.setSample (channel, at + i, amplitude);
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
    // Burst on the SECOND channel only, first channel silent: detection has
    // to sum across all channels (not just read channel 0) to find it. If it
    // ignored channel 1 the derivative would stay flat and collapse to a
    // single slice.
    juce::AudioBuffer<float> rightOnly (2, 44100);
    rightOnly.clear();
    addBurst (rightOnly, 10000, 5, 1.0f, /* channel */ 1);

    TransientDetector rightDetector;
    rightDetector.analyze (rightOnly, 44100.0);

    auto rightSlices = rightDetector.detectSlices (1.0f, 10.0f);
    REQUIRE (rightSlices.size() == 2);
    CHECK (rightSlices[0].startSample == 0);
    CHECK (rightSlices[1].startSample == 10000);

    // The same burst on BOTH channels: the mono sum is (|L| + |R|) / 2, so
    // two equal bursts sum right back to a unit-amplitude envelope and
    // detect the identical single boundary.
    juce::AudioBuffer<float> both (2, 44100);
    both.clear();
    addBurst (both, 10000, 5, 1.0f, 0);
    addBurst (both, 10000, 5, 1.0f, 1);

    TransientDetector bothDetector;
    bothDetector.analyze (both, 44100.0);

    auto bothSlices = bothDetector.detectSlices (1.0f, 10.0f);
    REQUIRE (bothSlices.size() == 2);
    CHECK (bothSlices[1].startSample == 10000);
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
