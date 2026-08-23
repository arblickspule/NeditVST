// Nedit -- Engine layer.
//
// Source-tempo derivation and host-sync math. Pure functions of
// SampleState + host transport values; all behaviour verified against the
// original implementation (computeSourceSpanSeconds /
// getCalculatedOriginalBpm / computeMinimumHoldoffMs / repitch math).
//
// Note: the original had to read a duplicate "tempoTrim" atomic pair here
// because Performance mode repointed the real trim. In the rewrite the
// global trim is only ever the global trim, so these read SampleState
// directly.

#pragma once

#include <state/SampleState.h>
#include <state/Types.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace nedit::engine::tempo {

// Fallback holdoff when no usable tempo exists yet (no sample / degenerate
// span) -- matches the original's fixed default.
inline constexpr float kDefaultHoldoffMs = 30.0f;

// Duration in seconds of the span the loop represents: the trimmed sample
// span, or the bars-derived duration when the manual BPM override is on.
[[nodiscard]] inline double sourceSpanSeconds (const state::SampleState& sample) noexcept
{
    if (sample.manualBpmOverrideEnabled)
    {
        const double bpm = sample.manualBpmOverrideValue;

        if (bpm <= 0.0)
            return 0.0;

        const double beats = static_cast<double> (sample.loopLengthBars) * 4.0;  // assumes 4/4
        return (beats * 60.0) / bpm;
    }

    if (sample.sampleSampleRate <= 0.0)
        return 0.0;

    const auto spanFrames = std::max<std::int64_t> (0, sample.trimEndFrame - sample.trimStartFrame);
    return static_cast<double> (spanFrames) / sample.sampleSampleRate;
}

// The source material's own tempo, derived from loopLengthBars + the
// trimmed span (or the override, reported verbatim). 0.0 = no usable tempo.
[[nodiscard]] inline double calculatedOriginalBpm (const state::SampleState& sample) noexcept
{
    if (sample.manualBpmOverrideEnabled)
        return sample.manualBpmOverrideValue;

    if (! sample.hasSample() || sample.sampleLengthFrames == 0)
        return 0.0;

    const double lengthSeconds = sourceSpanSeconds (sample);

    if (lengthSeconds <= 0.0)
        return 0.0;

    const double beats = static_cast<double> (sample.loopLengthBars) * 4.0;  // assumes 4/4
    return (beats * 60.0) / lengthSeconds;
}

// Tempo-relative detection holdoff: ~a 32nd note at the source tempo,
// floored at 1ms (numerical safety net); fixed fallback with no tempo.
[[nodiscard]] inline float minimumHoldoffMs (const state::SampleState& sample) noexcept
{
    const double bpm = calculatedOriginalBpm (sample);

    if (bpm <= 0.0)
        return kDefaultHoldoffMs;

    constexpr double kThirtySecondNoteFractionOfBeat = 1.0 / 8.0;
    const double beatMs = 60000.0 / bpm;
    return static_cast<float> (std::max (1.0, beatMs * kThirtySecondNoteFractionOfBeat));
}

// Repitch factor: how much faster/slower to play the source so its
// loopLengthBars bars match the host tempo. >1 = source slower than host
// (speed up, pitch rises); <1 = source faster (slow down, pitch drops).
[[nodiscard]] inline double repitchRatio (const state::SampleState& sample, double hostBpm) noexcept
{
    if (hostBpm <= 0.0)
        return 1.0;

    const double loopLengthQuarterNotes = static_cast<double> (sample.loopLengthBars) * 4.0;
    const double hostLoopLengthSeconds = loopLengthQuarterNotes * (60.0 / hostBpm);

    if (hostLoopLengthSeconds <= 0.0)
        return 1.0;

    return sourceSpanSeconds (sample) / hostLoopLengthSeconds;
}

// Per-output-sample read-pointer increment for the direct (Repitch) path:
// sample-rate conversion times the repitch factor.
[[nodiscard]] inline double playbackRate (const state::SampleState& sample,
                                          double hostSampleRate, double hostBpm) noexcept
{
    if (hostSampleRate <= 0.0 || sample.sampleSampleRate <= 0.0)
        return 0.0;

    return (sample.sampleSampleRate / hostSampleRate) * repitchRatio (sample, hostBpm);
}

// Snap a source-frame position to the nearest grid line of `gridBeats`
// (quarter-note units) anchored at `anchorFrame`, at the given source
// tempo. Shared by transient grid-quantize and Performance trim snapping.
// Returns the input unchanged when there is no usable tempo/grid.
[[nodiscard]] inline std::int64_t quantizeFrameToGrid (std::int64_t frame,
                                                       std::int64_t anchorFrame,
                                                       double gridBeats,
                                                       double bpm,
                                                       double sampleRate) noexcept
{
    if (gridBeats <= 0.0 || bpm <= 0.0 || sampleRate <= 0.0)
        return frame;

    const double offsetSeconds = static_cast<double> (frame - anchorFrame) / sampleRate;
    const double offsetBeats = offsetSeconds * (bpm / 60.0);
    const double nearestGridStep = std::round (offsetBeats / gridBeats);
    const double quantizedSeconds = nearestGridStep * gridBeats * (60.0 / bpm);

    return anchorFrame + static_cast<std::int64_t> (
               std::llround (quantizedSeconds * sampleRate));
}

