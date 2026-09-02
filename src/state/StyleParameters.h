// Nedit -- State layer.
//
// The playback-style parameter table: the 21-parameter vocabulary shared
// (as a *type*, not as an instance!) by every scope that stores style
// parameters. In the original JUCE code a single global set of these was
// silently shared by Generate, Control, and every sequencer step without
// an override -- a documented pitfall. In the rewrite each scope owns its
// own StyleParameters instance:
//
//   * GenerateState.styleParams       -- the Generate/Sequence tab slider
//                                        surface; ALSO the merged base for
//                                        a sequencer step without a per-cell
//                                        override (so the on-screen sliders
//                                        are what the sequencer speaks with)
//   * SequencerState.fallbackParams   -- carried + serialized but no longer
//                                        the audio default (lead-dev 2026-08-30)
//   * ControlState.styleParams        -- Control mode's parameter panel
//   * PerformanceSnapshot.params      -- one per performance slot (as before)
//
// Generic (id-indexed) access exists for per-step overrides, randomizers
// and generic UI panels; typed field access exists for the engine.

#pragma once

#include "Types.h"

#include <cstdint>
#include <optional>

namespace nedit::state {

// Parameter ids. Order and numbering match the original sequencer-cell
// parameter table (the original keyed overrides by *name string*; the
// rewrite keys by this id).
enum class StyleParamId : std::uint8_t
{
    filterResonance = 0,      //  0  Resonance          0.5 .. 10     (Filter Down/Up)
    filterType,               //  1  Filter Type        LP/HP/BP      (Filter Down/Up)
    curveShape,               //  2  Curve Shape        Linear/Exp    (Ping-Pong, Tape Stop)
    grainSizeMs,              //  3  Grain Size         5 .. 30 ms    (Stretch)
    grainSpeed,               //  4  Grain Speed        1 .. 8 x      (Stretch)
    subdivide,                //  5  Subdivide          Off + 20 note values (all styles)
    srReduction,              //  6  Sample Rate Reduction  1 .. 48 samples (Bitcrush)
    srReductionMode,          //  7  SRR Mode           Static/Sweep In/Sweep Out
    bitDepth,                 //  8  Bit Depth          1 .. 16 bits  (Bitcrush)
    bitDepthMode,             //  9  Bit Depth Mode     Static/Sweep In/Sweep Out
    scratchRate,              // 10  Rate               20 note values (Scratch)
    scratchForwardCurve,      // 11  Forward Curve      4 easing curves (Scratch)
    scratchBackwardCurve,     // 12  Backward Curve     4 easing curves (Scratch)
    flangerDelayMs,           // 13  Delay Time         0.5 .. 10 ms  (Flanger)
    flangerDelayMode,         // 14  Delay Time Mode    Static/Sweep In/Sweep Out
    flangerMix,               // 15  Mix                0 .. 1        (Flanger)
    flangerMixMode,           // 16  Mix Mode           Static/Sweep In/Sweep Out
    flangerFeedback,          // 17  Feedback           0 .. 0.88     (Flanger)
    flangerFeedbackMode,      // 18  Feedback Mode      Static/Sweep In/Sweep Out
    volume                    // 19  Volume             0 .. 1 (per-style; reserved
                              //     override key -- see below)
};

// Volume is per-style (a 9-value `styleVolume` array, see below) and has NO
// scalar generic get/set. `volume` (19) exists in the enum ONLY as the
// sequencer per-cell override key: a cell may override its own style's
// volume, stored in the same sparse override map as the scalar params. The
// generic vocabulary (`get`/`set`, automation, serialization's scalar loop,
// `sanitize`'s scalar loop) covers ids 0 .. kNumStyleParams-1 = 0..18 only.
inline constexpr int kNumStyleParams = 19;
inline constexpr int kNumStyleParamIds = 20;   // scalars + the reserved volume key



[[nodiscard]] constexpr bool isValidStyleParamId (int id) noexcept
{
    return id >= 0 && id < kNumStyleParams;
}

// Valid as a sequencer per-cell override key: the generic scalar params plus
// the reserved per-style Volume key (which a cell overrides on top of its
// style's own `styleVolume`).
[[nodiscard]] constexpr bool isValidSequencerOverrideId (int id) noexcept
{
    return (id >= 0 && id < kNumStyleParams) || id == static_cast<int> (StyleParamId::volume);
}

// Static description of one parameter (name, range, discreteness).
struct StyleParamInfo
{
    const char* name;
    float minValue;
    float maxValue;
    float defaultValue;
    bool discrete;        // rendered as an option list rather than a slider
    bool steppedSlider;   // discrete but rendered as a stepped slider (Subdivide)
    bool swept;           // has a paired sweep-mode parameter (the next id)
    int numOptions;       // discrete only; 0 otherwise
};

[[nodiscard]] const StyleParamInfo& styleParamInfo (StyleParamId id) noexcept;

// Option display name for a discrete parameter; nullptr if out of range /
// not discrete.
[[nodiscard]] const char* styleParamOptionName (StyleParamId id, int optionIndex) noexcept;

// Which parameters a given style exposes (Subdivide and Volume are always
// appended -- Subdivide applies to every style, and Volume is the per-cell
// override key layered on that style's own volume). Returns the ids in menu
// order. count is written with the number of valid entries; the array is
// large enough for the worst case.
struct ApplicableParams
{
    std::array<StyleParamId, 8> ids {};
    int count = 0;
};

[[nodiscard]] ApplicableParams applicableStyleParams (PlaybackStyle style) noexcept;

// ---------------------------------------------------------------------------
// The value set itself
// ---------------------------------------------------------------------------

struct StyleParameters
{
    // Filter Down / Filter Up
    float filterResonance = 2.0f;                        // 0.5 .. 10
    FilterType filterType = FilterType::lowPass;

