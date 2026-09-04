// Nedit -- State layer.
//
// Core value types and constant tables shared across the whole plugin.
// Pure C++ (no SDK / framework dependencies), fully serializable.
//
// The tables in this file are extracted verbatim from the original
// NeditVST (JUCE) implementation so the rewrite is behaviourally faithful:
// the 20-entry note-value palette, the 9 playback styles, and the small
// enum vocabularies used by style parameters.

#pragma once

#include <array>
#include <cstdint>

namespace nedit::state {

// ---------------------------------------------------------------------------
// Note-value palette
// ---------------------------------------------------------------------------
// Shared by: clock reference, subdivision probabilities, transient grid
// quantize, scratch rate, subdivide overrides, pattern-switch interval,
// performance trim grid + quantized recall. Sorted shortest to longest,
// capped at 1n (1nd deliberately excluded -- "no longer than 1 bar").
// Beats are quarter-note units.

struct NoteValueInfo
{
    const char* name;
    double beats;
};

inline constexpr int kNumNoteValues = 20;

inline constexpr std::array<NoteValueInfo, kNumNoteValues> kNoteValues { {
    { "128n", 1.0 / 32.0 },
    { "64n",  1.0 / 16.0 },
    { "32nt", 1.0 / 12.0 },
    { "64nd", 3.0 / 32.0 },
    { "32n",  1.0 / 8.0 },
    { "16nt", 1.0 / 6.0 },
    { "32nd", 3.0 / 16.0 },
    { "16n",  1.0 / 4.0 },
    { "8nt",  1.0 / 3.0 },
    { "16nd", 3.0 / 8.0 },
    { "8n",   1.0 / 2.0 },
    { "4nt",  2.0 / 3.0 },
    { "8nd",  3.0 / 4.0 },
    { "4n",   1.0 },
    { "2nt",  4.0 / 3.0 },
    { "4nd",  3.0 / 2.0 },
    { "2n",   2.0 },
    { "1nt",  8.0 / 3.0 },
    { "2nd",  3.0 },
    { "1n",   4.0 }
} };

// Well-known palette indices (used as defaults).
inline constexpr int kNoteValue16n = 7;
inline constexpr int kNoteValue4n  = 13;
inline constexpr int kNoteValue1n  = 19;

// Subdivision-variant grouping of the palette, by the name suffix: plain
// note values ("…n"), dotted ("…nd") and triplets ("…nt"). The editor's
// "n=0 / nd=0 / nt=0" quick-clears zero whole groups at once; 128n is the
// shortest plain value and has no dotted/triplet siblings.
enum class NoteValueVariant : std::uint8_t
{
    plain = 0,
    dotted = 1,
    triplet = 2
};

inline constexpr std::array<NoteValueVariant, kNumNoteValues> kNoteValueVariant { {
    NoteValueVariant::plain,    // 0:  128n
    NoteValueVariant::plain,    // 1:  64n
    NoteValueVariant::triplet,  // 2:  32nt
    NoteValueVariant::dotted,   // 3:  64nd
    NoteValueVariant::plain,    // 4:  32n
    NoteValueVariant::triplet,  // 5:  16nt
    NoteValueVariant::dotted,   // 6:  32nd
    NoteValueVariant::plain,    // 7:  16n
    NoteValueVariant::triplet,  // 8:  8nt
    NoteValueVariant::dotted,   // 9:  16nd
    NoteValueVariant::plain,    // 10: 8n
    NoteValueVariant::triplet,  // 11: 4nt
    NoteValueVariant::dotted,   // 12: 8nd
    NoteValueVariant::plain,    // 13: 4n
    NoteValueVariant::triplet,  // 14: 2nt
    NoteValueVariant::dotted,   // 15: 4nd
    NoteValueVariant::plain,    // 16: 2n
    NoteValueVariant::triplet,  // 17: 1nt
    NoteValueVariant::dotted,   // 18: 2nd
    NoteValueVariant::plain     // 19: 1n
} };

[[nodiscard]] constexpr bool isValidNoteValueIndex (int index) noexcept
{
    return index >= 0 && index < kNumNoteValues;
}

// ---------------------------------------------------------------------------
// Playback styles
// ---------------------------------------------------------------------------

enum class PlaybackStyle : std::uint8_t
{
    forward = 0,
    pingPong,
    tapeStop,
    stretch,
    filterDown,
    filterUp,
    bitcrush,
    scratch,
    flanger
};

inline constexpr int kNumPlaybackStyles = 9;

inline constexpr std::array<const char*, kNumPlaybackStyles> kPlaybackStyleNames { {
    "Forward", "Ping-Pong", "Tape Stop", "Stretch",
    "Filter Down", "Filter Up", "Bitcrush", "Scratch", "Flanger"
} };

[[nodiscard]] constexpr bool isValidPlaybackStyleIndex (int index) noexcept
{
    return index >= 0 && index < kNumPlaybackStyles;
}

[[nodiscard]] constexpr const char* playbackStyleName (PlaybackStyle style) noexcept
{
    return kPlaybackStyleNames[static_cast<std::size_t> (style)];
}

// ---------------------------------------------------------------------------
// Trigger modes (the five top-level modes)
// ---------------------------------------------------------------------------

enum class TriggerMode : std::uint8_t
{
    sliceLength = 0,  // Generate tab
    clock,            // Generate tab
    sequenced,        // Sequence tab
    performance,      // Perform tab
    control           // Control tab
};

inline constexpr int kNumTriggerModes = 5;

// ---------------------------------------------------------------------------
// Small enum vocabularies
// ---------------------------------------------------------------------------

enum class PitchMode : std::uint8_t { repitch = 0, timeStretch };
enum class GrainWindowShape : std::uint8_t { hann = 0, triangular };
enum class FilterType : std::uint8_t { lowPass = 0, highPass, bandPass };
enum class CurveShape : std::uint8_t { linear = 0, exponential };
enum class SweepMode : std::uint8_t { fixed = 0, sweepIn, sweepOut };
enum class VolumeRampMode : std::uint8_t { fixed = 0, rampUp, rampDown };
enum class WindowScope : std::uint8_t { wholeWindow = 0, perTick };
enum class TrimSnapMode : std::uint8_t { transients = 0, grid };
enum class PatternSwitchTiming : std::uint8_t { immediate = 0, setInterval, endOfPattern };

// Easing curves (Scratch v2) -- shape a 0..1 progress value.
enum class EasingCurve : std::uint8_t
{
    linear = 0,     // identity
    easeIn,         // slow start, accelerating (t*t)
    easeOut,        // fast start, decelerating (t*(2-t))
    easeInEaseOut   // smoothstep (3t^2 - 2t^3)
};

inline constexpr int kNumEasingCurves = 4;

inline constexpr std::array<const char*, kNumEasingCurves> kEasingCurveNames { {
    "Linear", "Ease In", "Ease Out", "Ease In-Out"
} };

inline constexpr std::array<const char*, 3> kSweepModeNames { {
    "Static", "Sweep In", "Sweep Out"
} };

inline constexpr std::array<const char*, 3> kVolumeRampModeNames { {
    "Static", "Ramp Up", "Ramp Down"
} };

inline constexpr std::array<const char*, 3> kFilterTypeNames { {
    "Low-pass", "High-pass", "Band-pass"
} };

inline constexpr std::array<const char*, 2> kCurveShapeNames { {
    "Linear", "Exponential"
} };

// Pattern-recall timing (the Sequencer page's pattern-switch timing). Order
// matches PatternSwitchTiming's numeric values (immediate 0, setInterval 1,
// endOfPattern 2).
inline constexpr std::array<const char*, 3> kPatternSwitchTimingNames { {
    "Immediate", "Set Interval", "End of Pattern"
} };

// ---------------------------------------------------------------------------
// Bar-count option tables
// ---------------------------------------------------------------------------

// Slice Length periodic reset ("Reset every ...").
inline constexpr std::array<int, 4> kResetBarsValues { { 1, 2, 4, 8 } };
inline constexpr int kDefaultResetBarsIndex = 2;  // 4 bars

// Sequenced pattern length (deliberately capped at 4 bars).
inline constexpr std::array<int, 3> kPatternLengthBarsValues { { 1, 2, 4 } };
inline constexpr int kDefaultPatternLengthBarsIndex = 0;  // 1 bar

// ---------------------------------------------------------------------------
// MIDI constants
// ---------------------------------------------------------------------------

inline constexpr int kNumMidiNotes = 128;
inline constexpr int kDefaultControlBaseNote = 36;  // C1

// ---------------------------------------------------------------------------
// Sequencer limits
// ---------------------------------------------------------------------------

inline constexpr int kMaxSequencerRows = 32;
inline constexpr int kMaxSequencerColumns = 256;

// Zoom bounds for the sequencer grid's persistent viewport (issue #2).
// Persisted (normalized) so they are clamped on load; the UI-layer geometry
// shares these constants so the sanitizer and the canvas can't drift.
inline constexpr double kMinSequencerZoom = 0.25;   // cells shrunk to 25%
inline constexpr double kMaxSequencerZoom = 8.0;    // cells grown to 8x

// ---------------------------------------------------------------------------
// Utility
// ---------------------------------------------------------------------------

template <typename T>
[[nodiscard]] constexpr T clampValue (T value, T lo, T hi) noexcept
{
    return value < lo ? lo : (value > hi ? hi : value);
}

} // namespace nedit::state
