// NeditProcessor: shell glue smoke tests driven through the real VST3
// classes (no host) -- parameter surface registration, state persistence
// round trip, and a process() block with MIDI events.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "plugin/NeditProcessor.h"
#include "plugin/NeditEditor.h"
#include "ui/TimingGrey.h"

#include "vstgui/lib/controls/ccontrol.h"

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
    CHECK (count == 19 + 8);

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

TEST_CASE ("shell: process survives hostile output bus shapes")
{
    // Regression: the buffer-zeroing loop null-checked channel pointers but
    // the render paths then dereferenced them blindly (and a zero-channel
    // bus was promoted to 1, indexing channelBuffers32[0] of a possibly
    // empty table). Broken hosts must get a clean kOk, not a segfault.
    RunningPlugin fx;

    std::vector<float> left (64, -1.0f);   // poisoned; proves the clear ran

    ProcessData data {};
    data.numSamples = 64;
    data.numOutputs = 1;

    Steinberg::Vst::AudioBusBuffers outputs {};
    data.outputs = &outputs;

    SECTION ("zero-channel bus")
    {
        outputs.numChannels = 0;
        outputs.channelBuffers32 = nullptr;
        CHECK (fx.processor.process (data) == Steinberg::kResultOk);
    }

    SECTION ("null channel pointer table")
    {
        outputs.numChannels = 2;
        outputs.channelBuffers32 = nullptr;
        CHECK (fx.processor.process (data) == Steinberg::kResultOk);
    }

    SECTION ("one null channel amid valid ones")
    {
        float* channels[2] { left.data(), nullptr };
        outputs.numChannels = 2;
        outputs.channelBuffers32 = channels;
        CHECK (fx.processor.process (data) == Steinberg::kResultOk);
        for (const float sample : left)
            CHECK (sample == 0.0f);   // valid channels still cleared pre-bail
    }
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

TEST_CASE ("shell: selecting a tab transfers control to that mode")
{
    RunningPlugin fx;

    // Default: Generate tab, sliceLength mode.
    CHECK (fx.processor.debugUiState().ui.activeTab == state::UiTab::generate);
    CHECK (fx.processor.debugUiState().triggerMode == state::TriggerMode::sliceLength);

    // Selecting the Sequence tab must switch the engine to the sequenced
    // scheduler -- this is the "transfer control to the sequencer" contract.
    fx.processor.setActiveTab (state::UiTab::sequence);
    CHECK (fx.processor.debugUiState().triggerMode == state::TriggerMode::sequenced);

    fx.processor.setActiveTab (state::UiTab::control);
    CHECK (fx.processor.debugUiState().triggerMode == state::TriggerMode::control);

    fx.processor.setActiveTab (state::UiTab::perform);
    CHECK (fx.processor.debugUiState().triggerMode == state::TriggerMode::performance);

    // Back to Generate: the tab carries the remembered sub-mode. Put the
    // ribbon on Clock first, then return via the tab.
    fx.processor.setGenerateMode (state::TriggerMode::clock);
    CHECK (fx.processor.debugUiState().ui.activeTab == state::UiTab::generate);  // ribbon keeps the tab
    fx.processor.setActiveTab (state::UiTab::sequence);                          // leave...
    CHECK (fx.processor.debugUiState().triggerMode == state::TriggerMode::sequenced);
    fx.processor.setActiveTab (state::UiTab::generate);                          // ...and return
    CHECK (fx.processor.debugUiState().triggerMode == state::TriggerMode::clock);
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

TEST_CASE ("shell: sweep-scope writes persist in GenerateState")
{
    RunningPlugin fx;

    // The opposite defaults (matches the original): wholeWindow for Tape
    // Stop, perTick for the Filter sweep.
    CHECK (fx.processor.debugUiState().generate.tapeStopScope
           == state::WindowScope::wholeWindow);
    CHECK (fx.processor.debugUiState().generate.filterSweepScope
           == state::WindowScope::perTick);

    fx.processor.setTapeStopScope (state::WindowScope::perTick);
    CHECK (fx.processor.debugUiState().generate.tapeStopScope
           == state::WindowScope::perTick);

    fx.processor.setFilterSweepScope (state::WindowScope::wholeWindow);
    CHECK (fx.processor.debugUiState().generate.filterSweepScope
           == state::WindowScope::wholeWindow);

    // Out-of-range scopes are ignored.
    fx.processor.setTapeStopScope (static_cast<state::WindowScope> (7));
    CHECK (fx.processor.debugUiState().generate.tapeStopScope
           == state::WindowScope::perTick);
}

TEST_CASE ("editor: mode switch fires on the first click")
{
    RunningPlugin fx;

    // Drive the SAME event sequence VSTGUI's CTextButton (kKickStyle)
    // delivers per click, without a window or a platform init: onMouseUp
    // fires valueChanged once with value=max, once after resetting to min.
    // A real first click must switch the mode -- anything that needs a
    // second click means the press-edge latch was left stale across clicks.
    // A minimal CControl subclass stands in for the real buttons (a real
    // CTextButton's ctor creates a CGradient, which needs the platform).
    NeditEditor editor (&fx.processor);

    struct FakeControl : VSTGUI::CControl
    {
        FakeControl (const VSTGUI::CRect& r, VSTGUI::IControlListener* l, int32_t customTag)
            : CControl (r, l, customTag) {}
        void draw (VSTGUI::CDrawContext*) override {}
        VSTGUI::CBaseObject* newCopy () const override { return new FakeControl (*this); }
        VSTGUI::CMouseEventResult onMouseDown (VSTGUI::CPoint&, const VSTGUI::CButtonState&) override
        {
            return VSTGUI::kMouseEventHandled;
        }
    };

    FakeControl clockBtn (VSTGUI::CRect (0, 0, 4, 4), &editor,
                          NeditEditor::kTagGenerateModeClock);
    const auto clickClock = [&] {
        clockBtn.setValue (1.0f);
        editor.valueChanged (&clockBtn);
        clockBtn.setValue (0.0f);
        editor.valueChanged (&clockBtn);
    };
    FakeControl slBtn (VSTGUI::CRect (0, 0, 4, 4), &editor,
                       NeditEditor::kTagGenerateModeSL);
    const auto clickSl = [&] {
        slBtn.setValue (1.0f);
        editor.valueChanged (&slBtn);
        slBtn.setValue (0.0f);
        editor.valueChanged (&slBtn);
    };

    clickClock();
    CHECK (fx.processor.debugUiState().generate.generateMode
           == state::TriggerMode::clock);
    // The ribbon switch also drives the scheduler-facing trigger mode, so
    // the audio actually changes when the mode flips.
    CHECK (fx.processor.debugUiState().triggerMode == state::TriggerMode::clock);

    // A second click on the same (now-active) segment is a no-op.
    clickClock();
    CHECK (fx.processor.debugUiState().generate.generateMode
           == state::TriggerMode::clock);

    // Back to Slice Length -- again on the FIRST click.
    clickSl();
    CHECK (fx.processor.debugUiState().generate.generateMode
           == state::TriggerMode::sliceLength);
    // Re-selecting the active segment is a no-op.
    clickSl();
    CHECK (fx.processor.debugUiState().generate.generateMode
           == state::TriggerMode::sliceLength);

    // The regression: after EACH successful flip the latch must be reusable,
    // so every later change happens on the FIRST click of its pair. Without
    // the unconditional pressedEdge update (the latch reset short-circuited
    // on the mode-matched min echo), these two would swallow the max echo
    // and leave the mode unchanged until the *next* click.
    clickClock();
    CHECK (fx.processor.debugUiState().generate.generateMode
           == state::TriggerMode::clock);
    clickSl();
    CHECK (fx.processor.debugUiState().generate.generateMode
           == state::TriggerMode::sliceLength);
}

TEST_CASE ("timing ribbon: option menus enable exactly with their mode")
{
    using nedit::ui::timingGreyState;
    using nedit::ui::TimingGreyState;

    // Slice Length: RESET EVERY rides SL, the Clock trio is greyed.
    const TimingGreyState sl = timingGreyState (state::TriggerMode::sliceLength);
    CHECK_FALSE (sl.resetBarsGreyed);
    CHECK (sl.clockRefGreyed);
    CHECK (sl.tapeScopeGreyed);
    CHECK (sl.filterScopeGreyed);

    // Clock: RESET EVERY greys, the Clock trio is enabled.
    const TimingGreyState clk = timingGreyState (state::TriggerMode::clock);
    CHECK (clk.resetBarsGreyed);
    CHECK_FALSE (clk.clockRefGreyed);
    CHECK_FALSE (clk.tapeScopeGreyed);
    CHECK_FALSE (clk.filterScopeGreyed);
}

TEST_CASE ("shell: generate timing setters persist in GenerateState")
{
    RunningPlugin fx;
    const auto& g0 = fx.processor.debugUiState().generate;

    // Defaults per the original: Slice Length, reset every 4 bars, clock
    // reference on the 1/4-note, all subdivision weights even.
    CHECK (g0.generateMode == state::TriggerMode::sliceLength);
    CHECK (g0.resetBarsIndex == state::kDefaultResetBarsIndex);
    CHECK (g0.clockReferenceIndex == state::kNoteValue4n);

    fx.processor.setGenerateMode (state::TriggerMode::clock);
    CHECK (fx.processor.debugUiState().generate.generateMode
           == state::TriggerMode::clock);
    // The Generate sub-modes ARE the scheduler's top-level sliceLength/clock
    // trigger modes, so the setter must drive triggerMode too -- otherwise
    // the ribbon flip changes UI-only state and the audio stays on the old
    // mode. (Regression for "mode change doesn't affect audio".)
    CHECK (fx.processor.debugUiState().triggerMode == state::TriggerMode::clock);

    // Picking a note-value by palette index.
    fx.processor.setClockReference (6);
    CHECK (fx.processor.debugUiState().generate.clockReferenceIndex == 6);

    fx.processor.setResetBars (1);
    CHECK (fx.processor.debugUiState().generate.resetBarsIndex == 1);

    // Subdivision weight writes clamp to [0,1].
    fx.processor.setSubdivisionWeight (2, 0.4f);
    fx.processor.setSubdivisionWeight (18, 1.7f);
    fx.processor.setSubdivisionWeight (4, -0.3f);
    const auto& w = fx.processor.debugUiState().generate.subdivisionWeights;
    CHECK (w[2] == Catch::Approx (0.4f));
    CHECK (w[18] == Catch::Approx (1.0f));
    CHECK (w[4] == Catch::Approx (0.0f));

    // Momentary "n=0"-style group clears zero ONLY their variant's columns.
    // Values stay inside the [0,1] clamp (0.05 * (i+1) tops out at exactly 1.0).
    for (int i = 0; i < state::kNumNoteValues; ++i)
        fx.processor.setSubdivisionWeight (i, 0.05f * static_cast<float> (i + 1));

    fx.processor.setSubdivisionGroupZero (state::NoteValueVariant::plain);
    for (int i = 0; i < state::kNumNoteValues; ++i)
    {
        const auto wt = state::kNoteValueVariant[static_cast<std::size_t> (i)];
        CHECK (w[static_cast<std::size_t> (i)]
               == Catch::Approx (wt == state::NoteValueVariant::plain
                                     ? 0.0f
                                     : 0.05f * static_cast<float> (i + 1)));
    }
    fx.processor.setSubdivisionGroupZero (state::NoteValueVariant::dotted);
    fx.processor.setSubdivisionGroupZero (state::NoteValueVariant::triplet);
    for (int i = 0; i < state::kNumNoteValues; ++i)
        CHECK (w[static_cast<std::size_t> (i)] == Catch::Approx (0.0f));

    // Clearing an already-zero group is a no-op publish (still consistent,
    // never crashes), and a bogus variant value is rejected wholesale.
    fx.processor.setSubdivisionGroupZero (state::NoteValueVariant::plain);
    fx.processor.setSubdivisionGroupZero (static_cast<state::NoteValueVariant> (77));
    for (int i = 0; i < state::kNumNoteValues; ++i)
        CHECK (w[static_cast<std::size_t> (i)] == Catch::Approx (0.0f));

    // Invalid values are ignored wholesale.
    fx.processor.setGenerateMode (state::TriggerMode::sequenced);   // not a Generate mode
    fx.processor.setResetBars (99);
    fx.processor.setClockReference (-1);
    fx.processor.setClockReference (state::kNumNoteValues + 4);
    fx.processor.setSubdivisionWeight (state::kNumNoteValues, 0.9f);  // one past the palette
    const auto& g1 = fx.processor.debugUiState().generate;
    CHECK (g1.generateMode == state::TriggerMode::clock);
    CHECK (g1.resetBarsIndex == 1);
    CHECK (g1.clockReferenceIndex == 6);
    CHECK (g1.subdivisionWeights == w);

    // Flipping back drives the scheduler-facing trigger mode in lockstep.
    fx.processor.setGenerateMode (state::TriggerMode::sliceLength);
    CHECK (fx.processor.debugUiState().triggerMode == state::TriggerMode::sliceLength);
    CHECK (fx.processor.debugUiState().generate.generateMode
           == state::TriggerMode::sliceLength);
}

TEST_CASE ("editor: subdivision quick-clear chips zero the group on the first click")
{
    RunningPlugin fx;
    NeditEditor editor (&fx.processor);

    struct FakeControl : VSTGUI::CControl
    {
        FakeControl (const VSTGUI::CRect& r, VSTGUI::IControlListener* l, int32_t customTag)
            : CControl (r, l, customTag) {}
        void draw (VSTGUI::CDrawContext*) override {}
        VSTGUI::CBaseObject* newCopy () const override { return new FakeControl (*this); }
        VSTGUI::CMouseEventResult onMouseDown (VSTGUI::CPoint&, const VSTGUI::CButtonState&) override
        {
            return VSTGUI::kMouseEventHandled;
        }
    };

    // A real CTextButton click echoes valueChanged(max) then valueChanged(min);
    // the press-edge latch must fire exactly once per click.
    FakeControl clearPlain (VSTGUI::CRect (0, 0, 4, 4), &editor,
                            NeditEditor::kTagClearPlain);
    auto clickPlain = [&] {
        clearPlain.setValue (1.0f);
        editor.valueChanged (&clearPlain);
        clearPlain.setValue (0.0f);
        editor.valueChanged (&clearPlain);
    };

    fx.processor.setGenerateMode (state::TriggerMode::clock);
    auto& w = fx.processor.debugUiState().generate.subdivisionWeights;

    auto paintAll = [&] (float v) {
        for (int i = 0; i < state::kNumNoteValues; ++i)
            fx.processor.setSubdivisionWeight (i, v);
    };

    paintAll (0.7f);
    clickPlain();
    // Only the plain group went to zero; dotted/triplet columns are intact.
    for (int i = 0; i < state::kNumNoteValues; ++i)
    {
        const auto variant = state::kNoteValueVariant[static_cast<std::size_t> (i)];
        CHECK (w[static_cast<std::size_t> (i)]
               == Catch::Approx (variant == state::NoteValueVariant::plain ? 0.0f : 0.7f));
    }

    // The latch must be reusable: repaint and click again -- a stale latch
    // (the press-edge update short-circuited by the min echo) would swallow
    // this second click and leave the group painted.
    paintAll (0.7f);
    clickPlain();
    for (int i = 0; i < state::kNumNoteValues; ++i)
    {
        if (state::kNoteValueVariant[static_cast<std::size_t> (i)]
            == state::NoteValueVariant::plain)
            CHECK (w[static_cast<std::size_t> (i)] == Catch::Approx (0.0f));
    }
}
