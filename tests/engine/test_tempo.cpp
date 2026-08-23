// Tempo math: source span, original BPM, holdoff, repitch ratio, grid
// quantization.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <engine/Tempo.h>

using namespace nedit::engine;
using nedit::state::SampleState;
using Catch::Matchers::WithinRel;

namespace {

// 2 seconds of 44.1k audio, fully trimmed, 1 bar => 120 BPM.
SampleState makeSample()
{
    SampleState sample;
    sample.samplePath = "/tmp/test.wav";
    sample.sampleLengthFrames = 88200;
    sample.sampleSampleRate = 44100.0;
    sample.trimStartFrame = 0;
    sample.trimEndFrame = 88200;
    sample.loopLengthBars = 1;
    return sample;
}

} // namespace

TEST_CASE ("source span from the trimmed range", "[tempo]")
{
    auto sample = makeSample();
    CHECK_THAT (tempo::sourceSpanSeconds (sample), WithinRel (2.0, 1e-9));

    sample.trimStartFrame = 22050;
    sample.trimEndFrame = 66150;
    CHECK_THAT (tempo::sourceSpanSeconds (sample), WithinRel (1.0, 1e-9));
}

TEST_CASE ("source span from the BPM override", "[tempo]")
{
    auto sample = makeSample();
    sample.manualBpmOverrideEnabled = true;
    sample.manualBpmOverrideValue = 150.0;
    sample.loopLengthBars = 2;

    // 8 beats at 150 BPM = 3.2s -- the trim is ignored entirely.
    CHECK_THAT (tempo::sourceSpanSeconds (sample), WithinRel (3.2, 1e-9));
}

TEST_CASE ("original BPM derivation", "[tempo]")
{
    auto sample = makeSample();
    CHECK_THAT (tempo::calculatedOriginalBpm (sample), WithinRel (120.0, 1e-9));

    sample.loopLengthBars = 2;
    CHECK_THAT (tempo::calculatedOriginalBpm (sample), WithinRel (240.0, 1e-9));

    // Override reported verbatim.
    sample.manualBpmOverrideEnabled = true;
    sample.manualBpmOverrideValue = 174.0;
    CHECK (tempo::calculatedOriginalBpm (sample) == 174.0);
}

TEST_CASE ("no sample -> no tempo", "[tempo]")
{
    SampleState sample;  // no path, no frames
    CHECK (tempo::calculatedOriginalBpm (sample) == 0.0);
    CHECK (tempo::sourceSpanSeconds (sample) == 0.0);
}

TEST_CASE ("tempo-relative holdoff is a 32nd note, floored", "[tempo]")
{
    auto sample = makeSample();  // 120 BPM

    // Beat = 500ms; 32nd = 1/8 beat = 62.5ms.
    CHECK_THAT (static_cast<double> (tempo::minimumHoldoffMs (sample)),
                WithinRel (62.5, 1e-6));

    // No tempo -> fixed 30ms fallback.
    SampleState empty;
    CHECK (tempo::minimumHoldoffMs (empty) == tempo::kDefaultHoldoffMs);

    // Absurd override -> floored at 1ms, never 0.
    sample.manualBpmOverrideEnabled = true;
    sample.manualBpmOverrideValue = 100000.0;
    CHECK (tempo::minimumHoldoffMs (sample) >= 1.0f);
}

TEST_CASE ("repitch ratio against the host tempo", "[tempo]")
{
    auto sample = makeSample();  // source: 1 bar in 2s = 120 BPM

    // Host at 120: ratio 1 (source already fits).
    CHECK_THAT (tempo::repitchRatio (sample, 120.0), WithinRel (1.0, 1e-9));

    // Host at 240: host bar lasts 1s, source span 2s -> play 2x faster.
    CHECK_THAT (tempo::repitchRatio (sample, 240.0), WithinRel (2.0, 1e-9));

    // Host at 60: half speed.
    CHECK_THAT (tempo::repitchRatio (sample, 60.0), WithinRel (0.5, 1e-9));

    // Degenerate host tempo: identity.
    CHECK (tempo::repitchRatio (sample, 0.0) == 1.0);
}

TEST_CASE ("playback rate combines SR conversion and repitch", "[tempo]")
{
    auto sample = makeSample();
    sample.sampleSampleRate = 48000.0;
    sample.trimEndFrame = 96000;      // still 2 seconds at 48k
    sample.sampleLengthFrames = 96000;

    // Host 44.1k at 120 BPM: rate = 48000/44100 * 1.0.
    CHECK_THAT (tempo::playbackRate (sample, 44100.0, 120.0),
                WithinRel (48000.0 / 44100.0, 1e-9));

    // Degenerate rates are safe.
    CHECK (tempo::playbackRate (sample, 0.0, 120.0) == 0.0);
}

TEST_CASE ("grid quantization snaps to the nearest line", "[tempo]")
{
    // 120 BPM at 44.1k: 4n = 22050 frames.
    const double bpm = 120.0;
    const double rate = 44100.0;
    const double quarterBeats = 1.0;

    CHECK (tempo::quantizeFrameToGrid (23000, 0, quarterBeats, bpm, rate) == 22050);
    CHECK (tempo::quantizeFrameToGrid (10000, 0, quarterBeats, bpm, rate) == 0);
    CHECK (tempo::quantizeFrameToGrid (12000, 0, quarterBeats, bpm, rate) == 22050);

    // Anchored grids shift with the anchor.
    CHECK (tempo::quantizeFrameToGrid (23000, 1000, quarterBeats, bpm, rate) == 23050);

    // No grid/tempo: identity.
    CHECK (tempo::quantizeFrameToGrid (23000, 0, 0.0, bpm, rate) == 23000);
    CHECK (tempo::quantizeFrameToGrid (23000, 0, quarterBeats, 0.0, rate) == 23000);
}
