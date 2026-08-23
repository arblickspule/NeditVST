// Nedit -- State layer (debug/tooling target, not linked by the plugin
// core).
//
// Human-readable JSON export/import of PluginState -- for debugging,
// diffing presets, and golden-file tests. The binary format
// (Serialization.h) is what goes into the DAW chunk; this is the
// inspection mirror of it.

#pragma once

#include "PluginState.h"

#include <optional>
#include <string>

namespace nedit::state {

// Pretty-printed JSON document of the full state.
[[nodiscard]] std::string toJson (const PluginState& state);

// Parse a JSON document produced by toJson(). Returns std::nullopt on
// malformed JSON or wrong document type. Missing fields keep defaults;
// all values are sanitized.
[[nodiscard]] std::optional<PluginState> fromJson (const std::string& text);

} // namespace nedit::state
