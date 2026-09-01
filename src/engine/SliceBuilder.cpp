#include "SliceBuilder.h"
#include "Tempo.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace nedit::engine {

namespace {

[[nodiscard]] double sliceRmsFrames (const float* const* channels, int numChannels,
                                     std::int64_t startFrame, std::int64_t endFrame) noexcept
{
    if (numChannels <= 0 || endFrame <= startFrame)
        return 0.0;

    double sum = 0.0;
    std::int64_t count = 0;

    for (std::int64_t i = startFrame; i < endFrame; ++i)
    {
        double energy = 0.0;
        for (int c = 0; c < numChannels; ++c)
        {
            if (channels[c] == nullptr)
                return 0.0;
            const double v = static_cast<double> (channels[c][i]);
            energy += v * v;
        }
        sum += energy;
        ++count;
    }

    if (count <= 0)
        return 0.0;
    return std::sqrt (sum / static_cast<double> (count));
}

} // namespace

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

std::vector<Slice> filterGhostSlices (const std::vector<Slice>& input,
                                      const float* const* channels,
                                      int numChannels,
                                      std::int64_t trimStart,
                                      std::int64_t trimEnd,
                                      double sampleRate,
                                      const std::vector<state::SamplePoint>* manualPoints)
{
    if (input.empty() || channels == nullptr || numChannels <= 0
        || trimEnd <= trimStart || sampleRate <= 0.0)
        return input;

    std::vector<Slice> out;

    // 1) Merge a leading sliver into the following slice: a slice that
    //    starts exactly at trimStart and is shorter than the threshold (in
    //    ms). It is the detector's "range start plays the role of position
    //    0" prepend when the first real onset lands a few frames in -- not a
    //    genuine boundary. Merging (rather than dropping) preserves the
    //    "slices tile the trim" invariant and remembers the attack frames;
    //    the merged result is the natural first slice [trimStart, 2ndOnset).
    //    A manual boundary placed inside the would-be sliver keeps it (the
    //    user placed it deliberately -- issue #5 is about the auto-detected
    //    artifact only).
    const std::int64_t start = input.front().startFrame;
    const std::int64_t sliverMaxFrames = static_cast<std::int64_t> (
        (kLeadingSliverMaxMs / 1000.0) * sampleRate);
    std::size_t from = 0;

    if (start == trimStart && input.front().lengthFrames() < sliverMaxFrames && input.size() > 1)
    {
        const std::int64_t sliverEnd = input.front().endFrame;
        bool manualInside = false;
        if (manualPoints != nullptr)
        {
            for (const auto& mp : *manualPoints)
                if (mp.position > trimStart && mp.position <= sliverEnd)
                {
                    manualInside = true;
                    break;
                }
        }
        if (! manualInside)
            from = 1;  // input[0] merges into input[1] (kept below)
    }

    // Find the final slice (the one ending at trimEnd).
    std::size_t tailIdx = input.size();
    for (std::size_t i = from; i < input.size(); ++i)
        if (input[i].endFrame == trimEnd)
            tailIdx = i;

    // 2) Drop a silent tail: the slice ending at trimEnd whose own RMS is
    //    below the silence ratio x the sample's RMS over the trim. A resting
    //    final bar/beat leaves this whole slice near-silent (a true rest
    //    carries no audible content to lose), and it reads as a tall empty
    //    top row in the sequencer. A final slice with real content, however
    //    short, stays.
    bool dropTail = false;
    if (tailIdx < input.size() && tailIdx >= from)
    {
        const double sliceRms = sliceRmsFrames (channels, numChannels,
                                                input[tailIdx].startFrame,
                                                input[tailIdx].endFrame);
        const double sampleRms = sliceRmsFrames (channels, numChannels,
                                                 trimStart, trimEnd);
        // Never drop the only remaining slice (e.g. the leading sliver was
        // merged AND the tail is silent -> keep the tail rather than return
        // an empty list).
        const bool wouldLeaveAtLeastOne = (input.size() - from) > 1;
        dropTail = wouldLeaveAtLeastOne
                   && sampleRms > 0.0
                   && sliceRms < kSilentTailRatio * sampleRms;
    }

    for (std::size_t i = from; i < input.size(); ++i)
        if (! (dropTail && i == tailIdx))
            out.push_back (input[i]);

    // The leading sliver is merged into its successor: the successor's
    // start moves back to trimStart (frames retained, no orphaned audio,
    // slices still tile [trimStart, trimEnd) exactly).
    if (from == 1 && ! out.empty())
        out[0].startFrame = trimStart;

    return out;
}

} // namespace nedit::engine