    // Ping-Pong turnaround fade / Tape Stop decel
    CurveShape curveShape = CurveShape::linear;

    // Stretch
    float grainSizeMs = 10.0f;                           // 5 .. 30
    float grainSpeed = 4.0f;                             // 1 .. 8

    // All styles: retrigger rate; 0 = Off, 1..20 = note value index + 1
    int subdivide = 0;

    // Bitcrush
    float srReduction = 12.0f;                           // 1 .. 48 samples
    SweepMode srReductionMode = SweepMode::fixed;
    float bitDepth = 5.0f;                               // 1 .. 16 bits
    SweepMode bitDepthMode = SweepMode::fixed;

    // Scratch
    int scratchRate = kNoteValue16n;                     // note value index
    EasingCurve scratchForwardCurve = EasingCurve::easeInEaseOut;
    EasingCurve scratchBackwardCurve = EasingCurve::easeInEaseOut;

    // Flanger
    float flangerDelayMs = 2.0f;                         // 0.5 .. 10
    SweepMode flangerDelayMode = SweepMode::fixed;
    float flangerMix = 0.5f;                             // 0 .. 1
    SweepMode flangerMixMode = SweepMode::fixed;
    float flangerFeedback = 0.3f;                        // 0 .. 0.88
    SweepMode flangerFeedbackMode = SweepMode::fixed;

    // Per-style gain stage (0 .. 1), applied after each style's own DSP.
    // One value per PlaybackStyle, indexed by the style's ordinal. A single
    // shared "Volume" param used to live here; the lead-dev made it
    // per-style (issue #7) and dropped the ramp-mode vocabulary entirely.
    std::array<float, kNumPlaybackStyles> styleVolume = makeUnitStyleVolume();

    [[nodiscard]] static constexpr std::array<float, kNumPlaybackStyles>
    makeUnitStyleVolume() noexcept
    {
        std::array<float, kNumPlaybackStyles> volumes {};
        volumes.fill (1.0f);
        return volumes;
    }

    // Per-style volume access (clamped on write).
    [[nodiscard]] float getStyleVolume (PlaybackStyle style) const noexcept;
    void setStyleVolume (PlaybackStyle style, float value) noexcept;

    // Generic access (per-step overrides, randomizers, generic panels).
    // Discrete parameters are represented as their option index cast to
    // float, matching the original override storage convention.
    [[nodiscard]] float get (StyleParamId id) const noexcept;

    // Sets a parameter, clamping to its valid range (rounding for
    // discrete parameters).
    void set (StyleParamId id, float value) noexcept;

    // Clamp every field to its documented range.
    void sanitize() noexcept;

    bool operator== (const StyleParameters&) const = default;
};

// Range constants referenced by both the info table and sanitize().
inline constexpr float kMinFilterResonance = 0.5f;
inline constexpr float kMaxFilterResonance = 10.0f;
inline constexpr float kMinGrainSizeMs = 5.0f;
inline constexpr float kMaxGrainSizeMs = 30.0f;
inline constexpr float kMinGrainSpeed = 1.0f;
inline constexpr float kMaxGrainSpeed = 8.0f;
inline constexpr float kMinSrReduction = 1.0f;
inline constexpr float kMaxSrReduction = 48.0f;   // Sweep In/Out fixed extreme
inline constexpr float kMinBitDepth = 1.0f;       // Sweep In/Out fixed extreme
inline constexpr float kMaxBitDepth = 16.0f;
inline constexpr float kMinFlangerDelayMs = 0.5f;
inline constexpr float kMaxFlangerDelayMs = 10.0f;  // Sweep In/Out fixed extreme
inline constexpr float kMaxFlangerMix = 1.0f;       // Sweep In/Out fixed extreme
inline constexpr float kMaxFlangerFeedback = 0.88f; // short of self-oscillation

} // namespace nedit::state
