// Nedit -- Engine layer.
//
// The shared per-sample voice path: turns "the pick currently in
// progress" into output samples, identically for every trigger mode.
// Composes the DSP primitives (SweepFilter, Bitcrusher, Flanger) and the
// GranularStretcher around the two pitch-mode read paths:
//
//   * direct-read (Repitch): one interpolated read pointer advancing at
//     playbackRate (varispeed -- pitch follows speed)
//   * granular (Time-Stretch pitch mode, and ALWAYS the Stretch style):
//     overlap-add grains, pitch independent of tempo
//
// plus the per-style behaviours, faithful to the original:
//   * fades: fade-in/out clamped to half the pick length; Ping-Pong and
//     Scratch get the same envelope around their bounce midpoint (curve-
//     shapeable); Tape Stop's gain rides its own decel curve REPLACING
//     the normal fade-out
//   * Tape Stop: rate multiplier ramping 1 -> 0 over its duration,
//     layered onto both read paths; once position exhausts the slice's
//     real content it freezes into a short looping window of real audio
//     (the "stuck tape") rather than reading past the slice or cutting
//     to silence
//   * Stretch: always granular, own grain size (hard-edged window), Grain
//     Speed as a fixed character constant; the declared pick length
//     governs duration, with the loop fold repeating the same pass
//   * Filter Down/Up: log-scale cutoff sweep 9k -> 250 Hz (mirrored for
//     Up), per-tick or whole-window progress
//   * Bitcrush: swept sample-rate reduction + bit depth (per-pick
//     progress); Flanger: swept delay/mix/feedback (whole-window
//     progress where a window exists); Volume: a constant per-style gain
//     (the style's own entry of StyleParameters::styleVolume)
//   * Control mode: velocity gain and gate-release fade (which force-
//     stops the pick at silence)
//
// The renderer owns per-pick runtime state ONLY (never serialized). The
// caller (a mode scheduler) decides WHEN picks start and what parameters
// they carry; this class decides what they sound like.

#pragma once

#include "GranularStretcher.h"
#include "dsp/Bitcrusher.h"
#include "dsp/Flanger.h"
#include "dsp/SweepFilter.h"

#include <state/StyleParameters.h>
#include <state/Types.h>

#include <cstdint>

namespace nedit::engine {

// Fixed sweep character constants (verified against the original).
inline constexpr float kFilterSweepStartHz = 9000.0f;
inline constexpr float kFilterSweepEndHz = 250.0f;
inline constexpr double kTapeStopFreezeLoopMs = 25.0;

// Per-block constants, computed once per processBlock by the caller.
struct BlockContext
{
    const float* const* source = nullptr;  // source channels
    int sourceChannels = 0;
    std::int64_t sourceFrames = 0;

    double hostSampleRate = 44100.0;
    double sourceSampleRate = 44100.0;

    double playbackRate = 1.0;       // (sourceRate/hostRate) * repitchRatio
    double srConversionRatio = 1.0;  // sourceRate/hostRate
    double pitchRatio = 1.0;         // 2^(pitchShiftSemitones/12)

    // Time-Stretch pitch-mode granular settings.
    bool timeStretchMode = false;
    double grainSizeHostSamples = 0.0;
    double grainSpeed = 1.0;       // re-granulation density (1.0 = clean rate-matching)
    double outputHopSamples = 0.0;   // grainSize * 0.5 (fixed 50% overlap)
    double sourceHopSamples = 0.0;   // outputHop * srConversion * repitchRatio
    GranularStretcher::WindowShape grainWindowShape = GranularStretcher::WindowShape::hann;

    double fadeInSamplesRequested = 0.0;
    double fadeOutSamplesRequested = 0.0;
};

// Everything captured at pick start. The scheduler builds this from
// state (+ per-step overrides) at the moment the pick triggers.
struct PickParams
{
    state::PlaybackStyle style = state::PlaybackStyle::forward;

    std::int64_t sliceStartFrame = 0;
    std::int64_t sliceLengthFrames = 0;
    std::int64_t schedulingEndFrame = 0;   // may exceed the source for bounce/stretch

    double pickLengthHostSamples = 0.0;    // authoritative pick duration
    double tapeStopDurationHostSamples = 0.0;
    double midpointHostSamples = 0.0;      // bounce turnaround (host samples)
    double scratchCycleLengthHostSamples = 0.0;

    bool beatQuantized = false;            // Slice Length Forward/Ping-Pong only
    double quantizedStretchRatio = 1.0;

    bool nativeRate = false;               // Performance Sync-off
    bool halfSliceFold = false;            // Sequenced-mode Ping-Pong
    bool useDurationGate = false;          // Sequenced mode: gate on declared length

