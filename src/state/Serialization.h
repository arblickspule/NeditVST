// Nedit -- State layer.
//
// Versioned binary serialization of PluginState -- the format written
// into the DAW's state chunk.
//
// Layout:
//   magic   'N' 'E' 'D' 'T'          (4 bytes)
//   version u32                       (kStateFormatVersion)
//   then repeated sections:
//     tag   u32 (fourcc)
//     size  u32 (payload bytes)
//     payload
//
// Unknown sections are skipped on read (forward compatibility); missing
// sections keep their defaults (backward compatibility). All values are
// range-clamped after reading -- a corrupted or malicious chunk can
// produce a default-ish state but never a crash or out-of-range value.

#pragma once

#include "PluginState.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace nedit::state {

inline constexpr std::uint32_t kStateFormatVersion = 2;

// Serialize the full state to a binary chunk.
[[nodiscard]] std::vector<std::uint8_t> serialize (const PluginState& state);

// Deserialize. Returns std::nullopt if the data is not a Nedit chunk
// (bad magic / bad header) or a section payload is structurally invalid.
// The returned state is always sanitized.
[[nodiscard]] std::optional<PluginState> deserialize (const std::uint8_t* data, std::size_t size);

[[nodiscard]] inline std::optional<PluginState> deserialize (const std::vector<std::uint8_t>& data)
{
    return deserialize (data.data(), data.size());
}

} // namespace nedit::state
