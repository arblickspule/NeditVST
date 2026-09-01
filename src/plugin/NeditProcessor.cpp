// NeditProcessor -- see the header for the wiring overview.

#include "plugin/NeditProcessor.h"

#include "plugin/NeditEditor.h"
#include "state/Serialization.h"

#include "base/source/fstreamer.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ustring.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"

#include <engine/SequenceRandomizer.h>
#include <engine/Tempo.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <random>
namespace nedit::plugin {

using Steinberg::int32;
using Steinberg::tresult;
using Steinberg::TBool;
using Steinberg::IBStream;
namespace Vst = Steinberg::Vst;

namespace {

constexpr auto kOk = Steinberg::kResultOk;
constexpr auto kFalse = Steinberg::kResultFalse;

// Point vectors grow on the UI thread; reserve headroom so a mid-block
// snapshot copy into automationScratch_ (audio thread) reuses capacity
// in steady state instead of allocating.
constexpr std::size_t kReservedSamplePoints = 1024;

// Declick fade slider range (ms). Small by design: attack/release are
// only ever a declick utility, so the range is 0..10 ms for fine control.
constexpr float kMaxFadeMs = 10.0f;

} // namespace

//------------------------------------------------------------------------
void clipSlicesToTrim (const std::vector<engine::Slice>& slices,
                       std::int64_t trimStart, std::int64_t trimEnd,
                       const std::vector<float>& sliceWeights,
                       std::vector<engine::Slice>& outSlices,
                       std::vector<float>& outWeights)
{
    outSlices.clear();
    outWeights.clear();

    for (std::size_t i = 0; i < slices.size(); ++i)
    {
        const auto& sl = slices[i];
        if (sl.endFrame <= trimStart || sl.startFrame >= trimEnd)
            continue;   // wholly outside the trim (soft-hidden by the view)

        const std::int64_t clipStart = std::max (sl.startFrame, trimStart);
        const std::int64_t clipEnd = std::min (sl.endFrame, trimEnd);
        if (clipEnd <= clipStart)
            continue;   // degenerate overlap -- never selectable

        outSlices.push_back ({ clipStart, clipEnd });
        outWeights.push_back ((i < sliceWeights.size()) ? sliceWeights[i] : 1.0f);
    }
}

//------------------------------------------------------------------------
NeditProcessor::NeditProcessor()
    : provider_ (uiState_)
{
    automationScratch_.sample.manualPoints.reserve (kReservedSamplePoints);
    automationScratch_.sample.excludedPoints.reserve (kReservedSamplePoints);
    trimSlices_.reserve (128);
    trimWeights_.reserve (128);
}

//------------------------------------------------------------------------
tresult PLUGIN_API NeditProcessor::initialize (Steinberg::FUnknown* context)
{
    const tresult result = SingleComponentEffect::initialize (context);

    if (result != kOk)
        return result;

    // Output-only audio effect with MIDI note input (the original's shape:
    // it renders sliced sample material, nothing streams through).
    addAudioOutput (STR16 ("Stereo Out"), Vst::SpeakerArr::kStereo);
    addEventInput (STR16 ("MIDI Notes"), 1);

    // Transport facts the engine actually consumes.
    processContextRequirements.flags
        = Vst::IProcessContextRequirements::kNeedTransportState
       | Vst::IProcessContextRequirements::kNeedTempo
       | Vst::IProcessContextRequirements::kNeedProjectTimeMusic;

    registerParameters();
    return kOk;
}

//------------------------------------------------------------------------
tresult PLUGIN_API NeditProcessor::terminate()
{
    return SingleComponentEffect::terminate();
}

//------------------------------------------------------------------------
void NeditProcessor::registerParameters()
{
    const auto flags = Vst::ParameterInfo::kCanAutomate;

    const auto addParam = [this] (const char* title, int stepCount,
                                  double defaultNorm, Vst::ParamID id) {
        // STR16 only pastes string literals; runtime titles need a real
        // UTF-16 conversion.
        Steinberg::char16 titleBuf[128];
        Steinberg::UString (titleBuf, 128).fromAscii (title);
        parameters.addParameter (titleBuf, nullptr, stepCount,
                                 defaultNorm, flags,
                                 static_cast<Steinberg::int32> (id));
    };

    for (std::uint32_t id = 0; id <= kLastStyleParamId; ++id)
        addParam (titleFor (id), stepCountFor (id),
                  static_cast<double> (defaultNormalized (id)),
                  static_cast<Vst::ParamID> (id));

    for (const std::uint32_t id : { kParamTriggerMode, kParamManualTempoEnabled,
                                    kParamManualTempoBpm, kParamLoopLengthBars,
                                    kParamControlBaseNote, kParamControlGateMode,
                                    kParamQuantizeRecallEnabled,
                                    kParamQuantizeRecallInterval })
        addParam (titleFor (id), stepCountFor (id),
                  static_cast<double> (defaultNormalized (id)),
                  static_cast<Vst::ParamID> (id));
}

//------------------------------------------------------------------------
tresult PLUGIN_API NeditProcessor::setActive (TBool state)
{
    if (state)
        scheduler_.prepare (processSetup.sampleRate > 0 ? processSetup.sampleRate
                                                        : 44100.0);
    return SingleComponentEffect::setActive (state);
}

//------------------------------------------------------------------------
tresult PLUGIN_API NeditProcessor::setProcessing (TBool state)
{
    if (! state)
        lastBlockEndPpq_ = 0.0;
    return kOk;
}

//------------------------------------------------------------------------
tresult PLUGIN_API NeditProcessor::setParamNormalized (
    Vst::ParamID id, Vst::ParamValue normalized)
{
    if (! isValidParamId (static_cast<std::uint32_t> (id)))
        return Steinberg::kInvalidArgument;

    applyNormalized (uiState_, static_cast<std::uint32_t> (id),
                                       static_cast<float> (normalized));
    provider_.publish (uiState_);

    return SingleComponentEffect::setParamNormalized (id, normalized);
}

//------------------------------------------------------------------------
tresult PLUGIN_API NeditProcessor::getState (IBStream* stream)
{
    if (stream == nullptr)
        return Steinberg::kInvalidArgument;

    const auto bytes = state::serialize (uiState_);

    if (bytes.empty())
        return Steinberg::kInternalError;

    const auto size = static_cast<Steinberg::int32> (bytes.size());

    // SDK write() takes a non-const void*.
    auto* bytesPtr = const_cast<void*> (static_cast<const void*> (bytes.data()));

    return stream->write (bytesPtr, size) == kOk ? kOk : Steinberg::kInternalError;
}

//------------------------------------------------------------------------
tresult PLUGIN_API NeditProcessor::setState (IBStream* stream)
{
    if (stream == nullptr)
        return Steinberg::kInvalidArgument;

    // Read everything the stream offers (our chunk is self-delimiting by
    // section tags; trailing host junk is ignored by deserialize).
    std::vector<std::uint8_t> bytes;
    std::array<std::uint8_t, 4096> buffer {};

    while (true)
    {
        Steinberg::int32 numRead = 0;

        if (stream->read (buffer.data(), static_cast<Steinberg::int32> (buffer.size()),
                          &numRead)
            != kOk)
            break;

        if (numRead <= 0)
            break;

        bytes.insert (bytes.end(), buffer.begin(), buffer.begin() + numRead);
    }

    auto parsed = state::deserialize (bytes);

    if (! parsed.has_value())
        return kFalse;   // not ours / corrupt: keep current state untouched

    uiState_ = *parsed;
    syncParameterObjectsFromState();
    provider_.publish (uiState_);
    return kOk;
}

//------------------------------------------------------------------------
void NeditProcessor::syncParameterObjectsFromState()
{
    // Hosts read back display values through getParamNormalized, which
    // reads the Parameter OBJECTS -- after a chunk load they must be
    // pushed from the loaded state (the parent stores normalized values).
    const auto sync = [this] (std::uint32_t id) {
        SingleComponentEffect::setParamNormalized (
            static_cast<Vst::ParamID> (id),
            static_cast<Vst::ParamValue> (toNormalized (uiState_, id)));
    };

    for (std::uint32_t id = 0; id <= kLastStyleParamId; ++id)
        sync (id);

    for (const std::uint32_t id : { kParamTriggerMode, kParamManualTempoEnabled,
                                    kParamManualTempoBpm, kParamLoopLengthBars,
                                    kParamControlBaseNote, kParamControlGateMode,
                                    kParamQuantizeRecallEnabled,
                                    kParamQuantizeRecallInterval })
        sync (id);
}

//------------------------------------------------------------------------
bool NeditProcessor::requestSampleLoad (const std::string& path)
{
    auto result = sampleManager_.loadFile (path, uiState_.sample);
    if (! result)
        return false;

    uiState_.sample = result->updated;
    // (The audition cursor is audio-thread-owned; renderAudition wraps it
    // into the new trim span on the next block, no reset needed here.)
    // Slice weights are parallel to the derived slice list and reset when
    // slices rebuild -- fresh analysis means equal probability everywhere.
    uiState_.generate.sliceWeights.assign (result->sample->slices.size(), 1.0f);
    // Establish the sequencer working-grid dimensions from the new slice
    // count (rows) + the current step resolution / pattern length (columns).
    resizeSequencerGridForSample();
    provider_.publish (uiState_);
    return true;
}

//------------------------------------------------------------------------
bool NeditProcessor::resizeSequencerGridForSample()
{
    const auto loaded = sampleManager_.acquire();
    const int sliceCount = loaded != nullptr ? static_cast<int> (loaded->slices.size()) : 0;
    const auto dims = engine::seq::computeSequencerDims (
        sliceCount, uiState_.sequencer.stepResolutionIndex,
        uiState_.sequencer.patternLengthBarsIndex);
    return engine::seq::resizeGrid (uiState_.sequencer, dims);
}

//------------------------------------------------------------------------
void NeditProcessor::setVisibleWindow (double startNorm, double endNorm)
{
    uiState_.ui.visibleStartNorm = startNorm;
    uiState_.ui.visibleEndNorm = endNorm;
    uiState_.ui.sanitize();
    provider_.publish (uiState_);
}

//------------------------------------------------------------------------
void NeditProcessor::setActiveTab (state::UiTab tab)
{
    if (static_cast<unsigned> (tab) > 3)
        return;
    uiState_.ui.activeTab = tab;
    // Tab drives the scheduler-facing mode: selecting a tab transfers audio
    // control to that mode (Generate keeps its remembered sliceLength/clock
    // sub-choice). This is what makes clicking SEQUENCER actually run the
    // sequenced scheduler. tab<->mode round-trips through the state helpers.
    uiState_.triggerMode = state::triggerModeForTab (tab, uiState_.generate.generateMode);
    provider_.publish (uiState_);
}

//------------------------------------------------------------------------
void NeditProcessor::setTrimFrames (std::int64_t startFrame, std::int64_t endFrame)
{
    uiState_.sample.trimStartFrame = startFrame;
    uiState_.sample.trimEndFrame = endFrame;
    uiState_.sample.sanitize();
    provider_.publish (uiState_);
}

//------------------------------------------------------------------------
void NeditProcessor::setStyleWeight (int styleIndex, float weight)
{
    if (! state::isValidPlaybackStyleIndex (styleIndex))
        return;
    uiState_.generate.styleWeights[static_cast<std::size_t> (styleIndex)]
        = std::clamp (weight, 0.0f, 1.0f);
    provider_.publish (uiState_);
}

//------------------------------------------------------------------------
float NeditProcessor::getSliceProbability (int sliceIndex) const
{
    const auto& w = uiState_.generate.sliceWeights;
    if (sliceIndex < 0 || sliceIndex >= static_cast<int> (w.size()))
        return 1.0f;
    return w[static_cast<std::size_t> (sliceIndex)];
}

//------------------------------------------------------------------------
void NeditProcessor::setSliceProbability (int sliceIndex, float weight)
{
    auto& w = uiState_.generate.sliceWeights;
    if (sliceIndex < 0 || sliceIndex >= static_cast<int> (w.size()))
        return;
    w[static_cast<std::size_t> (sliceIndex)] = std::clamp (weight, 0.0f, 1.0f);
    provider_.publish (uiState_);
}

//------------------------------------------------------------------------
std::int64_t NeditProcessor::resolveManualFrame (std::int64_t frame, bool snap) const
{
    const auto& sample = uiState_.sample;
    const std::int64_t trimStart = sample.trimStartFrame;
    const std::int64_t trimEnd = sample.trimEndFrame;
    std::int64_t pos = std::clamp (frame, trimStart, std::max (trimStart, trimEnd - 1));

    if (snap)
    {
        const auto radius = static_cast<std::int64_t> (
            static_cast<double> (engine::kManualSnapRadiusMs) / 1000.0 * sample.sampleSampleRate);
        pos = sampleManager_.snapToTransient (pos, radius, trimStart, trimEnd);
    }

    return pos;
}

//------------------------------------------------------------------------
std::int32_t NeditProcessor::addManualPoint (std::int64_t frame, bool snap)
{
    auto& sample = uiState_.sample;
    if (! sample.hasSample() || sample.sampleLengthFrames <= 0)
        return -1;

    const std::int64_t pos = resolveManualFrame (frame, snap);
    const std::int32_t id = sample.nextManualPointId++;
    sample.manualPoints.push_back ({ id, pos });

    rebuildSlicesPreservingWeights();
    return id;
}

//------------------------------------------------------------------------
void NeditProcessor::moveManualPoint (std::int32_t id, std::int64_t frame, bool snap)
{
    auto& sample = uiState_.sample;
    if (! sample.hasSample() || sample.sampleLengthFrames <= 0)
        return;

    auto it = std::find_if (sample.manualPoints.begin(), sample.manualPoints.end(),
                            [id] (const state::SamplePoint& mp) { return mp.id == id; });
    if (it == sample.manualPoints.end())
        return;

    const std::int64_t pos = resolveManualFrame (frame, snap);
    if (it->position == pos)
        return;   // unchanged -> skip the rebuild + publish

    it->position = pos;
    rebuildSlicesPreservingWeights();
}

//------------------------------------------------------------------------
bool NeditProcessor::removeManualPoint (std::int32_t id)
{
    auto& mps = uiState_.sample.manualPoints;
    const auto before = mps.size();
    mps.erase (std::remove_if (mps.begin(), mps.end(),
                               [id] (const state::SamplePoint& mp) { return mp.id == id; }),
               mps.end());

    if (mps.size() == before)
        return false;   // nothing removed

    rebuildSlicesPreservingWeights();
    return true;
}

//------------------------------------------------------------------------
bool NeditProcessor::excludeNearestAutoPoint (std::int64_t frame)
{
    auto& sample = uiState_.sample;
    if (! sample.hasSample() || sample.sampleLengthFrames <= 0)
        return false;

    const auto loaded = sampleManager_.acquire();
    if (loaded == nullptr || loaded->analysis == nullptr)
        return false;

    // Re-run detection at the CURRENT sensitivity+holdoff (exclusions are
    // applied later during the merge, so this still lists already-excluded
    // onsets -- the visible boundary disappears after the first exclusion,
    // so it can't normally be re-clicked).
    const auto autoSlices = loaded->analysis->detectSlices (
        sample.sensitivity, engine::tempo::minimumHoldoffMs (sample),
        sample.trimStartFrame, sample.trimEndFrame);

    std::int64_t nearest = -1;
    std::int64_t bestDist = std::numeric_limits<std::int64_t>::max();
    for (const auto& s : autoSlices)
    {
        if (s.startFrame == sample.trimStartFrame)
            continue;   // the trim start is never excludable
        const std::int64_t d = std::llabs (s.startFrame - frame);
        if (d < bestDist)
        {
            bestDist = d;
            nearest = s.startFrame;
        }
    }
    if (nearest < 0)
        return false;

    // Defensive dedup: if this raw onset already matches an exclusion, leave
    // state untouched (no rebuild, no duplicate exclusion points).
    const auto matchToleranceFrames = static_cast<std::int64_t> (
        static_cast<double> (engine::kManualSnapRadiusMs) / 1000.0 * sample.sampleSampleRate);
    for (const auto& ep : sample.excludedPoints)
        if (std::llabs (ep.position - nearest) <= matchToleranceFrames)
            return false;

    sample.excludedPoints.push_back ({ sample.nextExcludedPointId++, nearest });
    rebuildSlicesPreservingWeights();
    return true;
}

//------------------------------------------------------------------------
void NeditProcessor::setSensitivity (float value)
{
    if (! uiState_.sample.hasSample())
        return;
    uiState_.sample.sensitivity = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
    rebuildSlicesPreservingWeights();
}

//------------------------------------------------------------------------
void NeditProcessor::setQuantizeTransients (bool on)
{
    if (! uiState_.sample.hasSample())
        return;
    uiState_.sample.quantizeTransients = on;
    rebuildSlicesPreservingWeights();
}

//------------------------------------------------------------------------
void NeditProcessor::setQuantizeGrid (int gridIndex)
{
    if (! uiState_.sample.hasSample())
        return;
    if (! state::isValidNoteValueIndex (gridIndex))
        return;
    uiState_.sample.quantizeGridIndex = gridIndex;
    rebuildSlicesPreservingWeights();
}

//------------------------------------------------------------------------
void NeditProcessor::setFadeInMs (float ms)
{
    uiState_.render.fadeInMs = ms < 0.0f ? 0.0f : (ms > kMaxFadeMs ? kMaxFadeMs : ms);
    provider_.publish (uiState_);
}

//------------------------------------------------------------------------
void NeditProcessor::setFadeOutMs (float ms)
{
    uiState_.render.fadeOutMs = ms < 0.0f ? 0.0f : (ms > kMaxFadeMs ? kMaxFadeMs : ms);
    provider_.publish (uiState_);
}

//------------------------------------------------------------------------
void NeditProcessor::setPitchMode (state::PitchMode mode)
{
    if (mode < state::PitchMode::repitch || mode > state::PitchMode::timeStretch)
        return;
    uiState_.render.pitchMode = mode;
    provider_.publish (uiState_);
}

//------------------------------------------------------------------------
void NeditProcessor::setGrainSizeMs (float ms)
{
    constexpr float kMin = state::RenderState::kMinGrainSizeMs;
    constexpr float kMax = state::RenderState::kMaxGrainSizeMs;
    uiState_.render.grainSizeMs = ms < kMin ? kMin : (ms > kMax ? kMax : ms);
    provider_.publish (uiState_);
}

//------------------------------------------------------------------------
void NeditProcessor::setGrainSpeed (float speed)
{
    constexpr float kMin = state::RenderState::kMinGrainSpeed;
    constexpr float kMax = state::RenderState::kMaxGrainSpeed;
    uiState_.render.grainSpeed = speed < kMin ? kMin : (speed > kMax ? kMax : speed);
    provider_.publish (uiState_);
}

//------------------------------------------------------------------------
void NeditProcessor::setTapeStopScope (state::WindowScope scope)
{
    if (scope != state::WindowScope::wholeWindow && scope != state::WindowScope::perTick)
        return;
    uiState_.generate.tapeStopScope = scope;
    provider_.publish (uiState_);
}

//------------------------------------------------------------------------
void NeditProcessor::setFilterSweepScope (state::WindowScope scope)
{
    if (scope != state::WindowScope::wholeWindow && scope != state::WindowScope::perTick)
        return;
    uiState_.generate.filterSweepScope = scope;
    provider_.publish (uiState_);
}

//------------------------------------------------------------------------
void NeditProcessor::setGenerateMode (state::TriggerMode mode)
{
    if (mode != state::TriggerMode::sliceLength && mode != state::TriggerMode::clock)
        return;   // Generate hosts only its two generative sub-modes
    uiState_.generate.generateMode = mode;
    // The Generate sub-modes ARE the top-level sliceLength/clock trigger
    // modes (the enum has no separate "generate" wrapper; the scheduler
    // dispatches on triggerMode), so the same switch must drive the
    // scheduler-facing mode -- otherwise flipping the ribbon would change
    // UI-only state and the audio would keep the old mode.
    uiState_.triggerMode = mode;
    // Keep the tab<->mode invariant: setting a Generate sub-mode implies the
    // Generate tab (idempotent -- the ribbon lives on that tab -- but keeps
    // the rule "any triggerMode write moves the tab" true everywhere).
    uiState_.ui.activeTab = state::tabForTriggerMode (mode);
    provider_.publish (uiState_);
}

//------------------------------------------------------------------------
void NeditProcessor::setResetBars (int index)
{
    if (index < 0 || index >= static_cast<int> (state::kResetBarsValues.size()))
        return;
    uiState_.generate.resetBarsIndex = index;
    provider_.publish (uiState_);
}

//------------------------------------------------------------------------
void NeditProcessor::setClockReference (int index)
{
    if (! state::isValidNoteValueIndex (index))
        return;
    uiState_.generate.clockReferenceIndex = index;
    provider_.publish (uiState_);
}

//------------------------------------------------------------------------
void NeditProcessor::setSubdivisionWeight (int noteIndex, float weight)
{
    if (! state::isValidNoteValueIndex (noteIndex))
        return;
    uiState_.generate.subdivisionWeights[static_cast<std::size_t> (noteIndex)]
        = std::clamp (weight, 0.0f, 1.0f);
    provider_.publish (uiState_);
}

//------------------------------------------------------------------------
void NeditProcessor::setSubdivisionGroupZero (state::NoteValueVariant variant)
{
    if (variant != state::NoteValueVariant::plain
        && variant != state::NoteValueVariant::dotted
        && variant != state::NoteValueVariant::triplet)
        return;

    bool changed = false;
    for (int i = 0; i < state::kNumNoteValues; ++i)
    {
        if (state::kNoteValueVariant[static_cast<std::size_t> (i)] != variant)
            continue;
        auto& w = uiState_.generate.subdivisionWeights[static_cast<std::size_t> (i)];
        if (w != 0.0f)
        {
            w = 0.0f;
            changed = true;
        }
    }
    if (changed)
        provider_.publish (uiState_);
}

//------------------------------------------------------------------------
int NeditProcessor::randomizeSequence()
{
    // The randomizer needs the derived slice list (for natural lengths)
    // and the source tempo. Without a usable sample there are no slices to
    // distribute, so a randomize reduces to a clear.
    const auto loaded = sampleManager_.acquire();
    const auto& sample = uiState_.sample;

    if (loaded == nullptr || loaded->slices.empty())
    {
        clearSequence();
        return 0;
    }

    // Snapshot the Generate style-probability band (the ONLY style-probability
    // UI, shared by the Generate and Sequence tabs) at press time. The
    // sequencer randomizer draws from its OWN decoupled table (pitfall fix #1:
    // styleWeights must not be silently shared), so Randomize honors whatever
    // the band shows right now without re-coupling the two tables -- later band
    // edits don't change the frozen snapshot this call used.
    uiState_.sequencer.randomizeStyleWeights = uiState_.generate.styleWeights;

    // One fresh seed per call so successive Randomize presses genuinely
    // differ (determinism is exercised at the algorithm level, not here).
    std::random_device rd;

    const auto result = engine::seq::randomizeSequence (
        uiState_.sequencer, loaded->slices,
        sample.sampleSampleRate, engine::tempo::calculatedOriginalBpm (sample),
        rd());

    provider_.publish (uiState_);
    return result.cellsPlaced;
}

//------------------------------------------------------------------------
void NeditProcessor::clearSequence()
{
    engine::seq::clearGrid (uiState_.sequencer);
    provider_.publish (uiState_);
}

//------------------------------------------------------------------------
bool NeditProcessor::setSequencerCell (int row, int column, int styleOrdinal)
{
    auto& seq = uiState_.sequencer;
    if (row < 0 || column < 0 || row >= seq.rows || column >= seq.columns
        || styleOrdinal < -1 || styleOrdinal >= state::kNumPlaybackStyles)
        return false;

    const auto flat = static_cast<std::size_t> (row) * static_cast<std::size_t> (seq.columns)
                    + static_cast<std::size_t> (column);

    if (styleOrdinal < 0)
    {
        // Clear: drop the cell + its overrides/extensions.
        if (seq.grid[flat] < 0)
            return false;
        seq.grid[flat] = -1;
        seq.overrides.erase (static_cast<std::uint32_t> (flat));
        seq.extensions.erase (static_cast<std::uint32_t> (flat));
        provider_.publish (uiState_);
        return true;
    }

    // Writing the same style over itself is a no-op (paint-on-paint erases
    // in the UI, which issues a clear instead).
    if (seq.grid[flat] == static_cast<std::int8_t> (styleOrdinal))
        return false;

    // Monophony: drop every other filled row in this column (and their
    // overrides/extensions) before writing.
    for (int otherRow = 0; otherRow < seq.rows; ++otherRow)
    {
        if (otherRow == row)
            continue;
        const auto otherFlat = static_cast<std::size_t> (otherRow)
                             * static_cast<std::size_t> (seq.columns)
                             + static_cast<std::size_t> (column);
        if (seq.grid[otherFlat] >= 0)
        {
            seq.grid[otherFlat] = -1;
            seq.overrides.erase (static_cast<std::uint32_t> (otherFlat));
            seq.extensions.erase (static_cast<std::uint32_t> (otherFlat));
        }
    }

    seq.grid[flat] = static_cast<std::int8_t> (styleOrdinal);
    provider_.publish (uiState_);
    return true;
}

//------------------------------------------------------------------------
bool NeditProcessor::setSequencerCellExtension (int row, int column, int deltaSteps)
{
    auto& seq = uiState_.sequencer;
    if (row < 0 || column < 0 || row >= seq.rows || column >= seq.columns
        || seq.grid[static_cast<std::size_t> (row) * static_cast<std::size_t> (seq.columns)
                    + static_cast<std::size_t> (column)] < 0)
        return false;

    const auto flat = static_cast<std::uint32_t> (row) * static_cast<std::uint32_t> (seq.columns)
                    + static_cast<std::uint32_t> (column);

    int ext = 0;
    if (const auto it = seq.extensions.find (flat); it != seq.extensions.end())
        ext = static_cast<int> (it->second);

    ext = std::clamp (ext + deltaSteps, 0, 256);
    if (ext == 0)
        seq.extensions.erase (flat);
    else
        seq.extensions[flat] = static_cast<std::uint16_t> (ext);

    provider_.publish (uiState_);
    return true;
}

//------------------------------------------------------------------------
void NeditProcessor::setSelectedDrawingStyle (int styleOrdinal)
{
    if (styleOrdinal < 0 || styleOrdinal >= state::kNumPlaybackStyles)
        return;
    if (uiState_.sequencer.selectedDrawingStyle == styleOrdinal)
        return;
    uiState_.sequencer.selectedDrawingStyle = styleOrdinal;
    provider_.publish (uiState_);
}

//------------------------------------------------------------------------
bool NeditProcessor::setSequencerCellOverride (int row, int column,
                                               state::StyleParamId id, float value)
{
    if (! state::isValidStyleParamId (static_cast<int> (id)))
        return false;
    auto& seq = uiState_.sequencer;
    if (row < 0 || column < 0 || row >= seq.rows || column >= seq.columns
        || seq.grid[static_cast<std::size_t> (row) * static_cast<std::size_t> (seq.columns)
                    + static_cast<std::size_t> (column)] < 0)
        return false;

    const auto flat = static_cast<std::uint32_t> (row) * static_cast<std::uint32_t> (seq.columns)
                    + static_cast<std::uint32_t> (column);

    const auto& info = state::styleParamInfo (id);
    const float clamped = info.discrete
        ? static_cast<float> (state::clampValue (
            static_cast<int> (std::lround (value)), 0, info.numOptions - 1))
        : state::clampValue (value, info.minValue, info.maxValue);

    auto& cell = seq.overrides[flat];
    const auto it = cell.find (id);
    if (it != cell.end() && it->second == clamped)
        return false;
    cell[id] = clamped;
    provider_.publish (uiState_);
    return true;
}

//------------------------------------------------------------------------
bool NeditProcessor::clearSequencerCellOverride (int row, int column,
                                                 state::StyleParamId id)
{
    if (! state::isValidStyleParamId (static_cast<int> (id)))
        return false;
    auto& seq = uiState_.sequencer;
    if (row < 0 || column < 0 || row >= seq.rows || column >= seq.columns)
        return false;

    const auto flat = static_cast<std::uint32_t> (row) * static_cast<std::uint32_t> (seq.columns)
                    + static_cast<std::uint32_t> (column);
    const auto cellIt = seq.overrides.find (flat);
    if (cellIt == seq.overrides.end())
        return false;
    const auto erased = cellIt->second.erase (id) > 0;
    if (cellIt->second.empty())
        seq.overrides.erase (cellIt);
    if (erased)
        provider_.publish (uiState_);
    return erased;
}

//------------------------------------------------------------------------
bool NeditProcessor::setSequencerPatternLength (int index)
{
    if (index < 0
        || index >= static_cast<int> (state::kPatternLengthBarsValues.size()))
        return false;
    if (uiState_.sequencer.patternLengthBarsIndex == index)
        return false;
    uiState_.sequencer.patternLengthBarsIndex = index;
    // Pattern length is a grid DIMENSION: resizing the working grid resets
    // it to the new column count (the documented reset-on-dimension-change
    // contract -- analogous to a sample load).
    resizeSequencerGridForSample();
    provider_.publish (uiState_);
    return true;
}

//------------------------------------------------------------------------
bool NeditProcessor::setSequencerStepResolution (int index)
{
    if (! state::isValidNoteValueIndex (index))
        return false;
    if (uiState_.sequencer.stepResolutionIndex == index)
        return false;
    uiState_.sequencer.stepResolutionIndex = index;
    resizeSequencerGridForSample();
    provider_.publish (uiState_);
    return true;
}

//------------------------------------------------------------------------
bool NeditProcessor::setSequencerSwitchTiming (int ordinal)
{
    using state::PatternSwitchTiming;
    if (ordinal < static_cast<int> (PatternSwitchTiming::immediate)
        || ordinal > static_cast<int> (PatternSwitchTiming::endOfPattern))
        return false;
    if (static_cast<int> (uiState_.sequencer.patternSwitchTiming) == ordinal)
        return false;
    uiState_.sequencer.patternSwitchTiming = static_cast<PatternSwitchTiming> (ordinal);
    provider_.publish (uiState_);
    return true;
}

//------------------------------------------------------------------------
bool NeditProcessor::setSequencerSwitchInterval (int index)
{
    if (! state::isValidNoteValueIndex (index))
        return false;
    if (uiState_.sequencer.patternSwitchIntervalIndex == index)
        return false;
    uiState_.sequencer.patternSwitchIntervalIndex = index;
    provider_.publish (uiState_);
    return true;
}

//------------------------------------------------------------------------
void NeditProcessor::rebuildSlicesPreservingWeights()
{
    // Snapshot the OLD slices + their painted weights (kept parallel).
    std::vector<engine::Slice> oldSlices;
    if (const auto cur = sampleManager_.acquire())
        oldSlices = cur->slices;

    std::vector<float> oldWeights = uiState_.generate.sliceWeights;
    oldWeights.resize (oldSlices.size(), 1.0f);

    // Rebuild from the retained analysis + current sample state.
    const auto next = sampleManager_.rebuildSlices (uiState_.sample);
    const auto& newSlices = (next != nullptr) ? next->slices : oldSlices;

    // Remap: each new slice inherits the weight of the OLD slice that
    // contained its start-frame (an unchanged boundary matches exactly; a
    // split inherits its parent's value; a merge keeps the surviving
    // boundary's value). Defaults to 1.0 for genuinely new regions.
    std::vector<float> newWeights (newSlices.size(), 1.0f);
    for (std::size_t j = 0; j < newSlices.size(); ++j)
    {
        const std::int64_t s = newSlices[j].startFrame;
        for (std::size_t k = 0; k < oldSlices.size(); ++k)
        {
            if (s >= oldSlices[k].startFrame && s < oldSlices[k].endFrame)
            {
                newWeights[j] = oldWeights[k];
                break;
            }
        }
    }

    uiState_.generate.sliceWeights = std::move (newWeights);
    provider_.publish (uiState_);
}

//------------------------------------------------------------------------
void NeditProcessor::setAuditionEnabled (bool enabled)
{
    // The read cursor is audio-thread-owned; process() reseeds it to the
    // trim start on the off->on edge, so no cross-thread write happens here.
    uiState_.ui.auditionEnabled = enabled;
    provider_.publish (uiState_);
}

//------------------------------------------------------------------------
void NeditProcessor::pollAuditionAutoStop()
{
    if (! auditionAutoStopPending_.exchange (false, std::memory_order_acq_rel))
        return;
    if (! uiState_.ui.auditionEnabled)
        return;   // already off (user toggled first); nothing to fold

    uiState_.ui.auditionEnabled = false;
    provider_.publish (uiState_);
}

//------------------------------------------------------------------------
void NeditProcessor::startSliceAudition (int64_t startFrame, int64_t endFrame)
{
    // Bounds FIRST, flag LAST: the audio thread acquire-loads the flag, so
    // once it observes `true` the bounds stores are visible too. (The old
    // order -- flag first -- could render one block with the previous
    // slice's bounds.)
    sliceAuditionStart_.store (startFrame, std::memory_order_relaxed);
    sliceAuditionEnd_.store (endFrame, std::memory_order_relaxed);
    sliceAuditionActive_.store (true, std::memory_order_release);
}

//------------------------------------------------------------------------
void NeditProcessor::stopSliceAudition()
{
    sliceAuditionActive_.store (false, std::memory_order_release);
}

//------------------------------------------------------------------------
Steinberg::IPlugView* PLUGIN_API NeditProcessor::createView (Steinberg::FIDString name)
{
    if (name != nullptr && std::strcmp (name, Vst::ViewType::kEditor) == 0)
        return static_cast<Steinberg::IPlugView*> (new NeditEditor (this));
    return nullptr;
}

//------------------------------------------------------------------------
tresult PLUGIN_API NeditProcessor::process (Vst::ProcessData& data)
{
    if (data.numOutputs <= 0 || data.outputs == nullptr || data.numSamples <= 0)
        return kOk;

    const auto snapshot = provider_.acquire();
    const state::PluginState* st = snapshot.get();

    // --- host parameter automation: fold into the reusable scratch copy --
    if (data.inputParameterChanges != nullptr && data.inputParameterChanges->getParameterCount() > 0)
    {
        automationScratch_ = *snapshot;   // capacity-reused, see ctor

        auto* changes = data.inputParameterChanges;

        for (int32 i = 0; i < changes->getParameterCount(); ++i)
        {
            auto* queue = changes->getParameterData (i);

            if (queue == nullptr || queue->getPointCount() <= 0)
                continue;

            Steinberg::int32 offset = 0;
            Vst::ParamValue value = 0.0;

            if (queue->getPoint (queue->getPointCount() - 1, offset, value) == kOk)
                applyNormalized (automationScratch_,
                                                   static_cast<std::uint32_t> (queue->getParameterId()),
                                                   static_cast<float> (value));
        }

        st = &automationScratch_;
    }

    // --- transport ---------------------------------------------------------
    engine::TransportFrame transport {};
    transport.playing = false;
    transport.bpm = 120.0;
    transport.ppqStart = lastBlockEndPpq_;

    if (data.processContext != nullptr)
    {
        const auto& pc = *data.processContext;

        transport.playing = (pc.state & Vst::ProcessContext::kPlaying) != 0;

        if (pc.tempo > 0.5)
            transport.bpm = pc.tempo;

        if (transport.playing)
        {
            if ((pc.state & Vst::ProcessContext::kProjectTimeMusicValid) != 0)
                transport.ppqStart = pc.projectTimeMusic;
            else
                transport.ppqStart
                    = static_cast<double> (pc.projectTimeSamples) * (transport.bpm / 60.0)
                   / (processSetup.sampleRate > 0 ? processSetup.sampleRate : 44100.0);

            lastBlockEndPpq_
                = transport.ppqStart
               + static_cast<double> (data.numSamples) * (transport.bpm / 60.0)
              / (processSetup.sampleRate > 0 ? processSetup.sampleRate : 44100.0);
        }
        // Stopped: freeze at the last position (fixture semantics).
    }

    const double hostRate = processSetup.sampleRate > 0 ? processSetup.sampleRate
                                                        : 44100.0;

    // --- host output bus: validate, then clear ------------------------------
    // Every render path below ADDS into the buffers (outAdd[ch][i] += ...),
    // and hosts guarantee neither zeroed buffers nor non-null channel
    // pointers. The memset loop skips null channels; if ANY channel is null
    // (or the bus is empty) we bail after clearing what exists rather than
    // hand the scheduler a pointer table it would dereference blindly.
    auto& outBus = data.outputs[0];
    if (outBus.numChannels <= 0 || outBus.channelBuffers32 == nullptr)
        return kOk;

    outBus.silenceFlags = 0;
    bool anyNullChannel = false;
    for (Steinberg::int32 ch = 0; ch < outBus.numChannels; ++ch)
    {
        if (outBus.channelBuffers32[ch] != nullptr)
            std::memset (outBus.channelBuffers32[ch], 0,
                         sizeof (float) * static_cast<std::size_t> (data.numSamples));
        else
            anyNullChannel = true;
    }
    if (anyNullChannel)
        return kOk;

// --- sample slot --------------------------------------------------------
    const auto loaded = sampleManager_.acquire();
    static const std::vector<engine::Slice> kEmptySlices;
    const auto& rawSlices = (loaded != nullptr) ? loaded->slices
                                                : kEmptySlices;

    // The view keeps a SOFT trim -- trim edits do NOT rebuild the slice list
    // (slices outside the trim are merely hidden; widening reveals them with
    // weights intact; see WaveformView's draw comment). The engine must apply
    // the same rule to what it PLAYS, otherwise picks draw audio/slices from
    // outside the trim. Clip the shared list to [trimStart, trimEnd) every
    // block and remap generate.sliceWeights in lockstep into pre-reserved
    // scratch (no audio-thread allocation in steady state); the scheduler
    // picks from the override weights so indices stay parallel.
    clipSlicesToTrim (rawSlices, st->sample.trimStartFrame, st->sample.trimEndFrame,
                      st->generate.sliceWeights, trimSlices_, trimWeights_);
    const auto& slices = trimSlices_;
    const std::vector<float>* sliceWeights = &trimWeights_;

    // Slice audition (RMB hold): raw loop of a single slice region,
    // bypasses everything. No host-transport auto-stop — the user is
    // manually previewing a region. The cursor is reseeded to the slice
    // head on the inactive->active edge (audio-side edge detection; the UI
    // thread never writes the cursor).
    if (sliceAuditionActive_.load (std::memory_order_acquire) && loaded != nullptr)
    {
        if (! sliceAuditionWasActive_)
            sliceAuditionPosition_ = static_cast<double> (
                sliceAuditionStart_.load (std::memory_order_relaxed));
        sliceAuditionWasActive_ = true;

        renderSliceAudition (st->sample, *loaded, outBus.channelBuffers32,
                             outBus.numChannels, data.numSamples, hostRate);
        return kOk;
    }
    sliceAuditionWasActive_ = false;

    // --- MIDI ---------------------------------------------------------------
    if (data.inputEvents != nullptr)
    {
        auto* events = data.inputEvents;
        // Control mode dispatches at most 32 slice notes.
        const int availableSlices = std::min (static_cast<int> (slices.size()), 32);

        for (int32 i = 0; i < events->getEventCount(); ++i)
        {
            Vst::Event event {};

            if (events->getEvent (i, event) != kOk)
                continue;

            if (event.type == Vst::Event::kNoteOnEvent && event.noteOn.velocity > 0.f)
            {
                // Clamp BEFORE the uint8 cast: the spec says [0,1] but a
                // misbehaving host's velocity > ~2 would make the float ->
                // uint8 conversion itself UB (value unrepresentable).
                const float vel01 = std::clamp (event.noteOn.velocity, 0.0f, 1.0f);
                routeMidiNote (scheduler_, *st, event.noteOn.pitch,
                               engine::velocityFromMidiByte (
                                   static_cast<std::uint8_t> (vel01 * 127.f)),
                               true, transport.playing, availableSlices);
            }
            else if (event.type == Vst::Event::kNoteOffEvent)
            {
                routeMidiNote (scheduler_, *st, event.noteOff.pitch, 0.0f,
                               false, transport.playing, availableSlices);
            }
        }
    }

    // --- render ---------------------------------------------------------------
    ctx_.hostSampleRate = hostRate;
    ctx_.sourceSampleRate = st->sample.sampleSampleRate;

    if (loaded != nullptr && loaded->audio != nullptr && loaded->audio->channelCount() > 0)
    {
        // Refresh the per-channel pointer table. Fixed capacity: no
        // audio-thread allocation; channels beyond the cap are ignored.
        const int chans = std::min (loaded->audio->channelCount(), kMaxSourceChannels);
        for (int c = 0; c < chans; ++c)
            sourceChannelPointers_[static_cast<std::size_t> (c)] =
                loaded->audio->channels[static_cast<std::size_t> (c)].data();

        ctx_.source = sourceChannelPointers_.data();
        ctx_.sourceChannels = chans;
        ctx_.sourceFrames = loaded->audio->frames;
    }
    else
    {
        ctx_.source = nullptr;
        ctx_.sourceChannels = 0;
        ctx_.sourceFrames = 0;
    }

    // Audition gate: when the user enables audition (and the transport is
    // stopped), the scheduler is bypassed entirely and renderAudition
    // produces a raw loop of [trimStart, trimEnd) at native pitch/speed —
    // no slicing, no playback styles, no effects. The cursor reseeds to the
    // trim start on the off->on edge (audio-side; the UI never writes it).
    if (st->ui.auditionEnabled && ! transport.playing)
    {
        if (loaded != nullptr)
        {
            if (! auditionWasActive_)
                auditionPosition_ = static_cast<double> (st->sample.trimStartFrame);
            auditionWasActive_ = true;

            renderAudition (st->sample, *loaded, outBus.channelBuffers32,
                            outBus.numChannels, data.numSamples, hostRate);
        }
        return kOk;
    }
    auditionWasActive_ = false;

    // Transport started while audition was on: the engine takes over THIS
    // block (audition and the real engine never overlap), and the state-side
    // toggle is folded back on the UI thread via pollAuditionAutoStop().
    // CRITICAL: process() must never mutate or publish (clone) uiState_ --
    // that deep copy races UI-thread vector edits (heap corruption) and
    // allocates on the audio thread.
    if (st->ui.auditionEnabled)
        auditionAutoStopPending_.store (true, std::memory_order_release);

    // Hosts provide an array of per-channel pointers.
    scheduler_.process (*st, slices, ctx_, transport,
                        outBus.channelBuffers32,
                        outBus.numChannels,
                        data.numSamples,
                        sliceWeights);

    return kOk;
}

//------------------------------------------------------------------------
void NeditProcessor::renderAudition (const state::SampleState& sample,
                                     const LoadedSample& loaded,
                                     float* const* outAdd, int numOutChannels,
                                     int numSamples, double hostSampleRate)
{
    if (loaded.audio == nullptr)
        return;

    const auto& audio = *loaded.audio;
    const int sourceChannels = audio.channelCount();
    const int64_t sourceFrames = audio.frames;

    if (sourceFrames == 0 || sourceChannels == 0 || numSamples <= 0)
        return;

    const int64_t trimStart = sample.trimStartFrame;
    const int64_t trimEnd = sample.trimEndFrame;
    const int64_t trimLen = trimEnd - trimStart;

    if (trimLen <= 0)
        return;

    // Sample-rate matching only — never repitch.
    const double auditionRate = (sample.sampleSampleRate > 0.0 && hostSampleRate > 0.0)
                                    ? sample.sampleSampleRate / hostSampleRate
                                    : 1.0;

    for (int i = 0; i < numSamples; ++i)
    {
        // Wrap position into [trimStart, trimEnd).
        if (auditionPosition_ < static_cast<double> (trimStart)
            || auditionPosition_ >= static_cast<double> (trimEnd))
            auditionPosition_ = static_cast<double> (trimStart);

        const int64_t idx0 = static_cast<int64_t> (auditionPosition_);
        const int64_t idx1 = idx0 + 1;
        const float frac = static_cast<float> (auditionPosition_ - static_cast<double> (idx0));

        const int64_t idx0c = std::clamp (idx0, static_cast<int64_t> (0), sourceFrames - 1);
        const int64_t idx1c = std::clamp (idx1, static_cast<int64_t> (0), sourceFrames - 1);

        for (int ch = 0; ch < numOutChannels; ++ch)
        {
            const int srcCh = std::min (ch, sourceChannels - 1);
            const float s0 = audio.channels[static_cast<std::size_t> (srcCh)]
                                 [static_cast<std::size_t> (idx0c)];
            const float s1 = audio.channels[static_cast<std::size_t> (srcCh)]
                                 [static_cast<std::size_t> (idx1c)];
            outAdd[ch][i] += s0 + frac * (s1 - s0);
        }

        auditionPosition_ += auditionRate;
    }
}

//------------------------------------------------------------------------
void NeditProcessor::renderSliceAudition (const state::SampleState& sample,
                                          const LoadedSample& loaded,
                                          float* const* outAdd, int numOutChannels,
                                          int numSamples, double hostSampleRate)
{
    if (loaded.audio == nullptr)
        return;

    const auto& audio = *loaded.audio;
    const int sourceChannels = audio.channelCount();
    const int64_t sourceFrames = audio.frames;

    const int64_t start = sliceAuditionStart_.load (std::memory_order_relaxed);
    const int64_t end = sliceAuditionEnd_.load (std::memory_order_relaxed);

    if (sourceFrames == 0 || sourceChannels == 0 || numSamples <= 0 || end <= start)
        return;

    const double auditionRate = (sample.sampleSampleRate > 0.0 && hostSampleRate > 0.0)
                                    ? sample.sampleSampleRate / hostSampleRate
                                    : 1.0;

    for (int i = 0; i < numSamples; ++i)
    {
        if (sliceAuditionPosition_ < static_cast<double> (start)
            || sliceAuditionPosition_ >= static_cast<double> (end))
            sliceAuditionPosition_ = static_cast<double> (start);

        const int64_t idx0 = static_cast<int64_t> (sliceAuditionPosition_);
        const int64_t idx1 = idx0 + 1;
        const float frac = static_cast<float> (sliceAuditionPosition_ - static_cast<double> (idx0));

        const int64_t idx0c = std::clamp (idx0, static_cast<int64_t> (0), sourceFrames - 1);
        const int64_t idx1c = std::clamp (idx1, static_cast<int64_t> (0), sourceFrames - 1);

        for (int ch = 0; ch < numOutChannels; ++ch)
        {
            const int srcCh = std::min (ch, sourceChannels - 1);
            const float s0 = audio.channels[static_cast<std::size_t> (srcCh)]
                                 [static_cast<std::size_t> (idx0c)];
            const float s1 = audio.channels[static_cast<std::size_t> (srcCh)]
                                 [static_cast<std::size_t> (idx1c)];
            outAdd[ch][i] += s0 + frac * (s1 - s0);
        }

        sliceAuditionPosition_ += auditionRate;
    }
}

} // namespace nedit::plugin