    // Whole-window progress flags (the scheduler knows the mode):
    bool filterWholeWindow = false;
    bool flangerWholeWindow = false;

    // Sequencer per-cell volume ramp (value + mode), set only when a cell
    // overrides its style's volume; else the renderer uses the style's own
    // `styleVolume[key]`.
    bool volumeRampActive = false;
    bool volumeWholeWindow = false;
    float volumeValue = 1.0f;
    state::VolumeRampMode volumeMode = state::VolumeRampMode::fixed;

    double velocityGain = 1.0;             // Control mode

    // Captured parameter values (globals or per-step overrides).
    state::StyleParameters params;
};

class PickRenderer
{
public:
    // NOT audio-thread safe (allocates the flanger line). Call at setup.
    void prepare (double hostSampleRate);

    // Begin a new pick: captures params, resets all per-pick DSP state
    // (filter, bitcrush hold, flanger line, granular grains), zeroes the
    // pick clocks.
    void startPick (const PickParams& newPick);

    // Stop rendering (scheduler decision -- mode switch, note-off, etc.).
    void clearPick() noexcept { active = false; gateReleaseActive = false; }

    // Control-mode Gate: arm the additional fade-to-silence ramp. The
    // pick force-stops itself when the ramp completes.
    void beginGateRelease() noexcept
    {
        gateReleaseActive = true;
        gateReleaseElapsedSamples = 0.0;
    }

    // Window clock (Clock-mode windows / Sequenced-mode steps): the
    // scheduler starts a window and ticks it EVERY sample the window is
    // open -- including silence between a short pick's natural end and
    // the next tick -- so whole-window sweeps track wall-clock time.
    void startWindow (double windowLengthHostSamples) noexcept
    {
        windowLengthHost = windowLengthHostSamples;
        samplesSinceWindowStart = 0.0;
    }

    void tickWindowClock() noexcept { samplesSinceWindowStart += 1.0; }

    [[nodiscard]] double windowElapsedSamples() const noexcept { return samplesSinceWindowStart; }
    [[nodiscard]] double windowLengthSamples() const noexcept { return windowLengthHost; }

    // Sequenced-mode Subdivide retrigger: restarts THE SAME pick (same
    // slice/style/params) from its first frame with new durations --
    // deliberately WITHOUT resetting per-pick DSP state or the window
    // clock, so a subdivided step's Filter Sweep glides once across the
    // whole step while the retriggers happen underneath (the original
    // mutates its live fields in exactly this way).
    void retrigger (std::int64_t sliceStartFrame, double newPickLengthHostSamples,
                    double newTapeStopDurationHostSamples) noexcept
    {
        currentPosition = static_cast<double> (sliceStartFrame);
        samplesSincePick = 0.0;
        pick.pickLengthHostSamples = newPickLengthHostSamples;
        pick.tapeStopDurationHostSamples = newTapeStopDurationHostSamples;
        active = true;
    }

    // Render one host sample: ADDS the pick's contribution into
    // outSampleAdd[ch][0] for each channel ch (each pointer targets this
    // sample's slot in that channel's plane). Returns true if the pick
    // rendered (still within its schedule), false if it produced silence.
    bool renderSample (const BlockContext& ctx, float* const* outSampleAdd,
                       int numOutChannels);

    // Scheduler queries.
    [[nodiscard]] bool hasPick() const noexcept { return active; }
    [[nodiscard]] double position() const noexcept { return currentPosition; }
    [[nodiscard]] double samplesSincePickStart() const noexcept { return samplesSincePick; }
    [[nodiscard]] const PickParams& currentPick() const noexcept { return pick; }

    // True once the pick has run its course (the scheduler's completion /
    // self-chaining signal -- mirrors the original's pickWithinSchedule
    // going false).
    [[nodiscard]] bool finished (const BlockContext& ctx) const noexcept;

private:
    [[nodiscard]] double computeGain (const BlockContext& ctx, bool bounceActive,
                                      bool tapeStopActive, double tapeStopRateMultiplier) const noexcept;

    PickParams pick;
    bool active = false;

    // Per-pick runtime (never serialized).
    double currentPosition = 0.0;     // unbounded source-frame march
    double samplesSincePick = 0.0;

    // Window clock (Clock windows / Sequenced steps).
    double samplesSinceWindowStart = 0.0;
    double windowLengthHost = 0.0;

    // Control-mode gate release.
    bool gateReleaseActive = false;
    double gateReleaseElapsedSamples = 0.0;

    // DSP (reset per pick).
    dsp::SweepFilter filter;
    dsp::Bitcrusher bitcrusher;
    dsp::Flanger flanger;
    GranularStretcher stretcher;
};

} // namespace nedit::engine
