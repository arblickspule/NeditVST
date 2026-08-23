// Nedit -- Engine layer.
//
// Slice building: merges auto-detected onsets with the user's manual and
// excluded points, applies optional grid quantization, and assembles the
// final derived slice list confined to the trim range.
//
// Pipeline (order is behaviourally significant, matching the original):
//   1) exclusion matching happens against the RAW detected position
//      (before quantize) -- an exclusion click targets the peak the user
//      actually saw on the waveform
//   2) auto onsets are then optionally quantized to the grid
//   3) manual points are merged as-is (never quantized), and points
//      outside the trim are "soft-excluded" (filtered, not deleted --
//      widening the trim back out restores them)
//   4) the trim start is always the first boundary (never excludable)

#pragma once

#include "Slice.h"
#include "TransientDetector.h"

#include <state/SampleState.h>

#include <vector>

namespace nedit::engine {

// Match tolerance for pairing an excluded point with a detected onset,
// and the search radius for manual-point/trim transient snapping.
inline constexpr float kManualSnapRadiusMs = 50.0f;

// Merge auto-detected slices with SampleState's manual/excluded points
// and trim. autoSlices must already be confined to the trim range (they
// come from TransientDetector::detectSlices with the trim passed in).
[[nodiscard]] std::vector<Slice> mergeOnsetsIntoSlices (const std::vector<Slice>& autoSlices,
                                                        const state::SampleState& sample);

// Convenience: full rebuild -- detect (with tempo-relative holdoff and
// the sample's sensitivity/trim) then merge. The detector must have been
// analyze()d with the sample's audio already.
[[nodiscard]] std::vector<Slice> buildSlices (const TransientDetector& detector,
                                              const state::SampleState& sample);

} // namespace nedit::engine
