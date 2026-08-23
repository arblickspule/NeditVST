// Standalone VST3 host harness to reproduce the Bitwig engine crash
// outside Bitwig: full lifecycle incl. editor open() on the real X
// display, which our unit tests can't do (no X server there).

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
#include <map>
#include <string>
#include <vector>
#include <thread>
#include <chrono>

#include <poll.h>
#include <xcb/xcb.h>

#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>

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

int main (int argc, char** argv)
{
    if (argc < 2) { std::printf("usage: harness <bundle.vst3> [noeditor]\n"); return 2; }
    const bool doEditor = ! (argc >= 3 && std::string(argv[2]) == "noeditor");

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
                uint32_t vals[1] = { 1 };   // override_redirect: keep at 0,0
                xcb_create_window (conn, XCB_COPY_FROM_PARENT, win, screen->root,
                                   0,0, (uint16_t)(vr.right-vr.left>0?vr.right-vr.left:600),
                                   (uint16_t)(vr.bottom-vr.top>0?vr.bottom-vr.top:400),
                                   0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                                   XCB_CW_OVERRIDE_REDIRECT, vals);
                xcb_map_window (conn, win);
                uint32_t stack[1] = { XCB_STACK_MODE_ABOVE };
                xcb_configure_window (conn, win, XCB_CONFIG_WINDOW_STACK_MODE, stack);
                xcb_flush (conn);
                view->setFrame (static_cast<IPlugFrame*> (&host));   // IRunLoop via QI
                STEP("attach editor to X11 window");
                tresult r = view->attached (reinterpret_cast<void*>((uintptr_t)win),
                                            kPlatformTypeX11EmbedWindowID);
                std::printf("[harness] attached -> %d\n", r);

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
