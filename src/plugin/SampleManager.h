// Nedit -- Plugin layer.
//
// Owns the decoded-sample slot shared with the audio thread and the
// load/analyze pipeline (UI-thread only work: decode + transient
// analysis produce a fully immutable LoadedSample, published via an
// atomic shared_ptr -- the audio thread just acquires it per block).
//
// The returned LoadResult.updated SampleState carries the metadata the
// rest of the state needs (path/rate/length/full-span trim); the CALLER
// folds it into their authoritative PluginState copy and republishes,
// so state ownership stays exactly where it was in Phase 3.

#pragma once

#include "WavDecoder.h"

#include <engine/Slice.h>
#include <engine/SliceBuilder.h>
#include <engine/TransientDetector.h>
#include <state/SampleState.h>

#include <atomic>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace nedit::plugin {

// The immutable per-load bundle shared with the audio thread. The decoded
// audio and the transient analysis are BOTH immutable once built, so they
// are held by shared_ptr and shared across slice-only edits (adding or
// removing a manual marker rebuilds only the `slices` vector -- the large
// audio buffer and the analysis are never re-cloned or re-run).
struct LoadedSample
{
    std::shared_ptr<const DecodedAudio> audio;
    std::shared_ptr<const engine::TransientDetector> analysis;
    std::vector<engine::Slice> slices;
};

class SampleManager
{
public:
    struct LoadResult
    {
        std::shared_ptr<const LoadedSample> sample;
        state::SampleState updated;
    };

    [[nodiscard]] std::shared_ptr<const LoadedSample> acquire() const noexcept
    {
        return slot_.load (std::memory_order_acquire);
    }

    [[nodiscard]] std::optional<LoadResult> loadFile (const std::string& path,
                                                      const state::SampleState& current)
    {
        std::ifstream file (path, std::ios::binary);
        if (! file)
            return std::nullopt;

        std::vector<std::uint8_t> bytes ((std::istreambuf_iterator<char> (file)),
                                         std::istreambuf_iterator<char> {});
        if (bytes.empty())
            return std::nullopt;

        return loadFromMemory (bytes.data(), bytes.size(), current, path);
    }

    [[nodiscard]] std::optional<LoadResult>
        loadFromMemory (const std::uint8_t* data, std::size_t size,
                        const state::SampleState& current,
                        const std::string& path = {})
    {
        auto audio = decodeWav (data, size);
        if (! audio)
            return std::nullopt;

        state::SampleState updated = current;
        updated.samplePath = path;
        updated.sampleLengthFrames = audio->frames;
        updated.sampleSampleRate = audio->sampleRate;
        updated.trimStartFrame = 0;
        updated.trimEndFrame = audio->frames;

        auto audioPtr = std::make_shared<const DecodedAudio> (std::move (*audio));

        auto detector = std::make_shared<engine::TransientDetector>();
        const int numChannels = audioPtr->channelCount();
        std::vector<const float*> channelPointers (static_cast<std::size_t> (numChannels));
        for (int c = 0; c < numChannels; ++c)
            channelPointers[static_cast<std::size_t> (c)] =
                audioPtr->channels[static_cast<std::size_t> (c)].data();

        detector->analyze (channelPointers.data(), numChannels, audioPtr->frames,
                           audioPtr->sampleRate);

        auto loaded = std::make_shared<LoadedSample>();
        loaded->audio = audioPtr;
        loaded->analysis = detector;
        loaded->slices = engine::buildSlices (*detector, updated);

        slot_.store (loaded, std::memory_order_release);

        LoadResult result;
        result.sample = std::move (loaded);
        result.updated = std::move (updated);
        return result;
    }

    // Rebuild ONLY the slice list from the retained analysis + the given
    // sample state (manual points / exclusions / trim / sensitivity). The
    // audio and analysis are shared with the previous LoadedSample, so this
    // is cheap and safe to call on the UI thread for a marker edit. Returns
    // the new sample (or the current one unchanged if nothing is loaded).
    std::shared_ptr<const LoadedSample> rebuildSlices (const state::SampleState& sample)
    {
        auto cur = acquire();
        if (cur == nullptr || cur->analysis == nullptr)
            return cur;

        auto next = std::make_shared<LoadedSample>();
        next->audio = cur->audio;         // share (immutable)
        next->analysis = cur->analysis;   // share (immutable)
        next->slices = engine::buildSlices (*cur->analysis, sample);

        slot_.store (next, std::memory_order_release);
        return next;
    }

    // Snap a frame to the strongest transient within +/- radiusFrames,
    // confined to [rangeStart, rangeEnd). Backed by the retained analysis's
    // raw derivative peak search (no sensitivity threshold -- matches the
    // original's findNearestPeak). Returns the input unchanged if there is
    // no analysis.
    [[nodiscard]] std::int64_t snapToTransient (std::int64_t frame,
                                                std::int64_t radiusFrames,
                                                std::int64_t rangeStart,
                                                std::int64_t rangeEnd) const
    {
        auto cur = acquire();
        if (cur == nullptr || cur->analysis == nullptr)
            return frame;
        return cur->analysis->findNearestPeak (frame, radiusFrames, rangeStart, rangeEnd);
    }

private:
    std::atomic<std::shared_ptr<const LoadedSample>> slot_;
};

} // namespace nedit::plugin
