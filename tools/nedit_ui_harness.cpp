// nedit_ui_harness -- cross-platform UI-test harness for the Sequencer grid
// scroll/pan (issue #2).
//
// Unlike editor_smoke (which loads the shipped .vst3 bundle and only checks
// that the editor opens), this harness hosts the plugin IN-PROCESS: it
// constructs a NeditProcessor directly, opens its real VSTGUI editor on a
// native parent window (xcb / NSView / HWND, reusing editor_smoke's platform
// glue), then drives SYNTHETIC mouse + wheel events straight into the live
// CFrame via CFrame::dispatchEvent(). Those events travel the exact same
// hit-test + capture path a real click would, landing in the actual
// SequencerGridView handlers -- the "direct-first" driver from the harness
// design. After each scripted gesture it reads the persisted viewport back
// out of the processor (debugUiState().sequencer.viewport) and asserts it
// responded as the geometry model prescribes.
//
// This is a DEBUG-ONLY tool: it relies on the Debug-only test hooks
// (NeditProcessor::testHookEditor / NeditEditor::testHookFrame /
// testHookSequencerGrid) that are compiled out of release builds. Built in
// the same config as the plugin (Debug, where VSTGUI self-defines DEBUG), so
// the CFrame/CView vtable layout the harness links against matches the one
// inside nedit_plugin.
//
// Usage: nedit_ui_harness [scenario]
//   scenario defaults to "all". Exit 0 = pass, 1 = a check failed, 77 = skip
//   (release build / no display).

#include "editor_smoke_platform.h"

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/vsttypes.h"

// Provided by the SDK module-init TU (moduleinit.cpp / linuxmain.cpp), linked
// via nedit_plugin_entry. Running it in-process fires the ModuleInitializers
// -- crucially InitVSTGUI, which calls VSTGUI::init() to install the platform
// factory (a DAW gets this for free when it loads the .so; we must do it).
bool InitModule ();

#if defined(NDEBUG)
// The test hooks are compiled out of release builds; nothing to drive.
#include <cstdio>
int main ()
{
    std::printf ("[ui-harness] SKIP: requires a Debug build (test hooks compiled out)\n");
    return 77;
}
#else

#include "plugin/NeditEditor.h"
#include "plugin/NeditProcessor.h"
#include "state/PluginState.h"
#include "ui/SequencerGridGeometry.h"

#include "vstgui/lib/cframe.h"
#include "vstgui/lib/cpoint.h"
#include "vstgui/lib/crect.h"
#include "vstgui/lib/events.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace Steinberg;
using namespace Steinberg::Vst;
using namespace nedit;   // plugin::, state::, ui:: live under nedit::

// NOTE: unlike editor_smoke, the harness links nedit_plugin, which pulls the
// SDK's commoniids.cpp (IPlugFrame/IRunLoop/IEventHandler/ITimerHandler iids)
// into the link. So we must NOT DEF_CLASS_IID them here -- that would be a
// multiple-definition. The PlatformHost picks them up from commoniids.

