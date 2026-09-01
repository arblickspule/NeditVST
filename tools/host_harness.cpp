// Standalone VST3 host harness to reproduce the Bitwig engine crash
// outside Bitwig: full lifecycle incl. editor open() on the real X
// display, which our unit tests can't do (no X server there).

// IMPORTANT: must be the FIRST include. NeditProcessor pulls in
// vstsinglecomponenteffect.h which #defines setState->setEditorState /
// getState->getEditorState for the whole SDK header block it parses (then
// #undefs them), so IComponent/IEditController/EditController all see
// consistent names. If any pluginterfaces/vst header were parsed first,
// EditController::setState (vsteditcontroller.h) would appear to override
// nothing and the SDK header fails to compile.
#include "plugin/NeditProcessor.h"

#include "state/Types.h"

#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"

#include "pluginterfaces/base/funknownimpl.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/vsttypes.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <csignal>
#include <map>
#include <string>
#include <vector>
#include <thread>
#include <chrono>

#include <poll.h>
#include <xcb/xcb.h>

#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>

using namespace Steinberg;
using namespace Steinberg::Vst;

DEF_CLASS_IID (IPlugFrame)
DEF_CLASS_IID (Linux::IRunLoop)
DEF_CLASS_IID (Linux::IEventHandler)
DEF_CLASS_IID (Linux::ITimerHandler)

// Minimal host implementing IHostApplication + IPlugFrame + Linux::IRunLoop
// so VSTGUI's X11 backend can register its xcb fd/timers -- exactly what a
// real host provides. Plain multiple inheritance + manual FUnknown.
class HarnessHost : public IHostApplication, public IPlugFrame, public Linux::IRunLoop
{
public:
    tresult PLUGIN_API queryInterface (const TUID iid, void** obj) override
    {
        if (FUnknownPrivate::iidEqual (iid, FUnknown::iid) ||
            FUnknownPrivate::iidEqual (iid, IHostApplication::iid))
        { *obj = static_cast<IHostApplication*> (this); addRef(); return kResultOk; }
        if (FUnknownPrivate::iidEqual (iid, IPlugFrame::iid))
        { *obj = static_cast<IPlugFrame*> (this); addRef(); return kResultOk; }
        if (FUnknownPrivate::iidEqual (iid, Linux::IRunLoop::iid))
        { *obj = static_cast<Linux::IRunLoop*> (this); addRef(); return kResultOk; }
        *obj = nullptr; return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1000; }
    uint32 PLUGIN_API release() override { return 1000; }

    tresult PLUGIN_API getName (String128 name) override
    {
        static const char16_t n[] = u"NeditHarness";
        std::memcpy (name, n, sizeof (n));
        return kResultTrue;
    }
    tresult PLUGIN_API createInstance (TUID, TUID, void**) override { return kNotImplemented; }

    tresult PLUGIN_API resizeView (IPlugView*, ViewRect*) override { return kResultTrue; }

    tresult PLUGIN_API registerEventHandler (Linux::IEventHandler* h, Linux::FileDescriptor fd) override
    { fds_.push_back ({fd, h}); return kResultTrue; }
    tresult PLUGIN_API unregisterEventHandler (Linux::IEventHandler* h) override
    {
        for (auto it = fds_.begin(); it != fds_.end();)
            it = (it->second == h) ? fds_.erase (it) : it + 1;
        return kResultTrue;
    }
    tresult PLUGIN_API registerTimer (Linux::ITimerHandler* h, Linux::TimerInterval ms) override
    { timers_.push_back ({h, ms}); return kResultTrue; }
    tresult PLUGIN_API unregisterTimer (Linux::ITimerHandler* h) override
    {
        for (auto it = timers_.begin(); it != timers_.end();)
            it = (it->first == h) ? timers_.erase (it) : it + 1;
        return kResultTrue;
    }

    FUnknown* unknownCast() { return static_cast<IHostApplication*> (this); }

    // Merge registered event-handler fds into an external poll set (indices
    // starting at offset) so the caller can include them in a single poll().
    void gatherPollFds (std::vector<pollfd>& out) const
    {
        for (auto& [fd, handler] : fds_)
            out.push_back ({fd, POLLIN, 0});
    }

