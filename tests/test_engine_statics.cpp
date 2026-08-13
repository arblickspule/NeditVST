#include <doctest/doctest.h>
#include "SlicerEngine.h"

using PlaybackStyle = SlicerModel::PlaybackStyle;

TEST_CASE ("SlicerEngine::indexToPlaybackStyle maps indices to enums")
{
    CHECK (SlicerEngine::indexToPlaybackStyle (0) == PlaybackStyle::forward);
    CHECK (SlicerEngine::indexToPlaybackStyle (1) == PlaybackStyle::pingPong);
    CHECK (SlicerEngine::indexToPlaybackStyle (2) == PlaybackStyle::tapeStop);
    CHECK (SlicerEngine::indexToPlaybackStyle (3) == PlaybackStyle::stretch);
    CHECK (SlicerEngine::indexToPlaybackStyle (4) == PlaybackStyle::filterSweepDown);
    CHECK (SlicerEngine::indexToPlaybackStyle (5) == PlaybackStyle::filterSweepUp);
    CHECK (SlicerEngine::indexToPlaybackStyle (6) == PlaybackStyle::bitcrush);
    CHECK (SlicerEngine::indexToPlaybackStyle (7) == PlaybackStyle::scratch);
    CHECK (SlicerEngine::indexToPlaybackStyle (8) == PlaybackStyle::flanger);

    // Out-of-range / negative indices fall back to Forward rather than UB.
    CHECK (SlicerEngine::indexToPlaybackStyle (9) == PlaybackStyle::forward);
    CHECK (SlicerEngine::indexToPlaybackStyle (-1) == PlaybackStyle::forward);
    CHECK (SlicerEngine::indexToPlaybackStyle (1000) == PlaybackStyle::forward);
}

TEST_CASE ("SlicerEngine::nearestNoteValueIndex snaps to the palette")
{
    // Exact palette hits (see noteValueOptions in SlicerModel.cpp).
    CHECK (SlicerEngine::nearestNoteValueIndex (0.25) == 7);  // 16n
    CHECK (SlicerEngine::nearestNoteValueIndex (0.5) == 10);  // 8n
    CHECK (SlicerEngine::nearestNoteValueIndex (1.0) == 13);  // 4n
    CHECK (SlicerEngine::nearestNoteValueIndex (2.0) == 16);  // 2n
    CHECK (SlicerEngine::nearestNoteValueIndex (4.0) == 19);  // 1n

    // Off-grid targets land on the closest entry.
    CHECK (SlicerEngine::nearestNoteValueIndex (0.3) == 8);   // 8nt (1/3)
    CHECK (SlicerEngine::nearestNoteValueIndex (1.25) == 14); // 4nt (4/3)
    CHECK (SlicerEngine::nearestNoteValueIndex (1.5) == 15);  // 4nd (3/2)

    // Outside the palette at either end clamps to the extremes.
    CHECK (SlicerEngine::nearestNoteValueIndex (0.0) == 0);
    CHECK (SlicerEngine::nearestNoteValueIndex (4.5) == 19);
}

TEST_CASE ("SlicerEngine::computeBeatQuantizeTarget")
{
    // Degenerate inputs -> quantized == false, defaults untouched.
    CHECK_FALSE (SlicerEngine::computeBeatQuantizeTarget (0, false, 44100.0, 120.0, 120.0).quantized);
    CHECK_FALSE (SlicerEngine::computeBeatQuantizeTarget (-1, false, 44100.0, 120.0, 120.0).quantized);
    CHECK_FALSE (SlicerEngine::computeBeatQuantizeTarget (44100, false, 44100.0, 0.0, 120.0).quantized);
    CHECK_FALSE (SlicerEngine::computeBeatQuantizeTarget (44100, false, 44100.0, 120.0, 0.0).quantized);

    // 1s at 120bpm = 2 beats -> 2n -> target 1s -> ratio 1.0.
    auto result = SlicerEngine::computeBeatQuantizeTarget (44100, false, 44100.0, 120.0, 120.0);
    CHECK (result.quantized);
    CHECK (doctest::Approx (result.targetHostSeconds) == 1.0);
    CHECK (doctest::Approx (result.stretchRatio) == 1.0);

    // Ping-Pong quantizes the full round trip (2x) as one unit: 2s -> 4 beats -> 1n.
    auto pingPong = SlicerEngine::computeBeatQuantizeTarget (44100, true, 44100.0, 120.0, 120.0);
    CHECK (pingPong.quantized);
    CHECK (doctest::Approx (pingPong.targetHostSeconds) == 2.0);

    // Multi-bar length decomposes into whole bars + remainder: 2.5s at 120bpm
    // = 5 beats = 1 bar + 1 beat -> 5 beats total.
    auto multi = SlicerEngine::computeBeatQuantizeTarget (juce::roundToInt (2.5 * 44100.0), false, 44100.0, 120.0, 120.0);
    CHECK (multi.quantized);
    CHECK (doctest::Approx (multi.targetHostSeconds) == 2.5);

    // A sub-palette remainder below half the 128n entry is discarded, keeping
    // a near-whole-bar slice at exactly N whole bars.
    auto nearWholeBar = SlicerEngine::computeBeatQuantizeTarget (juce::roundToInt (4.01 / 2.0 * 44100.0), false, 44100.0, 120.0, 120.0);
    CHECK (nearWholeBar.quantized);
    CHECK (doctest::Approx (nearWholeBar.targetHostSeconds) == 2.0);
}