namespace {

// ---------------------------------------------------------------------------
// Tiny assertion harness (no dependency on the test framework -- this runs as
// a standalone CI executable on a live editor).
// ---------------------------------------------------------------------------
int g_failures = 0;
int g_checks = 0;

bool approxEq (double a, double b, double eps = 1e-3)
{
    return std::fabs (a - b) <= eps;
}

void check (bool ok, const char* what)
{
    ++g_checks;
    if (ok)
    {
        std::printf ("[ui-harness]   ok   : %s\n", what);
    }
    else
    {
        std::printf ("[ui-harness]   FAIL : %s\n", what);
        ++g_failures;
    }
    std::fflush (stdout);
}

// ---------------------------------------------------------------------------
// Synthesize a mono 16-bit PCM WAV with a train of sharp-attack, sustained
// segments. Each segment starts with a hard amplitude jump (a clean onset for
// the transient detector) and decays to a non-silent floor (so no segment is
// dropped as a "ghost"/silent slice). Written to `path`; returns false on I/O
// error.
// ---------------------------------------------------------------------------
bool writeClickTrainWav (const std::string& path, int sampleRate, double seconds,
                         int onsets)
{
    const auto frames = static_cast<std::int64_t> (seconds * sampleRate);
    const double segLen = static_cast<double> (frames) / std::max (1, onsets);

    std::vector<std::int16_t> pcm (static_cast<std::size_t> (frames));
    for (std::int64_t i = 0; i < frames; ++i)
    {
        const double posInSeg = std::fmod (static_cast<double> (i), segLen);
        const double tSeg = posInSeg / static_cast<double> (sampleRate);
        // Sharp attack -> decay to a 0.35 floor: onset is the amplitude jump
        // at each segment boundary; the floor keeps the slice audible.
        const double amp = 0.35 + 0.65 * std::exp (-tSeg / 0.02);
        const double tone = std::sin (2.0 * M_PI * 220.0 * static_cast<double> (i)
                                      / static_cast<double> (sampleRate));
        const double s = amp * tone;
        pcm[static_cast<std::size_t> (i)] =
            static_cast<std::int16_t> (std::lround (s * 30000.0));
    }

    const std::uint32_t dataBytes = static_cast<std::uint32_t> (pcm.size() * sizeof (std::int16_t));
    const std::uint32_t byteRate = static_cast<std::uint32_t> (sampleRate) * 2u;

    auto put32 = [] (std::ofstream& os, std::uint32_t v) {
        char b[4] = { char (v & 0xff), char ((v >> 8) & 0xff),
                      char ((v >> 16) & 0xff), char ((v >> 24) & 0xff) };
        os.write (b, 4);
    };
    auto put16 = [] (std::ofstream& os, std::uint16_t v) {
        char b[2] = { char (v & 0xff), char ((v >> 8) & 0xff) };
        os.write (b, 2);
    };

    std::ofstream os (path, std::ios::binary);
    if (! os)
        return false;
    os.write ("RIFF", 4);
    put32 (os, 36u + dataBytes);
    os.write ("WAVE", 4);
    os.write ("fmt ", 4);
    put32 (os, 16u);            // PCM fmt chunk size
    put16 (os, 1u);            // PCM
    put16 (os, 1u);            // mono
    put32 (os, static_cast<std::uint32_t> (sampleRate));
    put32 (os, byteRate);
    put16 (os, 2u);            // block align
    put16 (os, 16u);           // bits per sample
    os.write ("data", 4);
    put32 (os, dataBytes);
    os.write (reinterpret_cast<const char*> (pcm.data()),
              static_cast<std::streamsize> (dataBytes));
    return os.good();
}

// ---------------------------------------------------------------------------
// Direct event driver: constructs the new-style VSTGUI events and dispatches
// them into the frame, exactly as the platform backend would for a real
// pointer. A press that a view consumes captures it (CViewContainer sets the
// mouse-down view), so the follow-up move/up route to that same view.
// ---------------------------------------------------------------------------
struct Driver
{
    VSTGUI::CFrame* frame = nullptr;

