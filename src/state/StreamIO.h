// Nedit -- State layer.
//
// Minimal little-endian binary stream writer/reader used by the state
// serializer. The reader is fully bounds-checked: reads past the end set
// a sticky failure flag and return zero values instead of touching
// out-of-range memory, so corrupted/truncated host chunks can never
// crash the plugin.

#pragma once

#include <bit>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace nedit::state {

class StreamWriter
{
public:
    [[nodiscard]] const std::vector<std::uint8_t>& data() const noexcept { return bytes; }
    [[nodiscard]] std::vector<std::uint8_t> take() noexcept { return std::move (bytes); }
    [[nodiscard]] std::size_t size() const noexcept { return bytes.size(); }

    void writeU8 (std::uint8_t v)   { bytes.push_back (v); }
    void writeBool (bool v)         { writeU8 (v ? 1 : 0); }
    void writeU16 (std::uint16_t v) { writeLE (v); }
    void writeU32 (std::uint32_t v) { writeLE (v); }
    void writeU64 (std::uint64_t v) { writeLE (v); }
    void writeI32 (std::int32_t v)  { writeLE (static_cast<std::uint32_t> (v)); }
    void writeI64 (std::int64_t v)  { writeLE (static_cast<std::uint64_t> (v)); }
    void writeI8 (std::int8_t v)    { writeU8 (static_cast<std::uint8_t> (v)); }

    void writeF32 (float v)
    {
        writeLE (std::bit_cast<std::uint32_t> (v));
    }

    void writeF64 (double v)
    {
        writeLE (std::bit_cast<std::uint64_t> (v));
    }

    void writeString (const std::string& s)
    {
        writeU32 (static_cast<std::uint32_t> (s.size()));
        bytes.insert (bytes.end(), s.begin(), s.end());
    }

    void writeRaw (const void* src, std::size_t count)
    {
        const auto* p = static_cast<const std::uint8_t*> (src);
        bytes.insert (bytes.end(), p, p + count);
    }

    // Overwrite a previously written u32 (for patching section sizes).
    void patchU32 (std::size_t offset, std::uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            bytes[offset + static_cast<std::size_t> (i)] =
                static_cast<std::uint8_t> ((v >> (8 * i)) & 0xffu);
    }

private:
    template <typename T>
    void writeLE (T v)
    {
        for (std::size_t i = 0; i < sizeof (T); ++i)
            bytes.push_back (static_cast<std::uint8_t> ((v >> (8 * i)) & 0xffu));
    }

    std::vector<std::uint8_t> bytes;
};

class StreamReader
{
public:
    StreamReader (const std::uint8_t* data, std::size_t size) noexcept
        : begin (data), length (size) {}

    [[nodiscard]] bool ok() const noexcept { return ! failed; }
    [[nodiscard]] std::size_t position() const noexcept { return pos; }
    [[nodiscard]] std::size_t remaining() const noexcept { return length - pos; }
    [[nodiscard]] bool atEnd() const noexcept { return pos >= length; }

    void skip (std::size_t count) noexcept
    {
        if (! ensure (count))
            return;
        pos += count;
    }

    [[nodiscard]] std::uint8_t readU8() noexcept
    {
        if (! ensure (1))
            return 0;
        return begin[pos++];
    }

    [[nodiscard]] bool readBool() noexcept { return readU8() != 0; }
    [[nodiscard]] std::uint16_t readU16() noexcept { return readLE<std::uint16_t>(); }
    [[nodiscard]] std::uint32_t readU32() noexcept { return readLE<std::uint32_t>(); }
    [[nodiscard]] std::uint64_t readU64() noexcept { return readLE<std::uint64_t>(); }
    [[nodiscard]] std::int8_t readI8() noexcept { return static_cast<std::int8_t> (readU8()); }
    [[nodiscard]] std::int32_t readI32() noexcept { return static_cast<std::int32_t> (readLE<std::uint32_t>()); }
    [[nodiscard]] std::int64_t readI64() noexcept { return static_cast<std::int64_t> (readLE<std::uint64_t>()); }

    [[nodiscard]] float readF32() noexcept
    {
        return std::bit_cast<float> (readLE<std::uint32_t>());
    }

    [[nodiscard]] double readF64() noexcept
    {
        return std::bit_cast<double> (readLE<std::uint64_t>());
    }

    [[nodiscard]] std::string readString()
    {
        const auto count = readU32();

        if (! ensure (count))
            return {};

        std::string s (reinterpret_cast<const char*> (begin + pos), count);
        pos += count;
        return s;
    }

    // A bounded sub-reader over the next `count` bytes (for sections).
    // Advances this reader past the range. Returns an empty, failed
    // reader if out of range.
    [[nodiscard]] StreamReader subReader (std::size_t count) noexcept
    {
        if (! ensure (count))
        {
            StreamReader dead (nullptr, 0);
            dead.failed = true;
            return dead;
        }

        StreamReader sub (begin + pos, count);
        pos += count;
        return sub;
    }

private:
    template <typename T>
    [[nodiscard]] T readLE() noexcept
    {
        if (! ensure (sizeof (T)))
            return T {};

        T v {};
        for (std::size_t i = 0; i < sizeof (T); ++i)
            v = static_cast<T> (v | (static_cast<T> (begin[pos + i]) << (8 * i)));

        pos += sizeof (T);
        return v;
    }

    [[nodiscard]] bool ensure (std::size_t count) noexcept
    {
        if (failed || count > length - pos)
        {
            failed = true;
            return false;
        }

        return true;
    }

    const std::uint8_t* begin = nullptr;
    std::size_t length = 0;
    std::size_t pos = 0;
    bool failed = false;
};

} // namespace nedit::state
