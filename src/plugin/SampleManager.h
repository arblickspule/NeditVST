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
#include <state/SampleState.h>

#include <atomic>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace nedit::plugin {

struct LoadedSample
{
    DecodedAudio audio;
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

        auto loaded = std::make_shared<LoadedSample>();
        loaded->audio = std::move (*audio);

        engine::TransientDetector detector;
        const int numChannels = loaded->audio.channelCount();
        std::vector<const float*> channelPointers (static_cast<std::size_t> (numChannels));
        for (int c = 0; c < numChannels; ++c)
            channelPointers[static_cast<std::size_t> (c)] =
                loaded->audio.channels[static_cast<std::size_t> (c)].data();

        detector.analyze (channelPointers.data(), numChannels, loaded->audio.frames,
                          loaded->audio.sampleRate);
        loaded->slices = engine::buildSlices (detector, updated);

        slot_.store (loaded, std::memory_order_release);

        LoadResult result;
        result.sample = std::move (loaded);
        result.updated = std::move (updated);
        return result;
    }

private:
    std::atomic<std::shared_ptr<const LoadedSample>> slot_;
};

} // namespace nedit::plugin
