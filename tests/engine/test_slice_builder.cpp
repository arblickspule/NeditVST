// SliceBuilder: merging auto onsets with manual/excluded points, grid
// quantize, trim handling.

#include <catch2/catch_test_macros.hpp>

#include <engine/SliceBuilder.h>
#include <engine/Tempo.h>

#include "TestSignals.h"

#include <cstdlib>

using namespace nedit::engine;
using nedit::state::SampleState;

namespace {

constexpr double kRate = 44100.0;

// A sample state for a 2-second, full-trim document.
SampleState makeSample()
{
    SampleState sample;
    sample.samplePath = "/tmp/test.wav";
    sample.sampleLengthFrames = 88200;
    sample.sampleSampleRate = kRate;
    sample.trimStartFrame = 0;
    sample.trimEndFrame = 88200;
    return sample;
}

std::vector<Slice> autoSlices (const std::vector<std::int64_t>& onsets,
                               std::int64_t trimStart, std::int64_t trimEnd)
{
    std::vector<Slice> slices;
    std::vector<std::int64_t> boundaries;

    if (onsets.empty() || onsets.front() > trimStart)
        boundaries.push_back (trimStart);

    boundaries.insert (boundaries.end(), onsets.begin(), onsets.end());

    for (std::size_t i = 0; i < boundaries.size(); ++i)
        slices.push_back ({ boundaries[i],
                            i + 1 < boundaries.size() ? boundaries[i + 1] : trimEnd });

    return slices;
}

} // namespace

TEST_CASE ("plain merge: auto onsets become slices tiling the trim", "[slicebuilder]")
{
    const auto sample = makeSample();
    const auto merged = mergeOnsetsIntoSlices (autoSlices ({ 20000, 50000 }, 0, 88200), sample);

    REQUIRE (merged.size() == 3);
    CHECK (merged[0] == Slice { 0, 20000 });
    CHECK (merged[1] == Slice { 20000, 50000 });
    CHECK (merged[2] == Slice { 50000, 88200 });
}

TEST_CASE ("excluded points remove nearby auto onsets", "[slicebuilder]")
{
    auto sample = makeSample();

    // Within the 50ms (2205-frame) tolerance of the 50000 onset.
    sample.excludedPoints = { { 1, 51000 } };

    const auto merged = mergeOnsetsIntoSlices (autoSlices ({ 20000, 50000 }, 0, 88200), sample);

    REQUIRE (merged.size() == 2);
    CHECK (merged[0] == Slice { 0, 20000 });
    CHECK (merged[1] == Slice { 20000, 88200 });
}

TEST_CASE ("exclusion outside the tolerance does nothing", "[slicebuilder]")
{
    auto sample = makeSample();
    sample.excludedPoints = { { 1, 55000 } };  // 5000 frames away > 2205

    const auto merged = mergeOnsetsIntoSlices (autoSlices ({ 20000, 50000 }, 0, 88200), sample);
    CHECK (merged.size() == 3);
}

TEST_CASE ("the trim start can never be excluded", "[slicebuilder]")
{
    auto sample = makeSample();
    sample.excludedPoints = { { 1, 0 } };

    const auto merged = mergeOnsetsIntoSlices (autoSlices ({}, 0, 88200), sample);

    REQUIRE (merged.size() == 1);
    CHECK (merged[0] == Slice { 0, 88200 });
}

TEST_CASE ("manual points are merged and sorted in", "[slicebuilder]")
{
    auto sample = makeSample();
    sample.manualPoints = { { 1, 70000 }, { 2, 10000 } };

    const auto merged = mergeOnsetsIntoSlices (autoSlices ({ 40000 }, 0, 88200), sample);

    REQUIRE (merged.size() == 4);
    CHECK (merged[0] == Slice { 0, 10000 });
    CHECK (merged[1] == Slice { 10000, 40000 });
    CHECK (merged[2] == Slice { 40000, 70000 });
    CHECK (merged[3] == Slice { 70000, 88200 });
}

TEST_CASE ("manual points outside the trim are soft-excluded", "[slicebuilder]")
{
    auto sample = makeSample();
    sample.trimStartFrame = 20000;
    sample.trimEndFrame = 60000;
    sample.manualPoints = { { 1, 10000 },   // before trim -- filtered
                            { 2, 40000 },   // inside -- kept
                            { 3, 70000 } }; // after trim -- filtered

    const auto merged = mergeOnsetsIntoSlices (autoSlices ({}, 20000, 60000), sample);

    REQUIRE (merged.size() == 2);
    CHECK (merged[0] == Slice { 20000, 40000 });
    CHECK (merged[1] == Slice { 40000, 60000 });

    // ...and the state itself was not touched (soft exclude).
    CHECK (sample.manualPoints.size() == 3);
}

TEST_CASE ("duplicate boundaries collapse", "[slicebuilder]")
{
    auto sample = makeSample();
    sample.manualPoints = { { 1, 40000 } };  // same position as an auto onset

    const auto merged = mergeOnsetsIntoSlices (autoSlices ({ 40000 }, 0, 88200), sample);

    REQUIRE (merged.size() == 2);
    CHECK (merged[0] == Slice { 0, 40000 });
    CHECK (merged[1] == Slice { 40000, 88200 });
}