    void down (VSTGUI::CPoint p, VSTGUI::MouseButton b)
    {
        VSTGUI::MouseDownEvent e (p, VSTGUI::MouseEventButtonState (b));
        frame->dispatchEvent (e);
    }
    void move (VSTGUI::CPoint p, VSTGUI::MouseButton b = VSTGUI::MouseButton::None)
    {
        VSTGUI::MouseMoveEvent e (p, VSTGUI::MouseEventButtonState (b));
        frame->dispatchEvent (e);
    }
    void up (VSTGUI::CPoint p, VSTGUI::MouseButton b)
    {
        VSTGUI::MouseUpEvent e (p, VSTGUI::MouseEventButtonState (b));
        frame->dispatchEvent (e);
    }
    // One wheel notch at `p`; deltaY sign is the zoom direction (the handler
    // uses the sign only).
    void wheel (VSTGUI::CPoint p, double deltaY)
    {
        VSTGUI::MouseWheelEvent e;
        e.mousePosition = p;
        e.deltaY = deltaY;
        frame->dispatchEvent (e);
    }
};

// The grid's geometry, recomputed the way SequencerGridView::rebuildLayout()
// does, so the harness can aim at the canvas centre and the two scrollbar
// bands. Mirrors SequencerGridView::kPaletteH / kScrollBarThickness.
constexpr double kPaletteH = 18.0;
constexpr double kScrollBarThickness = 10.0;

struct GridGeom
{
    VSTGUI::CRect grid;        // gridRect(view): view minus the top palette strip
    ui::SequencerGridLayout layout;
};

GridGeom gridGeom (plugin::NeditProcessor* proc, VSTGUI::CControl* gridView)
{
    GridGeom gg;
    const VSTGUI::CRect vr = gridView->getViewSize();
    gg.grid = VSTGUI::CRect (vr.left, vr.top + kPaletteH, vr.right, vr.bottom);
    const auto& seq = proc->debugUiState().sequencer;
    gg.layout = ui::computeSequencerLayout (vr.getWidth(), vr.getHeight() - kPaletteH,
                                            seq.rows, seq.columns);
    return gg;
}

ui::SequencerViewport vp (plugin::NeditProcessor* proc)
{
    const auto& v = proc->debugUiState().sequencer.viewport;
    return ui::SequencerViewport { v.zoomX, v.zoomY, v.originX, v.originY };
}

// ---------------------------------------------------------------------------
// Scenarios. Each arranges a deterministic precondition through the processor
// (setSequencerViewport is a legitimate publish-only setter), drives the
// gesture through the CFrame, then asserts the persisted viewport.
// ---------------------------------------------------------------------------

void scenarioDefaults (plugin::NeditProcessor* proc)
{
    std::printf ("[ui-harness] scenario: fresh-viewport defaults\n");
    // A freshly loaded grid opens unzoomed, scrolled to the bottom (slice 0 at
    // the grid's bottom edge, like the original) -- originY == 1.
    const auto v = proc->debugUiState().sequencer.viewport;
    check (approxEq (v.zoomX, 1.0) && approxEq (v.zoomY, 1.0), "default zoom == 1,1");
    check (approxEq (v.originX, 0.0), "default originX == 0");
    check (approxEq (v.originY, 1.0), "default originY == 1 (bottom)");
}

void scenarioCanvasZoom (plugin::NeditProcessor* proc, Driver& drv, VSTGUI::CControl* gridView)
{
    std::printf ("[ui-harness] scenario: canvas wheel zoom (both axes, anchored)\n");
    proc->setSequencerViewport (2.0f, 2.0f, 0.5f, 0.5f);
    const auto gg = gridGeom (proc, gridView);
    const VSTGUI::CPoint center (gg.grid.left + gg.grid.getWidth() * 0.5,
                                 gg.grid.top + gg.grid.getHeight() * 0.5);

    const auto before = vp (proc);
    drv.wheel (center, +1.0);
    const auto after = vp (proc);
    check (after.zoomX > before.zoomX + 1e-4 && after.zoomY > before.zoomY + 1e-4,
           "wheel up over canvas zooms in on BOTH axes");
    check (approxEq (after.zoomX, after.zoomY),
           "canvas zoom scales X and Y equally");

    drv.wheel (center, -1.0);
    const auto back = vp (proc);
    check (back.zoomX < after.zoomX - 1e-4 && back.zoomY < after.zoomY - 1e-4,
           "wheel down over canvas zooms out on both axes");
}

void scenarioZoomClamp (plugin::NeditProcessor* proc, Driver& drv, VSTGUI::CControl* gridView)
{
    std::printf ("[ui-harness] scenario: wheel zoom clamps (max + logical fit-min)\n");
    proc->setSequencerViewport (1.0f, 1.0f, 0.5f, 0.5f);
    const auto gg = gridGeom (proc, gridView);
    const VSTGUI::CPoint center (gg.grid.left + gg.grid.getWidth() * 0.5,
                                 gg.grid.top + gg.grid.getHeight() * 0.5);

    for (int i = 0; i < 60; ++i)
        drv.wheel (center, +1.0);
    check (approxEq (vp (proc).zoomX, state::kMaxSequencerZoom)
               && approxEq (vp (proc).zoomY, state::kMaxSequencerZoom),
           "zoom-in saturates at kMaxSequencerZoom");

    // Zoom-out now stops at the per-axis FIT (all rows/cols visible + filling
    // the view), not the fixed kMinSequencerZoom. Columns always fill at zoom 1
    // (colWidth = viewW/totalCols) so X's fit is 1.0; Y's fit is < 1 while the
    // rows overflow at base zoom.
    for (int i = 0; i < 80; ++i)
        drv.wheel (center, -1.0);
    const double fitX = ui::sequencerMinZoom (gg.layout, false, gg.grid.getWidth());
    const double fitY = ui::sequencerMinZoom (gg.layout, true, gg.grid.getHeight());
    check (approxEq (vp (proc).zoomX, fitX),
           "zoom-out on X saturates at fit (all columns fill the width)");
    check (approxEq (vp (proc).zoomY, fitY),
           "zoom-out on Y saturates at fit (all rows visible + fill the height)");
    check (fitY < 1.0, "Y fit-min is below 1.0 for this many-row sample");
    check (approxEq (ui::sequencerContentExtent (
               gg.layout, true, ui::SequencerViewport { fitX, fitY, 0.0, 0.0 }),
               gg.grid.getHeight(), 0.75),
           "at the Y fit-min the content exactly fills the viewport (no dead space)");
}

void scenarioAxisLock (plugin::NeditProcessor* proc, Driver& drv, VSTGUI::CControl* gridView)
{
    std::printf ("[ui-harness] scenario: wheel over a scrollbar locks its axis\n");
    // Vertical bar: wheel zooms Y only.
    proc->setSequencerViewport (2.0f, 2.0f, 0.5f, 0.5f);
    auto gg = gridGeom (proc, gridView);
    {
        const VSTGUI::CPoint vBar (gg.grid.right - kScrollBarThickness * 0.5,
                                   gg.grid.top + gg.grid.getHeight() * 0.5);
        const auto before = vp (proc);
        drv.wheel (vBar, +1.0);
        const auto after = vp (proc);
        check (after.zoomY > before.zoomY + 1e-4, "V-bar wheel zooms Y");
        check (approxEq (after.zoomX, before.zoomX), "V-bar wheel leaves X unchanged");
    }
    // Horizontal bar: wheel zooms X only.
    proc->setSequencerViewport (2.0f, 2.0f, 0.5f, 0.5f);
    gg = gridGeom (proc, gridView);
    {
        const VSTGUI::CPoint hBar (gg.grid.left + gg.grid.getWidth() * 0.5,
                                   gg.grid.bottom - kScrollBarThickness * 0.5);
        const auto before = vp (proc);
        drv.wheel (hBar, +1.0);
        const auto after = vp (proc);
        check (after.zoomX > before.zoomX + 1e-4, "H-bar wheel zooms X");
        check (approxEq (after.zoomY, before.zoomY), "H-bar wheel leaves Y unchanged");
    }
}

void scenarioMiddleDragPan (plugin::NeditProcessor* proc, Driver& drv, VSTGUI::CControl* gridView)
{
    std::printf ("[ui-harness] scenario: middle-mouse drag pans the viewport\n");
    // Zoom right in so both axes overflow (guaranteed scrollable), centred.
    proc->setSequencerViewport (static_cast<float> (state::kMaxSequencerZoom),
                                static_cast<float> (state::kMaxSequencerZoom),
                                0.5f, 0.5f);
    const auto gg = gridGeom (proc, gridView);
    check (ui::sequencerMaxScroll (gg.layout, false, gg.grid.getWidth(), vp (proc)) > 0.0,
           "content overflows horizontally at max zoom");
    check (ui::sequencerMaxScroll (gg.layout, true, gg.grid.getHeight(), vp (proc)) > 0.0,
           "content overflows vertically at max zoom");

    const VSTGUI::CPoint center (gg.grid.left + gg.grid.getWidth() * 0.5,
                                 gg.grid.top + gg.grid.getHeight() * 0.5);
    const auto before = vp (proc);
    drv.down (center, VSTGUI::MouseButton::Middle);
    // Drag down-right in a few steps: content follows, origins DECREASE
    // (grab-and-move feel: originX/Y -= delta/maxScroll).
    for (int i = 1; i <= 4; ++i)
        drv.move (VSTGUI::CPoint (center.x + 40.0 * i, center.y + 24.0 * i),
                  VSTGUI::MouseButton::Middle);
    drv.up (VSTGUI::CPoint (center.x + 160.0, center.y + 96.0), VSTGUI::MouseButton::Middle);
    const auto after = vp (proc);
    check (after.originX < before.originX - 1e-4, "drag right lowers originX");
    check (after.originY < before.originY - 1e-4, "drag down lowers originY");

    // Drag the other way past the start: origins INCREASE, clamped at 1.
    const auto mid = vp (proc);
    drv.down (center, VSTGUI::MouseButton::Middle);
    for (int i = 1; i <= 6; ++i)
        drv.move (VSTGUI::CPoint (center.x - 60.0 * i, center.y - 40.0 * i),
                  VSTGUI::MouseButton::Middle);
    drv.up (VSTGUI::CPoint (center.x - 360.0, center.y - 240.0), VSTGUI::MouseButton::Middle);
    const auto rev = vp (proc);
    check (rev.originX > mid.originX + 1e-4, "reverse drag raises originX");
    check (rev.originY > mid.originY + 1e-4, "reverse drag raises originY");
    check (rev.originX <= 1.0 + 1e-9 && rev.originY <= 1.0 + 1e-9, "origins stay clamped <= 1");
}

void scenarioScrollbarKnob (plugin::NeditProcessor* proc, Driver& drv, VSTGUI::CControl* gridView)
{
    std::printf ("[ui-harness] scenario: vertical scrollbar knob drag + track paging\n");
    // Knob drag: from originY 0.3, dragging the V knob DOWN raises originY.
    proc->setSequencerViewport (static_cast<float> (state::kMaxSequencerZoom),
                                static_cast<float> (state::kMaxSequencerZoom),
                                0.5f, 0.3f);
    auto gg = gridGeom (proc, gridView);
    {
        const auto bar = ui::computeSequencerScrollBar (true, gg.layout,
                                                        gg.grid.getHeight(), vp (proc));
        check (bar.scrollable, "vertical overlay scrollbar is present at max zoom");
        const double knobMid = (bar.knobStart + bar.knobEnd) * 0.5;
        const double xBar = gg.grid.right - kScrollBarThickness * 0.5;
        const auto before = vp (proc);
        drv.down (VSTGUI::CPoint (xBar, gg.grid.top + knobMid), VSTGUI::MouseButton::Left);
        drv.move (VSTGUI::CPoint (xBar, gg.grid.top + knobMid + 100.0), VSTGUI::MouseButton::Left);
        drv.up (VSTGUI::CPoint (xBar, gg.grid.top + knobMid + 100.0), VSTGUI::MouseButton::Left);
        const auto after = vp (proc);
        check (after.originY > before.originY + 1e-3, "dragging the V knob down raises originY");
        check (approxEq (after.originX, before.originX), "V knob drag leaves originX unchanged");
    }

    // Track click: from originY 0.8, clicking the V track ABOVE the knob pages
    // the view up (originY drops sharply).
    proc->setSequencerViewport (static_cast<float> (state::kMaxSequencerZoom),
                                static_cast<float> (state::kMaxSequencerZoom),
                                0.5f, 0.8f);
    gg = gridGeom (proc, gridView);
    {
        const double xBar = gg.grid.right - kScrollBarThickness * 0.5;
        const auto before = vp (proc);
        drv.down (VSTGUI::CPoint (xBar, gg.grid.top + 12.0), VSTGUI::MouseButton::Left);
        drv.up (VSTGUI::CPoint (xBar, gg.grid.top + 12.0), VSTGUI::MouseButton::Left);
        const auto after = vp (proc);
        check (after.originY < before.originY - 1e-2, "clicking the V track near the top pages originY up");
    }
}

} // namespace

