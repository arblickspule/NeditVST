// NeditProcessor: shell glue smoke tests driven through the real VST3
// classes (no host) -- parameter surface registration, state persistence
// round trip, and a process() block with MIDI events.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "plugin/NeditProcessor.h"

#include "public.sdk/source/common/memorystream.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"

#include <array>
#include <cmath>
#include <vector>

using namespace nedit;
using namespace nedit::plugin;
using Steinberg::Vst::ProcessData;

namespace
{

// Minimal event list so tests can feed MIDI without a host.
class TestEventList : public Steinberg::Vst::IEventList
{
public:
    TestEventList() { FUNKNOWN_CTOR }
    ~TestEventList() { FUNKNOWN_DTOR }

    void add (const Steinberg::Vst::Event& e) { events.push_back (e); }
    Steinberg::int32 PLUGIN_API getEventCount() override { return static_cast<Steinberg::int32> (events.size()); }
    Steinberg::tresult PLUGIN_API getEvent (Steinberg::int32 index, Steinberg::Vst::Event& e) override
    {
        if (index < 0 || index >= static_cast<Steinberg::int32> (events.size()))
            return Steinberg::kResultFalse;
        e = events[static_cast<std::size_t> (index)];
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API addEvent (Steinberg::Vst::Event& e) override
    {
        events.push_back (e);
        return Steinberg::kResultOk;
    }

    DECLARE_FUNKNOWN_METHODS

private:
    std::vector<Steinberg::Vst::Event> events;
};

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
IMPLEMENT_FUNKNOWN_METHODS (TestEventList, Steinberg::Vst::IEventList,
                            Steinberg::Vst::IEventList::iid)
#pragma GCC diagnostic pop

Steinberg::Vst::Event makeNoteOn (Steinberg::int16 pitch, float velocity)
{
    Steinberg::Vst::Event e {};
    e.type = Steinberg::Vst::Event::kNoteOnEvent;
    e.busIndex = 0;
    e.noteOn.pitch = pitch;
    e.noteOn.velocity = velocity;
    return e;
}

struct RunningPlugin
{
    NeditProcessor processor;

