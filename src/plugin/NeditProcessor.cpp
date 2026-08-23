// NeditProcessor -- see the header for the wiring overview.

#include "plugin/NeditProcessor.h"

#include "state/Serialization.h"

#include "base/source/fstreamer.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ustring.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"

#include <algorithm>
#include <cstdint>

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

} // namespace

//------------------------------------------------------------------------
NeditProcessor::NeditProcessor()
    : provider_ (uiState_)
{
    automationScratch_.sample.manualPoints.reserve (kReservedSamplePoints);
    automationScratch_.sample.excludedPoints.reserve (kReservedSamplePoints);
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
int NeditProcessor::numAvailableSlices (const state::PluginState&) const noexcept
{
    // The derived slice list arrives with Phase 4's analysis plumbing;
    // until then no slice notes exist to trigger.
    return static_cast<int> (slices_.size());
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

    // --- MIDI ---------------------------------------------------------------
    if (data.inputEvents != nullptr)
    {
        auto* events = data.inputEvents;
        const int slices = numAvailableSlices (*st);

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
                               true, transport.playing, slices);
            }
            else if (event.type == Vst::Event::kNoteOffEvent)
            {
                routeMidiNote (scheduler_, *st, event.noteOff.pitch, 0.0f,
                               false, transport.playing, slices);
            }
        }
    }

    // --- render ---------------------------------------------------------------
    ctx_.hostSampleRate = processSetup.sampleRate > 0 ? processSetup.sampleRate : 44100.0;
    ctx_.sourceSampleRate = st->sample.sampleSampleRate;
    ctx_.source = nullptr;          // decoded sample memory arrives with Phase 4
    ctx_.sourceChannels = 0;
    ctx_.sourceFrames = 0;

    // Hosts provide an array of per-channel pointers.
    scheduler_.process (*st, slices_, ctx_, transport,
                        data.outputs[0].channelBuffers32,
                        data.outputs[0].numChannels < 1 ? 1
                                                        : data.outputs[0].numChannels,
                        data.numSamples);

    return kOk;
}

} // namespace nedit::plugin