int main (int argc, char** argv)
{
    const std::string scenario = argc >= 2 ? argv[1] : "all";

    // 1) Native platform (parent window + host/runloop), sized to the editor.
    smoke::Platform* platform = smoke::createPlatform (960, 800);
    if (! platform)
    {
        std::printf ("[ui-harness] SKIP: platform creation failed (no display?)\n");
        return 77;
    }
    Steinberg::PlatformHost* host = platform->host();

    // 2) Initialize the module in-process: fires InitVSTGUI (VSTGUI::init ->
    //    platform factory) + registers the Linux host-context callback on the
    //    plugin factory. Then hand the factory our host context so that
    //    callback wires VSTGUI's runloop to the platform (as editor_smoke does
    //    implicitly through the bundle's setHostContext).
    InitModule();
    if (auto* factory = GetPluginFactory())
    {
        FUnknownPtr<IPluginFactory3> f3 (factory);
        if (f3)
            f3->setHostContext (host->unknownCast());
    }

    // 3) Plugin in-process (leaked at exit, like editor_smoke -- the process
    //    owns everything and tears down at exit).
    auto* proc = new plugin::NeditProcessor;
    if (proc->initialize (host->unknownCast()) != kResultOk)
    {
        std::printf ("[ui-harness] FAIL: processor initialize failed\n");
        return 1;
    }

    IPlugView* view = proc->createView (ViewType::kEditor);
    if (! view)
    {
        std::printf ("[ui-harness] FAIL: createView returned null\n");
        return 1;
    }
    view->setFrame (host);
    auto* parent = reinterpret_cast<void*> (platform->parentHandle());
    if (view->attached (parent, platform->platformType()) != kResultOk)
    {
        std::printf ("[ui-harness] FAIL: editor attached() failed\n");
        return 1;
    }
    platform->pump (200);   // let open()/idle run

    // 3) Grab the live frame + grid through the Debug-only hooks.
    plugin::NeditEditor* editor = proc->testHookEditor();
    if (! editor)
    {
        std::printf ("[ui-harness] FAIL: testHookEditor() null after attach\n");
        return 1;
    }
    VSTGUI::CFrame* frame = editor->testHookFrame();
    VSTGUI::CControl* gridView = editor->testHookSequencerGrid();
    if (! frame || ! gridView)
    {
        std::printf ("[ui-harness] FAIL: null frame/grid from test hooks\n");
        return 1;
    }

    // 4) Load a synthesized sample so the grid has rows, then show the
    //    Sequence tab (idle makes the grid visible).
    const std::string wav = "/tmp/nedit_ui_harness_sample.wav";
    if (! writeClickTrainWav (wav, 44100, 3.0, 24))
    {
        std::printf ("[ui-harness] FAIL: could not write temp WAV\n");
        return 1;
    }
    if (! proc->requestSampleLoad (wav))
    {
        std::printf ("[ui-harness] FAIL: requestSampleLoad failed\n");
        return 1;
    }
    proc->setActiveTab (state::UiTab::sequence);
    platform->pump (150);

    std::printf ("[ui-harness] slices=%d rows=%d cols=%d gridVisible=%d\n",
                 proc->debugSliceCount(), proc->debugUiState().sequencer.rows,
                 proc->debugUiState().sequencer.columns, gridView->isVisible() ? 1 : 0);
    std::fflush (stdout);

    check (proc->debugSliceCount() >= 4, "sample yielded >= 4 slices");
    check (proc->debugUiState().sequencer.rows >= 4, "sequencer grid has >= 4 rows");
    check (gridView->isVisible(), "sequencer grid is visible on the Sequence tab");

    Driver drv { frame };

    // 5) Run scenarios.
    const bool all = (scenario == "all");
    if (all || scenario == "defaults")     scenarioDefaults (proc);
    if (all || scenario == "canvas-zoom")  scenarioCanvasZoom (proc, drv, gridView);
    if (all || scenario == "zoom-clamp")   scenarioZoomClamp (proc, drv, gridView);
    if (all || scenario == "axis-lock")    scenarioAxisLock (proc, drv, gridView);
    if (all || scenario == "pan")          scenarioMiddleDragPan (proc, drv, gridView);
    if (all || scenario == "scrollbar")    scenarioScrollbarKnob (proc, drv, gridView);

    // 6) Close the editor and confirm the Debug hook is dropped (no dangling
    //    view handed out after close()).
    view->removed();
    check (proc->testHookEditor() == nullptr, "test-hook editor cleared on close()");

    platform->pump (20);

    std::printf ("[ui-harness] %d/%d checks passed\n", g_checks - g_failures, g_checks);
    if (g_failures != 0)
    {
        std::printf ("[ui-harness] RESULT: FAIL (%d)\n", g_failures);
        return 1;
    }
    std::printf ("[ui-harness] RESULT: OK\n");
    return 0;
}

#endif // NDEBUG