    // After poll() returns, dispatch any ready fds that belong to us.
    // baseIdx is the pollfd index where our fds start in the caller's vector.
    void dispatchPollResults (const std::vector<pollfd>& pfds, std::size_t baseIdx = 1)
    {
        for (std::size_t i = 0; i < fds_.size (); ++i)
        {
            auto pi = baseIdx + i;
            if (pi < pfds.size () && (pfds[pi].revents & POLLIN))
                fds_[i].second->onFDIsSet (fds_[i].first);
        }
    }

    // Fire all registered timers (VSTGUI idle, rendering, etc.).
    void fireTimers ()
    {
        for (auto& [handler, interval] : timers_)
            handler->onTimer ();
    }

    void pump (int ms)
    {
        std::vector<pollfd> pfds;
        for (auto& e : fds_) pfds.push_back ({e.first, POLLIN, 0});
        if (! pfds.empty())
            ::poll (pfds.data(), pfds.size(), ms);
        else
            std::this_thread::sleep_for (std::chrono::milliseconds (ms));
        for (std::size_t i = 0; i < pfds.size(); ++i)
            if (pfds[i].revents & POLLIN)
                fds_[i].second->onFDIsSet (fds_[i].first);
        for (auto& t : timers_) t.first->onTimer();
    }

private:
    std::vector<std::pair<Linux::FileDescriptor, Linux::IEventHandler*>> fds_;
    std::vector<std::pair<Linux::ITimerHandler*, Linux::TimerInterval>> timers_;
};

#define STEP(msg) std::printf("[harness] %s\n", msg); std::fflush(stdout);

static volatile sig_atomic_t g_running = 1;
static void sigIntHandler (int) { g_running = 0; }

