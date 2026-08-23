// TransientDetector on synthetic signals with transients at known frames.

#include <catch2/catch_test_macros.hpp>

#include <engine/TransientDetector.h>

#include "TestSignals.h"

#include <cstdlib>

using namespace nedit::engine;
using nedit::test::makeClickTrack;

namespace {

constexpr double kRate = 44100.0;

TransientDetector analyzedDetector (const std::vector<float>& mono, double sampleRate = kRate)
{
    TransientDetector detector;
    const float* channels[] = { mono.data() };
    detector.analyze (channels, 1, static_cast<std::int64_t> (mono.size()), sampleRate);
    return detector;
}

// True if some slice starts within `tolerance` frames of `frame`.
bool hasBoundaryNear (const std::vector<Slice>& slices, std::int64_t frame,
                      std::int64_t tolerance = 64)
{
    for (const auto& slice : slices)
        if (std::llabs (slice.startFrame - frame) <= tolerance)
            return true;

    return false;
}

} // namespace

TEST_CASE ("no analysis -> no slices", "[detector]")
{
    const TransientDetector detector;

    CHECK_FALSE (detector.hasAnalysis());
    CHECK (detector.detectSlices (0.5f, 30.0f).empty());
    CHECK (detector.findNearestPeak (100, 50) == 100);
}

TEST_CASE ("empty buffer is handled", "[detector]")
{
    TransientDetector detector;
    detector.analyze (nullptr, 0, 0, kRate);

    CHECK_FALSE (detector.hasAnalysis());
    CHECK (detector.detectSlices (1.0f, 1.0f).empty());
}

TEST_CASE ("clicks at known positions are detected", "[detector]")
{
    const std::vector<std::int64_t> clicks { 4410, 22050, 44100, 66150 };
    const auto audio = makeClickTrack (88200, clicks, kRate);
    const auto detector = analyzedDetector (audio);

    const auto slices = detector.detectSlices (0.5f, 30.0f);

    // Position 0 is always a boundary; each click adds one.
    REQUIRE (slices.size() == clicks.size() + 1);
    CHECK (slices.front().startFrame == 0);

    for (const auto click : clicks)
        CHECK (hasBoundaryNear (slices, click));

    // Slices tile the range exactly: contiguous, ending at the buffer end.
    for (std::size_t i = 1; i < slices.size(); ++i)
        CHECK (slices[i].startFrame == slices[i - 1].endFrame);

    CHECK (slices.back().endFrame == 88200);
}

TEST_CASE ("sensitivity zero yields exactly one whole-range slice", "[detector]")
{
    const auto audio = makeClickTrack (44100, { 11025, 22050 }, kRate);
    const auto detector = analyzedDetector (audio);

    const auto slices = detector.detectSlices (0.0f, 30.0f);

    REQUIRE (slices.size() == 1);
    CHECK (slices.front() == Slice { 0, 44100 });
}

TEST_CASE ("higher sensitivity finds at least as many onsets", "[detector]")
{
    // Two loud clicks and two quieter ones. The quiet amplitude must sit
    // above the noise floor (the mean positive derivative -- sensitivity
    // 1.0's hard lower bound) but below the low-sensitivity threshold.
    auto audio = makeClickTrack (88200, { 10000, 50000 }, kRate, 1.0f);
    const auto quiet = makeClickTrack (88200, { 30000, 70000 }, kRate, 0.4f);

    for (std::size_t i = 0; i < audio.size(); ++i)
        audio[i] += quiet[i];

    const auto detector = analyzedDetector (audio);

    const auto low = detector.detectSlices (0.35f, 30.0f);
    const auto high = detector.detectSlices (1.0f, 30.0f);

    CHECK (high.size() >= low.size());

    // The loud clicks are found even at low sensitivity...
    CHECK (hasBoundaryNear (low, 10000));
    CHECK (hasBoundaryNear (low, 50000));

    // ...the quiet ones only at high sensitivity.
    CHECK_FALSE (hasBoundaryNear (low, 30000));
    CHECK (hasBoundaryNear (high, 30000));
    CHECK (hasBoundaryNear (high, 70000));
}

