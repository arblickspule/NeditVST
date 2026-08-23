// Nedit -- Plugin layer.
//
// Minimal RIFF/WAVE decoder for sample loading (UI thread). Supports the
// formats the original relied on JUCE readers for, restricted to what
// lossless sample-pack workflow needs: PCM 16/24/32-bit integer and
// IEEE float 32/64-bit, mono..16 channels, including
// WAVE_FORMAT_EXTENSIBLE wrappers.
//
// Deliberately strict: anything malformed returns nullopt instead of
// "best effort" audio -- a silently mis-decoded sample is worse than a
// visible load failure. Truncated data chunks still decode the frames
// that are present (hosts/browsers sometimes write short files).

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace nedit::plugin {

struct DecodedAudio
{
    // Planar channels, each exactly `frames` long.
    std::vector<std::vector<float>> channels;
    std::int64_t frames = 0;
    double sampleRate = 44100.0;

    [[nodiscard]] int channelCount() const noexcept
    {
        return static_cast<int> (channels.size());
    }
};

[[nodiscard]] std::optional<DecodedAudio> decodeWav (const std::uint8_t* data,
                                                     std::size_t size);

} // namespace nedit::plugin
