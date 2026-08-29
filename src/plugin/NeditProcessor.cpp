// NeditProcessor -- see the header for the wiring overview.

#include "plugin/NeditProcessor.h"

#include "plugin/NeditEditor.h"
#include "state/Serialization.h"

#include "base/source/fstreamer.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ustring.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"

#include <engine/Tempo.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>

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
    auditionPosition_ = 0.0;  // reset audition cursor for the new sample
    // Slice weights are parallel to the derived slice list and reset when
    // slices rebuild -- fresh analysis means equal probability everywhere.
    uiState_.generate.sliceWeights.assign (result->sample->slices.size(), 1.0f);
    provider_.publish (uiState_);
    return true;
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
    if (enabled)
        auditionPosition_ = 0.0;  // start from trim start on next block

    uiState_.ui.auditionEnabled = enabled;
    provider_.publish (uiState_);
}

//------------------------------------------------------------------------
void NeditProcessor::startSliceAudition (int64_t startFrame, int64_t endFrame)
{
    sliceAuditionActive_ = true;
    sliceAuditionStart_ = startFrame;
    sliceAuditionEnd_ = endFrame;
    sliceAuditionPosition_ = static_cast<double> (startFrame);
}

//------------------------------------------------------------------------
void NeditProcessor::stopSliceAudition()
{
    sliceAuditionActive_ = false;
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
    // manually previewing a region.
    if (sliceAuditionActive_ && loaded != nullptr)
    {
        auto& outBus = data.outputs[0];
        outBus.silenceFlags = 0;
        for (Steinberg::int32 ch = 0; ch < outBus.numChannels; ++ch)
            if (outBus.channelBuffers32[ch] != nullptr)
                std::memset (outBus.channelBuffers32[ch], 0,
                             sizeof (float) * static_cast<std::size_t> (data.numSamples));

        renderSliceAudition (outBus.channelBuffers32,
                             outBus.numChannels < 1 ? 1 : outBus.numChannels,
                             data.numSamples,
                             processSetup.sampleRate > 0 ? processSetup.sampleRate
                                                         : 44100.0);
        return kOk;
    }

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
                routeMidiNote (scheduler_, *st, event.noteOn.pitch,
                               engine::velocityFromMidiByte (static_cast<std::uint8_t> (
                                   event.noteOn.velocity * 127.f)),
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
    ctx_.hostSampleRate = processSetup.sampleRate > 0 ? processSetup.sampleRate : 44100.0;
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

    // The scheduler ADDS into the output (outAdd[c] += ...); hosts do NOT
    // guarantee zeroed output buffers, so clear them first -- otherwise we
    // sum our signal onto whatever garbage/previous audio they contain.
    auto& outBus = data.outputs[0];
    outBus.silenceFlags = 0;
    for (Steinberg::int32 ch = 0; ch < outBus.numChannels; ++ch)
        if (outBus.channelBuffers32[ch] != nullptr)
            std::memset (outBus.channelBuffers32[ch], 0,
                         sizeof (float) * static_cast<std::size_t> (data.numSamples));

    // Audition gate: when the user enables audition, the scheduler is
    // bypassed entirely and renderAudition produces a raw loop of
    // [trimStart, trimEnd) at native pitch/speed — no slicing, no
    // playback styles, no effects. Auto-stops the instant host transport
    // starts playing, so audition and the real engine never overlap.
    if (st->ui.auditionEnabled)
    {
        if (transport.playing)
        {
            uiState_.ui.auditionEnabled = false;
            provider_.publish (uiState_);
        }
        else if (loaded != nullptr)
        {
            renderAudition (outBus.channelBuffers32,
                            outBus.numChannels < 1 ? 1 : outBus.numChannels,
                            data.numSamples,
                            processSetup.sampleRate > 0 ? processSetup.sampleRate
                                                        : 44100.0);
        }
        return kOk;
    }

    // Hosts provide an array of per-channel pointers.
    scheduler_.process (*st, slices, ctx_, transport,
                        outBus.channelBuffers32,
                        outBus.numChannels < 1 ? 1 : outBus.numChannels,
                        data.numSamples,
                        sliceWeights);

    return kOk;
}

//------------------------------------------------------------------------
void NeditProcessor::renderAudition (float* const* outAdd, int numOutChannels,
                                     int numSamples, double hostSampleRate)
{
    const auto loaded = sampleManager_.acquire();
    if (loaded == nullptr || loaded->audio == nullptr)
        return;

    const auto& audio = *loaded->audio;
    const auto& sample = uiState_.sample;
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

        const int64_t idx0c = std::min (idx0, sourceFrames - 1);
        const int64_t idx1c = std::min (idx1, sourceFrames - 1);

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
void NeditProcessor::renderSliceAudition (float* const* outAdd, int numOutChannels,
                                          int numSamples, double hostSampleRate)
{
    const auto loaded = sampleManager_.acquire();
    if (loaded == nullptr || loaded->audio == nullptr)
        return;

    const auto& audio = *loaded->audio;
    const auto& sample = uiState_.sample;
    const int sourceChannels = audio.channelCount();
    const int64_t sourceFrames = audio.frames;

    const int64_t start = sliceAuditionStart_;
    const int64_t end = sliceAuditionEnd_;

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

        const int64_t idx0c = std::min (idx0, sourceFrames - 1);
        const int64_t idx1c = std::min (idx1, sourceFrames - 1);

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