TEST_CASE ("SlicerEngine::computeScratchCycleLengthHostSamples")
{
    // Degenerate inputs -> 0.0 (callers treat as a no-op).
    CHECK (SlicerEngine::computeScratchCycleLengthHostSamples (13, 0, 120.0, 44100.0, 1.0) == 0.0);
    CHECK (SlicerEngine::computeScratchCycleLengthHostSamples (13, 44100, 0.0, 44100.0, 1.0) == 0.0);
    CHECK (SlicerEngine::computeScratchCycleLengthHostSamples (13, 44100, 120.0, 0.0, 1.0) == 0.0);
    CHECK (SlicerEngine::computeScratchCycleLengthHostSamples (13, 44100, 120.0, 44100.0, 0.0) == 0.0);

    // 4n rate (1 beat) at 120bpm = 0.5s = 22050 host samples per cycle.
    CHECK (doctest::Approx (SlicerEngine::computeScratchCycleLengthHostSamples (13, 44100, 120.0, 44100.0, 1.0)) == 22050.0);

    // A leg (half a cycle) longer than the slice content clamps to the slice
    // length: 1n rate wants an 88200-sample cycle, but a 10000-sample slice
    // collapses it to 20000.
    CHECK (doctest::Approx (SlicerEngine::computeScratchCycleLengthHostSamples (19, 10000, 120.0, 44100.0, 1.0)) == 20000.0);

    // playbackRate scales the source/host mapping, not the beat math: at
    // double speed the same 4n cycle occupies half the host time.
    CHECK (doctest::Approx (SlicerEngine::computeScratchCycleLengthHostSamples (13, 4410000, 120.0, 44100.0, 2.0)) == 22050.0);
}

TEST_CASE ("SlicerEngine::pickWeightedIndex")
{
    SlicerModel model;
    SlicerEngine engine { model };

    // Empty weight list -> no pick.
    CHECK (engine.pickWeightedIndex ({}) == -1);

    const int onlyEntry = engine.pickWeightedIndex ({ 5.0f });
    CHECK (onlyEntry == 0);

    // A single positive weight always wins -- and specifically a LEADING
    // zero-weight entry is never selectable, even on the target == 0.0 draw
    // that used to match its cumulative boundary. Only index 1 has weight,
    // so the result is deterministic across every draw, regardless of RNG.
    for (int i = 0; i < 1000; ++i)
        CHECK (engine.pickWeightedIndex ({ 0.0f, 1.0f, 0.0f }) == 1);

    // All-zero weights fall back to uniform random, but stay in range.
    for (int i = 0; i < 100; ++i)
    {
        const int index = engine.pickWeightedIndex ({ 0.0f, 0.0f, 0.0f });
        CHECK (index >= 0);
        CHECK (index < 3);
    }

    // Mixed weights only ever draw from the nonzero entries -- zero-weight
    // entries at the front, between, and at the back are all excluded.
    for (int i = 0; i < 1000; ++i)
    {
        const int index = engine.pickWeightedIndex ({ 0.0f, 3.0f, 0.0f, 0.0f, 2.0f, 0.0f });
        const bool validPick = (index == 1 || index == 4);
        CHECK (validPick);
    }
}
