#include "SliceBuilder.h"
#include "Tempo.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace nedit::engine {

std::vector<Slice> mergeOnsetsIntoSlices (const std::vector<Slice>& autoSlices,
                                          const state::SampleState& sample)
{
    const std::int64_t trimStart = sample.trimStartFrame;
    const std::int64_t trimEnd = sample.trimEndFrame;

    const auto matchToleranceFrames = static_cast<std::int64_t> (
        (static_cast<double> (kManualSnapRadiusMs) / 1000.0) * sample.sampleSampleRate);

    const double originalBpm = tempo::calculatedOriginalBpm (sample);
    const double gridBeats = state::isValidNoteValueIndex (sample.quantizeGridIndex)
        ? state::kNoteValues[static_cast<std::size_t> (sample.quantizeGridIndex)].beats
        : 0.0;

    std::vector<std::int64_t> onsets;
    onsets.reserve (autoSlices.size() + sample.manualPoints.size());

    for (const auto& autoSlice : autoSlices)
    {
        if (autoSlice.startFrame == trimStart)
        {
            onsets.push_back (trimStart);  // the trim start is never excludable
            continue;
        }

        // 1) Exclusion matching against the RAW detected position.
        bool excluded = false;

        for (const auto& excludedPoint : sample.excludedPoints)
        {
            if (std::llabs (autoSlice.startFrame - excludedPoint.position) <= matchToleranceFrames)
            {
                excluded = true;
                break;
            }
        }

        if (excluded)
            continue;

        // 2) Optional grid quantization of the surviving auto onset,
        //    clamped so it can never leave the trim range.
        std::int64_t onsetFrame = autoSlice.startFrame;

        if (sample.quantizeTransients)
        {
            onsetFrame = tempo::quantizeFrameToGrid (onsetFrame, trimStart, gridBeats,
                                                     originalBpm, sample.sampleSampleRate);
            onsetFrame = std::clamp (onsetFrame, trimStart,
                                     std::max (trimStart, trimEnd - 1));
        }

        onsets.push_back (onsetFrame);
    }

    // 3) Manual points, merged as-is; outside-trim points are filtered
    //    here (soft exclude), never deleted from the state.
    for (const auto& manualPoint : sample.manualPoints)
        if (manualPoint.position > trimStart && manualPoint.position < trimEnd)
            onsets.push_back (manualPoint.position);

    // 4) The trim start is always the first boundary.
    if (onsets.empty() || onsets.front() != trimStart)
        onsets.insert (onsets.begin(), trimStart);

    std::sort (onsets.begin(), onsets.end());
    onsets.erase (std::unique (onsets.begin(), onsets.end()), onsets.end());

    std::vector<Slice> result;
    result.reserve (onsets.size());

    for (std::size_t i = 0; i < onsets.size(); ++i)
    {
        Slice slice;
        slice.startFrame = onsets[i];
        slice.endFrame = (i + 1 < onsets.size()) ? onsets[i + 1] : trimEnd;

        if (slice.lengthFrames() > 0)
            result.push_back (slice);
    }

    return result;
}

std::vector<Slice> buildSlices (const TransientDetector& detector,
                                const state::SampleState& sample)
{
    const auto autoSlices = detector.detectSlices (sample.sensitivity,
                                                   tempo::minimumHoldoffMs (sample),
                                                   sample.trimStartFrame,
                                                   sample.trimEndFrame);

    return mergeOnsetsIntoSlices (autoSlices, sample);
}

} // namespace nedit::engine
