// Nedit -- State layer.
//
// Generate tab state: the two free-running generative modes (Slice Length
// and Clock) and everything they draw from.
//
// Fixes over the original:
//   * styleWeights here belongs to Generate ONLY. The sequencer's
//     Randomize has its own weight table (SequencerState::
//     randomizeStyleWeights) instead of silently sharing this one.
//   * styleParams here is Generate's own parameter set; Control mode and
//     the sequencer fallback own separate copies.
//   * generateMode (which of the two modes the Generate tab hosts) is
//     model state, not editor-local state, so it persists.

#pragma once

#include "StyleParameters.h"
#include "Types.h"

#include <array>
#include <vector>

namespace nedit::state {

struct GenerateState
{
    // Which sub-mode the Generate tab uses when the top-level TriggerMode
    // points at Generate.
    TriggerMode generateMode = TriggerMode::sliceLength;  // sliceLength or clock only

    // Per-slice draw weights, parallel to the DERIVED slice list.
    // 0 = never picked. Reset to all-1.0 whenever the slice list is
    // rebuilt (documented reset semantics -- slices have no stable
    // identity across boundary edits; revisit post-Phase-1 if weight
    // survival across edits becomes a requirement).
    std::vector<float> sliceWeights;

    // Per-style draw weights (both Generate modes). Default: Forward only.
    std::array<float, kNumPlaybackStyles> styleWeights { { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                                           0.0f, 0.0f, 0.0f, 0.0f } };

    // Generate's own style parameter values.
    StyleParameters styleParams;

    // --- Slice Length mode ---------------------------------------------------
    int resetBarsIndex = kDefaultResetBarsIndex;  // index into kResetBarsValues

    // --- Clock mode ------------------------------------------------------------
    int clockReferenceIndex = kNoteValue4n;       // outer window length (note value)

    // Weighted draw of the retrigger rate per window, one weight per
    // note value. Default all 1.0.
    std::array<float, kNumNoteValues> subdivisionWeights = makeUnitWeights();

    // Whether Tape Stop / Filter sweeps span the whole window or each tick.
    // Opposite defaults are deliberate (matches the original).
    WindowScope tapeStopScope = WindowScope::wholeWindow;
    WindowScope filterSweepScope = WindowScope::perTick;

    [[nodiscard]] static constexpr std::array<float, kNumNoteValues> makeUnitWeights() noexcept
    {
        std::array<float, kNumNoteValues> weights {};
        for (auto& w : weights)
            w = 1.0f;
        return weights;
    }

    void sanitize() noexcept
    {
        if (generateMode != TriggerMode::sliceLength && generateMode != TriggerMode::clock)
            generateMode = TriggerMode::sliceLength;

        for (auto& w : sliceWeights)
            w = clampValue (w, 0.0f, 1.0f);

        for (auto& w : styleWeights)
            w = clampValue (w, 0.0f, 1.0f);

        for (auto& w : subdivisionWeights)
            w = clampValue (w, 0.0f, 1.0f);

        styleParams.sanitize();

        resetBarsIndex = clampValue (resetBarsIndex, 0,
                                     static_cast<int> (kResetBarsValues.size()) - 1);

        if (! isValidNoteValueIndex (clockReferenceIndex))
            clockReferenceIndex = kNoteValue4n;
    }

    bool operator== (const GenerateState&) const = default;
};

} // namespace nedit::state
