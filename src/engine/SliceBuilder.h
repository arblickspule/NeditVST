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

//--------------------------------------------------------------------------
// Ghost-slice filtering (issue #5). The transient detector's rising-edge
// rule cannot fire AT the range start, and a resting final bar/beat leaves
// the last slice running to trimEnd -- both surface as sequencer rows the
// user calls "silent ghosts". This drops them from the derived slice list so
// the sequencer rows and the audio slice list stay in sync:
//
//   1) Leading sliver. When the loop's content starts exactly at trimStart,
//      the detector places the first real onset a few frames in and (because
//      the range start "plays the role of position 0") prepends a boundary at
//      trimStart, orphaning a tiny [trimStart, nearZero) slice. Any leading
//      slice STARTING at trimStart that is shorter than kLeadingSliverMaxMs
//      is dropped: its few frames are just the attack whose onset belongs at
//      the start anyway, and the following slice still contains them.
//
//   2) Silent tail. The final slice -- the one ending at trimEnd -- is the
//      whole span [lastOnset, trimEnd). When the loop's last bar/beat is a
//      rest (or a hit that decays to nothing well before trimEnd), that tail
//      is near-silent and reads as a tall empty top row. It is dropped when
//      its own RMS is below kSilentTailRatio x the sample's overall RMS over
//      the trim (a true rest reaches ~0; a final slice with real content,
//      however short, is well above the ratio and is kept).
//
// channels is the mono-summed (any channel order) source audio over
// [trimStart, trimEnd); only those frames are read. A degenerate input
// (no channels, wrong trim, empty list) returns the input unchanged.
//
// manualPoints (may be null) guards the leading-sliver merge: if the user
// explicitly placed a boundary inside the would-be sliver, that boundary is
// deliberate and the slice is left alone.
[[nodiscard]] std::vector<Slice> filterGhostSlices (const std::vector<Slice>& input,
                                                    const float* const* channels,
                                                    int numChannels,
                                                    std::int64_t trimStart,
                                                    std::int64_t trimEnd,
                                                    double sampleRate,
                                                    const std::vector<state::SamplePoint>* manualPoints);

// Config for the two ghost rules above (see each rule's comment).
inline constexpr double kLeadingSliverMaxMs = 15.0;
inline constexpr double kSilentTailRatio = 0.05;

} // namespace nedit::engine