TEST_CASE ("grid quantize snaps auto onsets but never manual points", "[slicebuilder]")
{
    auto sample = makeSample();

    // 1 bar over 88200 frames at 44.1k = 2s => 120 BPM; 4n grid = 22050 frames.
    sample.loopLengthBars = 1;
    sample.quantizeTransients = true;
    sample.quantizeGridIndex = nedit::state::kNoteValue4n;
    sample.manualPoints = { { 1, 67000 } };  // deliberately off-grid

    // Auto onset at 23000 -- nearest 4n line is 22050.
    const auto merged = mergeOnsetsIntoSlices (autoSlices ({ 23000 }, 0, 88200), sample);

    REQUIRE (merged.size() == 3);
    CHECK (merged[0] == Slice { 0, 22050 });        // quantized auto onset
    CHECK (merged[1] == Slice { 22050, 67000 });    // manual point untouched
    CHECK (merged[2] == Slice { 67000, 88200 });
}

TEST_CASE ("exclusion matches the raw position even with quantize on", "[slicebuilder]")
{
    auto sample = makeSample();
    sample.loopLengthBars = 1;
    sample.quantizeTransients = true;
    sample.quantizeGridIndex = nedit::state::kNoteValue4n;

    // The user clicked the raw detected peak at 23000 to exclude it.
    // Even though quantize would have moved it to 22050, the exclusion
    // must match against the raw position.
    sample.excludedPoints = { { 1, 23000 } };

    const auto merged = mergeOnsetsIntoSlices (autoSlices ({ 23000 }, 0, 88200), sample);

    REQUIRE (merged.size() == 1);
    CHECK (merged[0] == Slice { 0, 88200 });
}

TEST_CASE ("full pipeline: detector + merge on synthetic audio", "[slicebuilder]")
{
    const auto audio = nedit::test::makeClickTrack (88200, { 22050, 44100, 66150 }, kRate);

    TransientDetector detector;
    const float* channels[] = { audio.data() };
    detector.analyze (channels, 1, 88200, kRate);

    auto sample = makeSample();
    sample.sensitivity = 0.6f;
    sample.excludedPoints = { { 1, 44100 } };  // kill the middle click
    sample.manualPoints = { { 1, 55000 } };    // add one by hand

    const auto slices = buildSlices (detector, sample);

    REQUIRE (slices.size() == 4);
    CHECK (slices[0].startFrame == 0);
    CHECK (std::llabs (slices[1].startFrame - 22050) <= 64);
    CHECK (slices[2].startFrame == 55000);          // manual, exact
    CHECK (std::llabs (slices[3].startFrame - 66150) <= 64);
    CHECK (slices.back().endFrame == 88200);
}

// --- filterGhostSlices: issue #5 (silent/empty sequencer rows) ------------

// Audio with steady content across the whole document (so RMS of any real
// slice is far above the 5% silent-tail threshold).
std::vector<float> steadyAudio (std::int64_t frames)
{
    std::vector<float> a (static_cast<std::size_t> (frames), 0.0f);
    for (std::int64_t i = 0; i < frames; ++i)
        a[static_cast<std::size_t> (i)] = (i / 1200) % 2 == 0 ? 1.0f : 0.0f;
    return a;
}

const float* pointerTo (const std::vector<float>& v)
{
    return v.data();
}

TEST_CASE ("ghost filter: a sub-15ms leading sliver merges into the next slice", "[slicebuilder]")
{
    const auto audio = steadyAudio (48000);
    const float* channels[] = { pointerTo (audio) };

    // [0,30) is the detector's range-start prepend artifact (~0.7ms at
    // 44.1k); the real first onset is at 30. Merging must produce a slice
    // tiling [0, trimEnd) with the attack frames retained.
    const std::vector<Slice> raw = { { 0, 30 }, { 30, 24000 }, { 24000, 48000 } };

    const auto filtered = filterGhostSlices (raw, channels, 1, 0, 48000, kRate, nullptr);

    REQUIRE (filtered.size() == 2);
    CHECK (filtered[0] == Slice { 0, 24000 });   // sliver merged: tiles the trim
    CHECK (filtered[1] == Slice { 24000, 48000 });
}

TEST_CASE ("ghost filter: a leading slice at or beyond 15ms is a real slice, kept", "[slicebuilder]")
{
    const auto audio = steadyAudio (48000);
    const float* channels[] = { pointerTo (audio) };

    // 30ms = 1323 frames > kLeadingSliverMaxMs -> genuine boundary.
    const std::vector<Slice> raw = { { 0, 1323 }, { 1323, 24000 }, { 24000, 48000 } };

    const auto filtered = filterGhostSlices (raw, channels, 1, 0, 48000, kRate, nullptr);
    CHECK (filtered == raw);
}

