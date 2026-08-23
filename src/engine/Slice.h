// Nedit -- Engine layer.
//
// A slice: a half-open [startFrame, endFrame) span of the source sample.
// DERIVED state -- rebuilt deterministically from SampleState + audio by
// the transient detector and slice builder. Never serialized.

#pragma once

#include <cstdint>

namespace nedit::engine {

struct Slice
{
    std::int64_t startFrame = 0;
    std::int64_t endFrame = 0;  // exclusive

    [[nodiscard]] std::int64_t lengthFrames() const noexcept { return endFrame - startFrame; }

    bool operator== (const Slice&) const = default;
};

} // namespace nedit::engine
