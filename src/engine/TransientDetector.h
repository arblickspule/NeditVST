// Nedit -- Engine layer.
//
// Transient detector, behaviourally faithful to the original (which was
// itself ported from a validated Max/MSP prototype):
//
//   1) rectified envelope follower (1ms attack, 50ms release, one-pole)
//   2) positive derivative of the envelope
//   3) adaptive threshold scaled by a 0-1 sensitivity control:
//        sensitivity 0 -> nothing crosses (zero transients, by contract)
//        sensitivity 1 -> threshold drops to the noise floor
//   4) peak-picking with holdoff so one transient can't fire twice
//
// Two-stage split: analyze() does the expensive one-off pass (envelope +
// derivative over the whole file) and caches it; detectSlices() cheaply
// re-runs thresholding/peak-picking against the cache, so dragging the
// sensitivity slider never re-analyses audio. findNearestPeak() backs
// manual-point and trim snapping ("the transient that would have been
// detected at higher sensitivity").
//
// Pure C++, framework-free: audio comes in as raw channel pointers.

#pragma once

#include "Slice.h"

#include <cstdint>
#include <vector>

namespace nedit::engine {

class TransientDetector
{
public:
    // Runs the envelope + derivative pass once and caches the results.
    // Call whenever a new sample is loaded. Mono-sums the channels for
    // detection purposes (playback uses all channels).
    // channels: numChannels pointers to numFrames floats each.
    void analyze (const float* const* channels, int numChannels,
                  std::int64_t numFrames, double sampleRate);

    // Cheap re-run of thresholding + peak-picking against the cache.
    //   sensitivity: 0 (nothing detected) .. 1 (maximally permissive)
    //   holdoffMs:   minimum gap between consecutive onsets
    //   rangeStart/rangeEnd: confine search AND output to
    //     [rangeStart, rangeEnd) -- the trim range. rangeStart is always
    //     the first boundary (never excludable); the last slice ends at
    //     rangeEnd. Negative values mean "the whole analysed buffer".
    [[nodiscard]] std::vector<Slice> detectSlices (float sensitivity, float holdoffMs,
                                                   std::int64_t rangeStart = -1,
                                                   std::int64_t rangeEnd = -1) const;

    // Strongest derivative peak within +/- searchRadiusFrames of
    // targetFrame, clamped to [rangeStart, rangeEnd). Returns targetFrame
    // unchanged if there is no analysis.
    [[nodiscard]] std::int64_t findNearestPeak (std::int64_t targetFrame,
                                                std::int64_t searchRadiusFrames,
                                                std::int64_t rangeStart = -1,
                                                std::int64_t rangeEnd = -1) const;

    [[nodiscard]] bool hasAnalysis() const noexcept { return ! derivative.empty(); }
    [[nodiscard]] std::int64_t analyzedLengthFrames() const noexcept { return numFrames; }

    // Envelope follower time constants (fixed, matching the original; easy
    // to surface as parameters later if per-source tuning is needed).
    static constexpr float kAttackTimeMs = 1.0f;
    static constexpr float kReleaseTimeMs = 50.0f;

private:
    std::vector<float> envelope;
    std::vector<float> derivative;
    double analyzedSampleRate = 44100.0;
    std::int64_t numFrames = 0;
    float globalMaxDerivative = 0.0f;
    float noiseFloor = 0.0f;
};

} // namespace nedit::engine