TEST_CASE ("ghost filter: a manual boundary inside the sliver keeps it", "[slicebuilder]")
{
    const auto audio = steadyAudio (48000);
    const float* channels[] = { pointerTo (audio) };

    const std::vector<Slice> raw = { { 0, 30 }, { 30, 24000 }, { 24000, 48000 } };

    // The user explicitly placed a boundary at frame 10: the "sliver" is
    // their deliberate beat-start cut, not the auto artifact.
    const std::vector<nedit::state::SamplePoint> manual = { { 7, 10 } };

    const auto filtered = filterGhostSlices (raw, channels, 1, 0, 48000, kRate, &manual);
    CHECK (filtered == raw);
}

TEST_CASE ("ghost filter: an all-silent final tail slice is dropped", "[slicebuilder]")
{
    // Content fills only the first half; [48000,96000) is pure silence
    // (e.g. a loop whose trim was extended past the actual content, or a
    // final bar that is a complete rest).
    std::vector<float> audio (96000, 0.0f);
    for (std::int64_t i = 0; i < 48000; ++i)
        audio[static_cast<std::size_t> (i)] = 1.0f;
    const float* channels[] = { pointerTo (audio) };

    const std::vector<Slice> raw = { { 0, 24000 }, { 24000, 48000 }, { 48000, 96000 } };

    const auto filtered = filterGhostSlices (raw, channels, 1, 0, 96000, kRate, nullptr);

    REQUIRE (filtered.size() == 2);
    CHECK (filtered[0] == Slice { 0, 24000 });
    CHECK (filtered[1] == Slice { 24000, 48000 });   // silent rest: not a row
}

TEST_CASE ("ghost filter: a final slice with real content (snare + rest) is kept", "[slicebuilder]")
{
    // Last slice [48000,96000) opens with a real snare hit (~520ms at 44.1k
    // of ~1.0 amplitude); the rest is silence. It is NOT silent, so rows
    // must keep it -- dropping it would remove an actual beat.
    std::vector<float> audio (96000, 0.0f);
    for (std::int64_t i = 0; i < 48000; ++i)
        audio[static_cast<std::size_t> (i)] = 1.0f;
    for (std::int64_t i = 48000; i < 52000; ++i)
        audio[static_cast<std::size_t> (i)] = 1.0f;
    const float* channels[] = { pointerTo (audio) };

    const std::vector<Slice> raw = { { 0, 24000 }, { 24000, 48000 }, { 48000, 96000 } };

    const auto filtered = filterGhostSlices (raw, channels, 1, 0, 96000, kRate, nullptr);
    CHECK (filtered == raw);
}

TEST_CASE ("ghost filter: degenerate inputs pass through unchanged", "[slicebuilder]")
{
    const auto audio = steadyAudio (48000);
    const float* channels[] = { pointerTo (audio) };

    // Empty slice list.
    CHECK (filterGhostSlices ({}, channels, 1, 0, 48000, kRate, nullptr).empty());

    // No channels / no audio reference.
    const std::vector<Slice> raw = { { 0, 30 }, { 30, 24000 } };
    CHECK (filterGhostSlices (raw, nullptr, 0, 0, 24000, kRate, nullptr) == raw);

    // Single slice spanning the whole trim: never split or touch it.
    const std::vector<Slice> single = { { 0, 48000 } };
    CHECK (filterGhostSlices (single, channels, 1, 0, 48000, kRate, nullptr) == single);
}

TEST_CASE ("ghost filter: never returns an empty list", "[slicebuilder]")
{
    // Content only in [0,30): the leading sliver merges away AND the tail
    // [30,48000) is silent. Dropping both would yield an empty slice list;
    // the tail must be kept so there is always at least one row.
    std::vector<float> audio (48000, 0.0f);
    for (std::int64_t i = 0; i < 30; ++i)
        audio[static_cast<std::size_t> (i)] = 1.0f;
    const float* channels[] = { pointerTo (audio) };

    const std::vector<Slice> raw = { { 0, 30 }, { 30, 48000 } };

    const auto filtered = filterGhostSlices (raw, channels, 1, 0, 48000, kRate, nullptr);

    REQUIRE (filtered.size() == 1);
    CHECK (filtered[0] == Slice { 0, 48000 });   // whole trim, one slice survives
}

TEST_CASE ("ghost filter: stereo audio sums for the silence check", "[slicebuilder]")
{
    // Left channel carries the loud content, right channel is silent.
    // The RMS must be computed over the mono-ish sum, not one channel.
    std::vector<float> left (96000, 0.0f);
    for (std::int64_t i = 0; i < 52000; ++i)
        left[static_cast<std::size_t> (i)] = 1.0f;
    std::vector<float> right (96000, 0.0f);
    const float* channels[] = { left.data(), right.data() };

    const std::vector<Slice> raw = { { 0, 24000 }, { 24000, 48000 }, { 48000, 96000 } };

    // Left has real content in [48000,52000), so the final slice is kept.
    const auto filtered = filterGhostSlices (raw, channels, 2, 0, 96000, kRate, nullptr);
    CHECK (filtered == raw);
}