// Beat-quantized slice length: the palette entry closest to targetBeats.
// Strict less-than comparison, so the earliest (shortest) palette entry
// wins ties -- matching the original.
[[nodiscard]] inline int nearestNoteValueIndex (double targetBeats) noexcept
{
    int bestIndex = 0;
    double bestDistance = 1e300;

    for (int i = 0; i < state::kNumNoteValues; ++i)
    {
        const double distance = std::abs (state::kNoteValues[static_cast<std::size_t> (i)].beats
                                          - targetBeats);

        if (distance < bestDistance)
        {
            bestDistance = distance;
            bestIndex = i;
        }
    }

    return bestIndex;
}

// Result of the shared beat-quantize target calculation (Slice Length
// mode's per-pick length substitution).
struct BeatQuantizeTarget
{
    bool quantized = false;
    double stretchRatio = 1.0;      // replaces repitchRatio for this pick
    double targetHostSeconds = 0.0; // this pick's target duration
};

// The shared target-duration calculation both pitch modes' beat-quantize
// toggles feed into:
//   1. naturalBeats from the slice's source-domain duration at originalBpm
//      (Ping-Pong quantizes the FULL ROUND TRIP as one unit)
//   2. snap to the nearest note-value palette entry -- spans longer than a
//      bar decompose into whole bars plus a palette-quantized remainder
//      (the palette tops out at 1n, so searching it directly would crush
//      every multi-bar slice down to one bar)
//   3. targetHostSeconds = quantizedBeats at hostBpm
//   4. stretchRatio = naturalSourceSeconds / targetHostSeconds
// quantized stays false for degenerate inputs.
[[nodiscard]] inline BeatQuantizeTarget computeBeatQuantizeTarget (
    std::int64_t sliceLengthFrames, bool pingPong,
    double sampleSampleRate, double originalBpm, double hostBpm) noexcept
{
    BeatQuantizeTarget result;

    if (sliceLengthFrames <= 0 || originalBpm <= 0.0 || hostBpm <= 0.0
        || sampleSampleRate <= 0.0)
        return result;

    const auto lengthFrames = static_cast<double> (
        pingPong ? 2 * sliceLengthFrames : sliceLengthFrames);
    const double sliceNaturalSourceSeconds = lengthFrames / sampleSampleRate;
    const double naturalBeats = sliceNaturalSourceSeconds * originalBpm / 60.0;

    double quantizedBeats;

    if (naturalBeats > 4.0)
    {
        const double wholeBars = std::floor (naturalBeats / 4.0);
        const double remainderBeats = naturalBeats - (wholeBars * 4.0);
        const double smallestPaletteBeats =
            state::kNoteValues[0].beats;  // 128n

        // Below half the smallest palette entry, treat as no remainder --
        // an almost-exactly-N-bars slice gets no spurious addition.
        const double quantizedRemainder =
            (remainderBeats > smallestPaletteBeats * 0.5)
                ? state::kNoteValues[static_cast<std::size_t> (
                      nearestNoteValueIndex (remainderBeats))].beats
                : 0.0;

        quantizedBeats = (wholeBars * 4.0) + quantizedRemainder;
    }
    else
    {
        quantizedBeats =
            state::kNoteValues[static_cast<std::size_t> (
                nearestNoteValueIndex (naturalBeats))].beats;
    }

    const double targetHostSeconds = quantizedBeats * (60.0 / hostBpm);

    if (targetHostSeconds <= 0.0)
        return result;

    result.quantized = true;
    result.stretchRatio = sliceNaturalSourceSeconds / targetHostSeconds;
    result.targetHostSeconds = targetHostSeconds;
    return result;
}

// Scratch bounce-cycle length (one full forward-backward cycle at the
// Rate note value), in HOST samples. One LEG of the cycle is clamped to
// the slice's own content length -- the rate is tempo-synced and
// independent of slice size, so an unclamped cycle could otherwise ask
// the fold to bounce past the slice into whatever audio follows it.
// Degenerate inputs return 0.0 (callers treat that as a harmless no-op).
[[nodiscard]] inline double scratchCycleLengthHostSamples (
    int rateIndex, std::int64_t sliceLengthFrames,
    double hostBpm, double hostSampleRate, double playbackRate) noexcept
{
    if (sliceLengthFrames <= 0 || hostBpm <= 0.0 || hostSampleRate <= 0.0
        || playbackRate <= 0.0 || ! state::isValidNoteValueIndex (rateIndex))
        return 0.0;

    const double rateBeats = state::kNoteValues[static_cast<std::size_t> (rateIndex)].beats;
    const double desiredCycleHostSamples = rateBeats * (60.0 / hostBpm) * hostSampleRate;
    const double desiredCycleSourceSamples = desiredCycleHostSamples * playbackRate;
    const double clampedLegSourceSamples = std::min (desiredCycleSourceSamples * 0.5,
                                                     static_cast<double> (sliceLengthFrames));

    return 2.0 * clampedLegSourceSamples / playbackRate;
}

} // namespace nedit::engine::tempo
