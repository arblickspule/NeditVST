// Nedit -- Plugin layer. See WavDecoder.h.

#include "WavDecoder.h"

#include <algorithm>
#include <cstring>

namespace nedit::plugin {
namespace {

constexpr std::size_t kHeaderSize = 12; // "RIFF" + size + "WAVE"
constexpr int kMaxChannels = 16;
constexpr std::uint16_t kFormatPcm = 1;
constexpr std::uint16_t kFormatFloat = 3;
constexpr std::uint16_t kFormatExtensible = 0xFFFE;

struct ChunkView
{
    const std::uint8_t* data = nullptr;
    std::uint32_t size = 0;
};

[[nodiscard]] std::uint16_t readU16 (const std::uint8_t* p) noexcept
{
    return static_cast<std::uint16_t> (static_cast<std::uint16_t> (p[0])
                                       | (static_cast<std::uint16_t> (p[1]) << 8));
}

[[nodiscard]] std::uint32_t readU32 (const std::uint8_t* p) noexcept
{
    return static_cast<std::uint32_t> (p[0]) | (static_cast<std::uint32_t> (p[1]) << 8)
         | (static_cast<std::uint32_t> (p[2]) << 16)
         | (static_cast<std::uint32_t> (p[3]) << 24);
}

[[nodiscard]] bool tagEquals (const std::uint8_t* p, const char (&tag)[5]) noexcept
{
    return std::memcmp (p, tag, 4) == 0;
}

// Walks chunks after the RIFF header, invoking `visit` per chunk. Returns
// false on structural garbage. Handles the odd-size pad byte.
template <typename Visit>
bool forEachChunk (const std::uint8_t* data, std::size_t size, Visit&& visit)
{
    if (size < kHeaderSize || ! tagEquals (data, "RIFF") || ! tagEquals (data + 8, "WAVE"))
        return false;

    std::size_t pos = kHeaderSize;
    while (pos + 8 <= size)
    {
        ChunkView chunk { data + pos + 8, readU32 (data + pos + 4) };
        if (chunk.size > size - (pos + 8))
            chunk.size = static_cast<std::uint32_t> (size - (pos + 8)); // truncated tail

        visit (data + pos, chunk);

        pos += 8u + chunk.size + (chunk.size & 1u);
    }
    return true;
}

[[nodiscard]] float convertSample (const std::uint8_t* p, std::uint16_t bits,
                                   std::uint16_t format) noexcept
{
    switch (format)
    {
        case kFormatFloat:
            if (bits == 64)
            {
                double d = 0.0;
                std::memcpy (&d, p, 8);
                return static_cast<float> (d);
            }
            {
                float f = 0.0f;
                std::memcpy (&f, p, 4);
                return f;
            }
        default: // integer PCM
            if (bits == 16)
                return static_cast<float> (static_cast<std::int16_t> (readU16 (p))) / 32768.0f;
            if (bits == 24)
            {
                std::int32_t v = static_cast<std::int32_t> (p[0]) | (static_cast<std::int32_t> (p[1]) << 8)
                               | (static_cast<std::int32_t> (p[2]) << 16);
                if ((v & 0x00800000) != 0)
                    v |= ~0x00FFFFFF; // sign-extend
                return static_cast<float> (v) / 8388608.0f;
            }
            {
                std::int32_t v = static_cast<std::int32_t> (readU32 (p));
                return static_cast<float> (v) / 2147483648.0f;
            }
    }
}

} // namespace

std::optional<DecodedAudio> decodeWav (const std::uint8_t* data, std::size_t size)
{
    if (data == nullptr)
        return std::nullopt;

    std::uint16_t formatTag = 0;
    std::uint16_t numChannels = 0;
    std::uint16_t bitsPerSample = 0;
    std::uint32_t sampleRate = 0;
    std::uint16_t blockAlign = 0;
    ChunkView dataChunk {};

    bool haveFmt = false;
    bool haveData = false;

    if (! forEachChunk (
            data, size,
            [&] (const std::uint8_t* id, const ChunkView& chunk) {
                if (tagEquals (id, "fmt ") && chunk.size >= 16 && ! haveFmt)
                {
                    formatTag = readU16 (chunk.data);
                    numChannels = readU16 (chunk.data + 2);
                    sampleRate = readU32 (chunk.data + 4);
                    bitsPerSample = readU16 (chunk.data + 14);
                    // Fmt payload layout: format(0) channels(2) rate(4)
                    // byteRate(8) blockAlign(12) bits(14).
                    blockAlign = readU16 (chunk.data + 12);

                    if (formatTag == kFormatExtensible && chunk.size >= 40)
                        formatTag = readU16 (chunk.data + 24); // SubFormat GUID first two bytes

                    haveFmt = true;
                }
                else if (tagEquals (id, "data") && ! haveData)
                {
                    dataChunk = chunk;
                    haveData = true;
                }
            }))
    {
        return std::nullopt;
    }

    if (! haveFmt || ! haveData || numChannels == 0 || numChannels > kMaxChannels
        || sampleRate == 0 || blockAlign == 0)
        return std::nullopt;

    if (formatTag == kFormatPcm)
    {
        if (bitsPerSample != 16 && bitsPerSample != 24 && bitsPerSample != 32)
            return std::nullopt;
    }
    else if (formatTag == kFormatFloat)
    {
        if (bitsPerSample != 32 && bitsPerSample != 64)
            return std::nullopt;
    }
    else
    {
        return std::nullopt;
    }

    // Internal consistency: a frame must be at least numChannels x
    // bytesPerSample wide. blockAlign comes straight from the file, and a
    // lying fmt chunk (e.g. mono float64 claiming blockAlign = 1) would
    // otherwise inflate `frames` and send the per-frame reads below past
    // the end of the data chunk -- a heap out-of-bounds read on attacker-
    // controlled input. This check also guarantees the per-channel stride
    // (blockAlign / numChannels) is >= bytesPerSample, so every
    // convertSample read stays inside its own frame.
    const auto bytesPerSample = static_cast<std::uint32_t> (bitsPerSample / 8u);
    if (static_cast<std::uint32_t> (blockAlign)
        < static_cast<std::uint32_t> (numChannels) * bytesPerSample)
        return std::nullopt;

    auto frames = static_cast<std::int64_t> (dataChunk.size / blockAlign);
    if (frames <= 0)
        return std::nullopt;

    DecodedAudio out;
    out.frames = frames;
    out.sampleRate = static_cast<double> (sampleRate);
    out.channels.resize (numChannels);
    for (auto& c : out.channels)
        c.resize (static_cast<std::size_t> (out.frames));

    for (std::int64_t f = 0; f < frames; ++f)
    {
        const std::uint8_t* frame = dataChunk.data
                                  + static_cast<std::size_t> (f) * blockAlign;
        for (std::uint16_t c = 0; c < numChannels; ++c)
            out.channels[c][static_cast<std::size_t> (f)] =
                convertSample (frame + static_cast<std::size_t> (c) * (blockAlign / numChannels),
                               bitsPerSample, formatTag);
    }
    return out;
}

} // namespace nedit::plugin