// Interactive preview: open the editor in a real window and pump events
// until the user closes it or presses Ctrl-C.
int main (int argc, char** argv)
{
    if (argc < 2) { std::printf("usage: harness <bundle.vst3> [noeditor|preview]\n"); return 2; }
    const char* mode = argc >= 3 ? argv[2] : "editor";
    const bool doEditor = (std::string(mode) != "noeditor");
    const bool preview  = (std::string(mode) == "preview");

    static HarnessHost host;

    std::string err;
    STEP("loading module");
    auto module = VST3::Hosting::Module::create (argv[1], err);
    if (! module) { std::printf("load failed: %s\n", err.c_str()); return 1; }

    auto factory = module->getFactory();
    // Give the plugin its host context so its VSTGUI runloop callback fires.
    if (auto f3 = U::cast<IPluginFactory3> (factory.get()))
    {
        STEP("setHostContext");
        f3->setHostContext (host.unknownCast());
    }
    IPtr<IComponent> component;
    for (auto& ci : factory.classInfos())
    {
        if (ci.category() == kVstAudioEffectClass)
        {
            component = factory.createInstance<IComponent> (ci.ID());
            break;
        }
    }
    if (! component) { std::printf("no audio class\n"); return 1; }

    STEP("initialize");
    if (component->initialize (host.unknownCast()) != kResultOk) { std::printf("init failed\n"); return 1; }

    FUnknownPtr<IAudioProcessor> processor (component);
    FUnknownPtr<IEditController> controller (component);
    if (! processor) { std::printf("no IAudioProcessor\n"); return 1; }
    std::printf("[harness] controller via component: %p\n", (void*)controller.getInterface());

    STEP("setupProcessing");
    ProcessSetup setup {};
    setup.processMode = kRealtime;
    setup.symbolicSampleSize = kSample32;
    setup.maxSamplesPerBlock = 512;
    setup.sampleRate = 48000.0;
    processor->setupProcessing (setup);

    component->setActive (true);
    processor->setProcessing (true);

    STEP("process a few blocks");
    float left[512] = {0}; float right[512] = {0};
    float* chans[2] = { left, right };
    AudioBusBuffers out {}; out.numChannels = 2; out.channelBuffers32 = chans;
    ProcessData data {};
    data.numSamples = 512; data.numOutputs = 1; data.outputs = &out;
    data.processMode = kRealtime; data.symbolicSampleSize = kSample32;
    for (int i = 0; i < 4; ++i) processor->process (data);

    if (doEditor && controller)
    {
        STEP("createView editor");
        IPlugView* view = controller->createView (ViewType::kEditor);
        if (view)
        {
            ViewRect vr {}; view->getSize (&vr);
            std::printf("[harness] view size %d x %d\n", vr.right-vr.left, vr.bottom-vr.top);

            // Create a real X11 window and attach.
            xcb_connection_t* conn = xcb_connect (nullptr, nullptr);
            if (conn && ! xcb_connection_has_error (conn))
            {
                const xcb_setup_t* s = xcb_get_setup (conn);
                xcb_screen_t* screen = xcb_setup_roots_iterator (s).data;
                xcb_window_t win = xcb_generate_id (conn);
                const uint16_t wW = (uint16_t)(vr.right-vr.left>0?vr.right-vr.left:760);
                const uint16_t wH = (uint16_t)(vr.bottom-vr.top>0?vr.bottom-vr.top:440);
                uint32_t vals[1] = { XCB_EVENT_MASK_STRUCTURE_NOTIFY
                                   | XCB_EVENT_MASK_EXPOSURE
                                   | XCB_EVENT_MASK_KEY_PRESS
                                   | XCB_EVENT_MASK_BUTTON_PRESS
                                   | XCB_EVENT_MASK_BUTTON_RELEASE
                                   | XCB_EVENT_MASK_POINTER_MOTION };
                xcb_create_window (conn, XCB_COPY_FROM_PARENT, win, screen->root,
                                   100, 100, wW, wH,
                                   0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                                   XCB_CW_EVENT_MASK, vals);
                // Set WM_NAME so the window is identifiable.
                xcb_change_property (conn, XCB_PROP_MODE_REPLACE, win,
                                     XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8,
                                     11, "Nedit Preview");
                xcb_map_window (conn, win);
                uint32_t stack[1] = { XCB_STACK_MODE_ABOVE };
                xcb_configure_window (conn, win, XCB_CONFIG_WINDOW_STACK_MODE, stack);
                xcb_flush (conn);
                view->setFrame (static_cast<IPlugFrame*> (&host));   // IRunLoop via QI
                STEP("attach editor to X11 window");
                tresult r = view->attached (reinterpret_cast<void*>((uintptr_t)win),
                                            kPlatformTypeX11EmbedWindowID);
                std::printf("[harness] attached -> %d\n", r);

                if (preview)
                {
                    STEP("interactive preview -- close window or Ctrl-C to exit");
                    std::signal (SIGINT, sigIntHandler);

                    Display* dpy = XOpenDisplay (nullptr);

                    xcb_intern_atom_cookie_t wmProto =
                        xcb_intern_atom (conn, 1, 12, "WM_PROTOCOLS");
                    xcb_intern_atom_cookie_t wmDel =
                        xcb_intern_atom (conn, 1, 16, "WM_DELETE_WINDOW");
                    xcb_intern_atom_reply_t* wmProtoR =
                        xcb_intern_atom_reply (conn, wmProto, nullptr);
                    xcb_intern_atom_reply_t* wmDelR =
                        xcb_intern_atom_reply (conn, wmDel, nullptr);
                    xcb_atom_t wmDeleteAtom = 0;
                    if (wmProtoR && wmDelR)
                    {
                        wmDeleteAtom = wmDelR->atom;
                        xcb_change_property (conn, XCB_PROP_MODE_REPLACE, win,
                                             wmProtoR->atom, 4, 32, 1, &wmDeleteAtom);
                    }
                    free (wmProtoR); free (wmDelR);
                    xcb_flush (conn);

                    while (g_running)
                    {
                        // Collect our xcb fd + VSTGUI runloop fds into one
                        // poll set so everything wakes up together.
                        std::vector<pollfd> pfds;
                        pfds.push_back ({xcb_get_file_descriptor (conn), POLLIN, 0});
                        // Let the harness expose its registered fds for merging.
                        host.gatherPollFds (pfds);
                        ::poll (pfds.data (), pfds.size (), 16); // ~60 fps

                        // Dispatch our own xcb events.
                        xcb_generic_event_t* ev;
                        while ((ev = xcb_poll_for_event (conn)))
                        {
                            const uint8_t type = ev->response_type & ~0x80;
                            if (type == XCB_CLIENT_MESSAGE)
                            {
                                auto* cm = reinterpret_cast<xcb_client_message_event_t*>(ev);
                                if (wmDeleteAtom && cm->data.data32[0] == wmDeleteAtom)
                                    g_running = 0;
                            }
                            else if (type == XCB_KEY_PRESS && dpy)
                            {
                                auto* kp = reinterpret_cast<xcb_key_press_event_t*>(ev);
                                XKeyEvent xev {};
                                xev.display = dpy;
                                xev.keycode = kp->detail;
                                if (XLookupKeysym (&xev, 0) == XK_q)
                                    g_running = 0;
                            }
                            free (ev);
                        }

                        // Dispatch VSTGUI runloop fds + timers.
                        host.dispatchPollResults (pfds, 1);
                        host.fireTimers ();
                        processor->process (data);
                    }

                    STEP("shutting down");
                    if (dpy) XCloseDisplay (dpy);
                    view->removed();
                    xcb_destroy_window (conn, win); xcb_flush (conn);
                    xcb_disconnect (conn);
                    view->release();
                    processor->setProcessing (false);
                    component->setActive (false);
                    component->terminate();
                    return 0;
                }

                STEP("pump + process with editor open");
                for (int i = 0; i < 20; ++i)
                {
                    processor->process (data);
                    host.pump (20);
                }

                // Inject a REAL click on the Load button via XTEST (synthetic
                // SendEvent clicks are ignored by X). Drives the async file-
                // dialog path; with NEDIT_TEST_FILE set it returns at once.
                STEP("click Load button (XTEST)");
                    // Play XEmbed host: VSTGUI maps its embedded child window
                    // only after the parent sends _XEMBED EMBEDDED_NOTIFY.
                    // Without this the child stays unmapped (no rendering,
                    // no input), which silently disables every real click.
                    xcb_intern_atom_cookie_t embAt = xcb_intern_atom (conn, 0, 7, "_XEMBED");
                    if (xcb_intern_atom_reply_t* embR = xcb_intern_atom_reply (conn, embAt, nullptr))
                    {
                        xcb_window_t child = 0;
                        xcb_query_tree_cookie_t qt2 = xcb_query_tree (conn, win);
                        if (xcb_query_tree_reply_t* tr2 = xcb_query_tree_reply (conn, qt2, nullptr))
                        {
                            xcb_window_t* kids = xcb_query_tree_children (tr2);
                            if (xcb_query_tree_children_length (tr2) > 0)
                                child = kids[0];
                            free (tr2);
                        }
                        if (child)
                        {
                            // XEmbed EMBEDDED_NOTIFY (op 0) + XEMBED_VERSION 15.
                            xcb_client_message_event_t msg{};
                            msg.response_type = XCB_CLIENT_MESSAGE;
                            msg.format = 32;
                            msg.window = child;
                            msg.type = embR->atom;
                            msg.data.data32[0] = 0;
                            msg.data.data32[1] = 0;                 // EMBEDDED_NOTIFY
                            msg.data.data32[2] = 15;                // XEMBED_VERSION
                            msg.data.data32[3] = 0;
                            msg.data.data32[4] = 0;
                            xcb_send_event (conn, 0, child, XCB_EVENT_MASK_NO_EVENT, (const char*) &msg);
                            xcb_flush (conn);
                            host.pump (40);
                            host.pump (40);
                        }
                        free (embR);
                    }
                if (Display* dpy = XOpenDisplay (nullptr))
                {
                    // Translate the button's window coords to root coords.
                    int rx = 0, ry = 0; xcb_window_t childRet;
                    const int bx = (vr.right - vr.left) - 90;
                    const int by = 20;
                    xcb_translate_coordinates_cookie_t tc =
                        xcb_translate_coordinates (conn, win, screen->root, (int16_t) bx, (int16_t) by);
                    if (auto* tr = xcb_translate_coordinates_reply (conn, tc, nullptr))
                    { rx = tr->dst_x; ry = tr->dst_y; childRet = tr->child; (void) childRet; free (tr); }
                    XTestFakeMotionEvent (dpy, -1, rx, ry, CurrentTime); XFlush (dpy);
                    host.pump (30);
                    XTestFakeButtonEvent (dpy, 1, True, CurrentTime); XFlush (dpy);
                    host.pump (30);
                    XTestFakeButtonEvent (dpy, 1, False, CurrentTime); XFlush (dpy);
                    XFlush (dpy);
                    for (int i = 0; i < 10; ++i) host.pump (20);
                    XCloseDisplay (dpy);
                }

                STEP("pump + process after click (dialog + idle load)");
                for (int i = 0; i < 40; ++i)
                {
                    processor->process (data);
                    host.pump (20);
                }

                // ---- Mode-switch one-click diagnostic (real XTEST) ----
                // Each click below is ONE physical press (down+up). The
                // editor-local CLOCK segment spans x 481..924, y 484..520;
                // SLICE LENGTH spans x 36..480 (window == editor size, so
                // window coords are editor coords). After each single click
                // we read the generate mode back: it must flip on the FIRST
                // click, and re-clicking the now-active segment must not.
                // This reproduces the DAW's real widget event path (the
                // unit-test replay can't construct a live CTextButton).
                auto* ned = static_cast<nedit::plugin::NeditProcessor*> (
                    static_cast<Steinberg::Vst::SingleComponentEffect*> (
                        component.get()));
                const auto modeName = [] (nedit::state::TriggerMode m) {
                    return m == nedit::state::TriggerMode::sliceLength ? "sliceLength"
                         : m == nedit::state::TriggerMode::clock         ? "clock"
                         : m == nedit::state::TriggerMode::sequenced     ? "sequenced"
                         : m == nedit::state::TriggerMode::performance   ? "performance"
                                                                        : "control";
                };
                const auto clickAt = [&] (int wx, int wy, Display* dpy, const char* label) {
                    xcb_translate_coordinates_cookie_t tc =
                        xcb_translate_coordinates (conn, win, screen->root,
                                                   (int16_t) wx, (int16_t) wy);
                    int rx = wx, ry = wy;
                    if (auto* tr = xcb_translate_coordinates_reply (conn, tc, nullptr))
                    { rx = tr->dst_x; ry = tr->dst_y; free (tr); }
                    XTestFakeMotionEvent (dpy, -1, rx, ry, CurrentTime); XFlush (dpy);
                    host.pump (20);
                    XTestFakeButtonEvent (dpy, 1, True, CurrentTime);  XFlush (dpy);
                    host.pump (20);
                    XTestFakeButtonEvent (dpy, 1, False, CurrentTime); XFlush (dpy);
                    XFlush (dpy);
                    for (int i = 0; i < 6; ++i) host.pump (20);
                    std::printf ("[harness]   single click <<%s>> at (%d,%d) -> root (%d,%d)\n",
                                 label, wx, wy, rx, ry);
                };
                if (Display* dpy = XOpenDisplay (nullptr))
                {
                    std::printf ("[harness] mode before clicks = %s (expect sliceLength)\n",
                                 modeName (ned->debugUiState().generate.generateMode));
                    STEP("one-click mode switch round-trip (XTEST)");
                    clickAt (702, 502, dpy, "CLOCK");
                    const auto afterClock =
                        modeName (ned->debugUiState().generate.generateMode);
                    std::printf ("[harness]   mode after 1st CLOCK click = %s -> %s\n",
                                 afterClock, afterClock == std::string ("clock")
                                                 ? "PASS (first click flips)"
                                                 : "FAIL (needs a second click)");
                    clickAt (702, 502, dpy, "CLOCK again");
                    const auto afterClockRe =
                        modeName (ned->debugUiState().generate.generateMode);
                    std::printf ("[harness]   mode after re-click CLOCK = %s -> %s\n",
                                 afterClockRe,
                                 afterClockRe == std::string ("clock")
                                     ? "PASS (re-select is a no-op)"
                                     : "FAIL (re-select toggled)");
                    // Back to the Generate page's default layout the segments
                    // are at y 484..520; the SLICE LENGTH click must flip back.
                    // (Sequence: CLOCK selected, so a click on SL switches.)
                    clickAt (258, 502, dpy, "SLICE LENGTH");
                    const auto afterSl =
                        modeName (ned->debugUiState().generate.generateMode);
                    std::printf ("[harness]   mode after 1st SL click = %s -> %s\n",
                                 afterSl, afterSl == std::string ("sliceLength")
                                              ? "PASS (clicked back)"
                                              : "FAIL (did not flip back)");
                    // Second full round-trip -- this is the regression the
                    // first-round sequence could NOT catch (it never re-clicked
                    // CLOCK after SL, so the stale latch never surfaced).
                    clickAt (702, 502, dpy, "CLOCK (round 2)");
                    const auto round2Clock =
                        modeName (ned->debugUiState().generate.generateMode);
                    std::printf ("[harness]   mode after CLOCK round 2 = %s -> %s\n",
                                 round2Clock,
                                 round2Clock == std::string ("clock")
                                     ? "PASS (first click of round 2 flips)"
                                     : "FAIL (regressed to two-click)");
                    clickAt (258, 502, dpy, "SLICE LENGTH (round 2)");
                    const auto round2Sl =
                        modeName (ned->debugUiState().generate.generateMode);
                    std::printf ("[harness]   mode after SL round 2 = %s -> %s\n",
                                 round2Sl,
                                 round2Sl == std::string ("sliceLength")
                                     ? "PASS (clicked back round 2)"
                                     : "FAIL (regressed round 2)");
                    STEP("single click SEQUENCER tab (XTEST, hits tab strip y 192..240)");
                    clickAt (400, 216, dpy, "SEQUENCER tab");
                    std::printf ("[harness]   activeTab after tab click = %d -> %s\n",
                                 static_cast<int> (ned->debugUiState().ui.activeTab),
                                 static_cast<int> (ned->debugUiState().ui.activeTab) == 1
                                     ? "PASS" : "FAIL");
                    XCloseDisplay (dpy);
                }

                // Prove the click path actually loaded audio: play transport
                // (Slice Length is the default mode), measure output, and dump
                // it to a WAV so the rendered audio can be inspected offline.
                STEP("play transport, measure output energy");
                float peak = 0.f; double ppq = 0.0;
                std::vector<float> dumpL, dumpR;
                const char* dumpPath = std::getenv ("NEDIT_DUMP_WAV");
                for (int blk = 0; blk < 200; ++blk)   // ~2.1s at 48k/512
                {
                    ProcessContext pc {};
                    pc.state = ProcessContext::kPlaying | ProcessContext::kTempoValid
                             | ProcessContext::kProjectTimeMusicValid;
                    pc.tempo = 120.0; pc.projectTimeMusic = ppq;
                    data.processContext = &pc;
                    // Poison the buffers to mimic a host that hands over dirty
                    // memory -- proves the processor clears before adding.
                    for (int s = 0; s < 512; ++s) { left[s] = 0.777f; right[s] = -0.777f; }
                    processor->process (data);
                    for (int s = 0; s < 512; ++s) peak = std::max (peak, std::abs (left[s]));
                    if (dumpPath)
                    {
                        dumpL.insert (dumpL.end(), left, left + 512);
                        dumpR.insert (dumpR.end(), right, right + 512);
                    }
                    ppq += 512.0 * (120.0/60.0) / 48000.0;
                    host.pump (2);
                }
                data.processContext = nullptr;
                std::printf ("[harness] post-load output peak = %.4f (%s)\n",
                             peak, peak > 0.001f ? "AUDIBLE" : "silent");
                if (dumpPath && ! dumpL.empty())
                {
                    // Minimal 16-bit stereo WAV writer.
                    std::FILE* f = std::fopen (dumpPath, "wb");
                    if (f)
                    {
                        auto w32 = [&] (uint32_t v) { std::fwrite (&v, 4, 1, f); };
                        auto w16 = [&] (uint16_t v) { std::fwrite (&v, 2, 1, f); };
                        const uint32_t n = (uint32_t) dumpL.size();
                        const uint32_t dataBytes = n * 2 * 2;
                        std::fwrite ("RIFF", 1, 4, f); w32 (36 + dataBytes);
                        std::fwrite ("WAVE", 1, 4, f); std::fwrite ("fmt ", 1, 4, f);
                        w32 (16); w16 (1); w16 (2); w32 (48000); w32 (48000*4); w16 (4); w16 (16);
                        std::fwrite ("data", 1, 4, f); w32 (dataBytes);
                        for (uint32_t i = 0; i < n; ++i)
                        {
                            auto q = [] (float x) {
                                long v = std::lround (x * 32767.0f);
                                return (int16_t) (v < -32768 ? -32768 : v > 32767 ? 32767 : v);
                            };
                            w16 ((uint16_t) q (dumpL[i])); w16 ((uint16_t) q (dumpR[i]));
                        }
                        std::fclose (f);
                        std::printf ("[harness] wrote %s (%u frames)\n", dumpPath, n);
                    }
                }
                STEP("onSize shrink/restore round-trip (host-size fit)");
                {
                    Steinberg::ViewRect full (0, 0, 960, 800);
                    Steinberg::ViewRect small (0, 0, 960, 480);
                    view->onSize (&small);   // NeditEditor::onSize -> refit -> scale 0.5
                    host.pump (20);
                    view->onSize (&full);    // back to design size -> identity
                    host.pump (20);
                    std::printf ("[harness] onSize round-trip done\n");
                }
                STEP("removed editor");
                view->removed();
                xcb_destroy_window (conn, win); xcb_flush (conn);
                xcb_disconnect (conn);
            }
            else STEP("xcb_connect failed - skipping attach");
            view->release();
        }
        else STEP("createView returned null");
    }

    STEP("teardown");
    processor->setProcessing (false);
    component->setActive (false);
    component->terminate();
    STEP("DONE clean");
    return 0;
}