TEST_CASE ("holdoff suppresses double triggers", "[detector]")
{
    // Two clicks 10ms apart (441 frames), then one far away.
    const auto audio = makeClickTrack (88200, { 10000, 10441, 60000 }, kRate);
    const auto detector = analyzedDetector (audio);

    // 30ms holdoff: the second click of the pair must be swallowed.
    const auto slices = detector.detectSlices (0.9f, 30.0f);

    CHECK (hasBoundaryNear (slices, 10000));
    CHECK_FALSE (hasBoundaryNear (slices, 10441, 100));
    CHECK (hasBoundaryNear (slices, 60000));

    // 5ms holdoff: both clicks of the pair are detected.
    const auto slicesShort = detector.detectSlices (0.9f, 5.0f);
    CHECK (hasBoundaryNear (slicesShort, 10000));
    CHECK (hasBoundaryNear (slicesShort, 10441, 100));
}

TEST_CASE ("trim range confines detection and output", "[detector]")
{
    const std::vector<std::int64_t> clicks { 5000, 30000, 60000, 80000 };
    const auto audio = makeClickTrack (88200, clicks, kRate);
    const auto detector = analyzedDetector (audio);

    const auto slices = detector.detectSlices (0.8f, 30.0f, 20000, 70000);

    REQUIRE (! slices.empty());

    // First boundary is exactly the range start; last slice ends at range end.
    CHECK (slices.front().startFrame == 20000);
    CHECK (slices.back().endFrame == 70000);

    // In-range clicks found, out-of-range clicks never leak in.
    CHECK (hasBoundaryNear (slices, 30000));
    CHECK (hasBoundaryNear (slices, 60000));
    CHECK_FALSE (hasBoundaryNear (slices, 5000, 1000));
    CHECK_FALSE (hasBoundaryNear (slices, 80000, 1000));

    for (const auto& slice : slices)
    {
        CHECK (slice.startFrame >= 20000);
        CHECK (slice.endFrame <= 70000);
    }
}

TEST_CASE ("findNearestPeak snaps to the strongest nearby transient", "[detector]")
{
    const auto audio = makeClickTrack (88200, { 40000 }, kRate);
    const auto detector = analyzedDetector (audio);

    // A click 500 frames off target, search radius 2205 (50ms).
    const auto snapped = detector.findNearestPeak (39500, 2205);
    CHECK (std::llabs (snapped - 40000) <= 4);

    // Nothing within radius: the target itself (clamped) comes back --
    // the strongest "peak" in silence is arbitrary but must stay in range.
    const auto far = detector.findNearestPeak (10000, 500);
    CHECK (far >= 9500);
    CHECK (far <= 10500);
}

TEST_CASE ("findNearestPeak respects the trim range", "[detector]")
{
    const auto audio = makeClickTrack (88200, { 40000, 50000 }, kRate);
    const auto detector = analyzedDetector (audio);

    // Target near the 40000 click, but the range only allows [45000, 88200).
    const auto snapped = detector.findNearestPeak (44000, 44100, 45000, 88200);
    CHECK (std::llabs (snapped - 50000) <= 4);
    CHECK (snapped >= 45000);
}

TEST_CASE ("stereo input is mono-summed for detection", "[detector]")
{
    // Click only in the right channel -- must still be detected.
    const auto left = std::vector<float> (44100, 0.0f);
    const auto right = makeClickTrack (44100, { 20000 }, kRate);

    TransientDetector detector;
    const float* channels[] = { left.data(), right.data() };
    detector.analyze (channels, 2, 44100, kRate);

    const auto slices = detector.detectSlices (0.7f, 30.0f);
    CHECK (hasBoundaryNear (slices, 20000));
}