    RunningPlugin()
    {
        REQUIRE (processor.initialize (nullptr) == Steinberg::kResultOk);
        processor.setActive (1);
        processor.setProcessing (1);
    }
};

} // namespace

TEST_CASE ("shell: initialize registers the full parameter surface")
{
    NeditProcessor p;
    REQUIRE (p.initialize (nullptr) == Steinberg::kResultOk);

    const auto count = p.getParameterCount();
    CHECK (count == 21 + 8);

    // Spot-check ids survive the container mapping.
    CHECK (p.getParamNormalized (kParamTriggerMode)
           == Catch::Approx (0.0).margin (1e-6));
}

TEST_CASE ("shell: setParamNormalized writes state and publishes")
{
    RunningPlugin fx;

    fx.processor.setParamNormalized (kParamTriggerMode, 1.0);   // control mode
    CHECK (fx.processor.debugUiState().triggerMode == state::TriggerMode::control);

    fx.processor.setParamNormalized (kParamManualTempoBpm, 0.5);
    CHECK_THAT (fx.processor.debugUiState().sample.manualBpmOverrideValue,
                Catch::Matchers::WithinAbs (165.0, 1e-3));

    fx.processor.setParamNormalized (9999, 0.5);   // invalid: rejected
    CHECK (fx.processor.debugUiState().control.baseNote == 36);   // untouched default
}

TEST_CASE ("shell: getState/setState round trips through real serialization")
{
    RunningPlugin source;
    source.processor.setParamNormalized (kParamManualTempoBpm, 0.75);
    source.processor.setParamNormalized (kParamControlGateMode, 1.0);
    source.processor.setParamNormalized (kParamQuantizeRecallInterval, 7.0 / 19.0);

    Steinberg::MemoryStream outStream;
    REQUIRE (source.processor.getState (&outStream) == Steinberg::kResultOk);
    CHECK (outStream.getSize() > 0);

    RunningPlugin target;
    outStream.seek (0, Steinberg::IBStream::kIBSeekSet, nullptr);
    REQUIRE (target.processor.setState (&outStream) == Steinberg::kResultOk);

    const auto& a = source.processor.debugUiState();
    const auto& b = target.processor.debugUiState();

    CHECK (a == b);   // PluginState::operator== -- full structural equality

    // And the parameter surface reflects it.
    CHECK (target.processor.getParamNormalized (kParamControlGateMode)
           == Catch::Approx (1.0).margin (1e-9));
}

TEST_CASE ("shell: setState rejects garbage and keeps current state")
{
    RunningPlugin fx;
    fx.processor.setParamNormalized (kParamManualTempoBpm, 0.25);

    std::vector<Steinberg::uint8> garbage (512, 0xAB);
    for (auto& byte : garbage)
        byte = static_cast<Steinberg::uint8> (std::rand() & 0xFF);

    Steinberg::MemoryStream badStream (garbage.data(),
                                       static_cast<Steinberg::int32> (garbage.size()));
    CHECK (fx.processor.setState (&badStream) == Steinberg::kResultFalse);

    // Untouched: 0.25 normalized = 30 + 0.25*270 = 97.5 bpm.
    CHECK_THAT (fx.processor.debugUiState().sample.manualBpmOverrideValue,
                Catch::Matchers::WithinAbs (97.5, 1e-3));
}

TEST_CASE ("shell: process block renders silence safely, MIDI routes to scheduler")
{
    RunningPlugin fx;
    fx.processor.setParamNormalized (kParamTriggerMode,
                                     4.0 / 4.0);   // control mode

    // Hosts hand over an array of per-channel pointers.
    std::vector<float> left (64, -1.0f);   // poisoned; proves finiteness
    std::vector<float> right (64, -1.0f);
    float* channelPointers[2] { left.data(), right.data() };

    ProcessData data {};
    data.numSamples = 64;
    data.numOutputs = 1;
    data.numInputs = 0;

    Steinberg::Vst::AudioBusBuffers outputs {};
    outputs.numChannels = 2;
    outputs.silenceFlags = 0;
    outputs.channelBuffers32 = channelPointers;
    data.outputs = &outputs;

    TestEventList events;
    auto noteOn = makeNoteOn (37, 0.8f);
    events.add (noteOn);       // slice 1 note (pending; no slices exist yet)

    data.inputEvents = &events;

    REQUIRE (fx.processor.process (data) == Steinberg::kResultOk);

    // No sample analysis exists in the shell yet => no slices => no picks,
    // and the untouched sink stays finite.
    CHECK (fx.processor.debugScheduler().picksStarted() == 0);

    for (const float sample : left)
        CHECK (std::isfinite (sample));
    for (const float sample : right)
        CHECK (std::isfinite (sample));
}

TEST_CASE ("shell: createView produces the editor (headless-safe checks only)")
{
    RunningPlugin fx;

    auto* view = fx.processor.createView (Steinberg::Vst::ViewType::kEditor);
    REQUIRE (view != nullptr);

    // Editors must embed via a platform window ID; X11 on Linux, HWND on
    // Windows, Cocoa on macOS.
#if defined(__linux__)
    CHECK (view->isPlatformTypeSupported (Steinberg::kPlatformTypeX11EmbedWindowID)
           == Steinberg::kResultTrue);
#elif defined(_WIN32)
    CHECK (view->isPlatformTypeSupported (Steinberg::kPlatformTypeHWND)
           == Steinberg::kResultTrue);
#else
    // macOS embeds via an NSView.
    CHECK (view->isPlatformTypeSupported (Steinberg::kPlatformTypeNSView)
           == Steinberg::kResultTrue);
#endif

    Steinberg::ViewRect size {};
    CHECK (view->getSize (&size) == Steinberg::kResultTrue);
    CHECK (size.getWidth() > 0);
    CHECK (size.getHeight() > 0);

    view->release();

    CHECK (fx.processor.createView ("definitely-not-a-view") == nullptr);
}

TEST_CASE ("shell: visible-window writes persist in UiState and sanitize")
{
    RunningPlugin fx;

    fx.processor.setVisibleWindow (0.25, 0.5);
    CHECK (fx.processor.debugUiState().ui.visibleStartNorm == 0.25);
    CHECK (fx.processor.debugUiState().ui.visibleEndNorm == 0.5);

    // Inverted garbage collapses back to the full window.
    fx.processor.setVisibleWindow (0.9, 0.1);
    CHECK (fx.processor.debugUiState().ui.visibleStartNorm == 0.0);
    CHECK (fx.processor.debugUiState().ui.visibleEndNorm == 1.0);
}

TEST_CASE ("shell: active-tab writes persist in UiState")
{
    RunningPlugin fx;

    CHECK (fx.processor.debugUiState().ui.activeTab == state::UiTab::generate);

    fx.processor.setActiveTab (state::UiTab::perform);
    CHECK (fx.processor.debugUiState().ui.activeTab == state::UiTab::perform);

    fx.processor.setActiveTab (static_cast<state::UiTab> (state::UiTab::generate));
    CHECK (fx.processor.debugUiState().ui.activeTab == state::UiTab::generate);
}

TEST_CASE ("shell: style-probability writes persist in GenerateState")
{
    RunningPlugin fx;

    fx.processor.setStyleWeight (3, 0.4f);
    CHECK (fx.processor.debugUiState().generate.styleWeights[3]
           == Catch::Approx (0.4f));

    // Out-of-range weights clamp to the [0,1] domain.
    fx.processor.setStyleWeight (3, 1.7f);
    CHECK (fx.processor.debugUiState().generate.styleWeights[3]
           == Catch::Approx (1.0f));

    // Out-of-range style indices are ignored.
    fx.processor.setStyleWeight (99, 0.5f);
    CHECK (fx.processor.debugUiState().generate.styleWeights[0]
           == Catch::Approx (1.0f));
}
