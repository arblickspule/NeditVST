// editor_smoke -- cross-platform editor-open smoke test for CI.
//
// Loads a nedit.vst3 bundle through the real VST3 hosting API, creates the
// component, opens its editor on a native parent window (xcb / NSView /
// HWND), pumps the platform event loop so VSTGUI's open()/idle actually
// run, then detaches and exits. A nonzero exit (including a SIGSEGV mid-
// open, like the vtable-mismatch crash in issue #25) fails the CI run.
//
// Usage: editor_smoke <bundle.vst3> [soak-ms] [instances]
//
// This must run when the plugin was built with the SAME config that a host
// would load (esp. Debug, where VSTGUI self-defines DEBUG): only then do
// the vtable-layout mismatches the smoke guards against actually exist.

#include "editor_smoke_platform.h"

#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/funknownimpl.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/vsttypes.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace Steinberg;
using namespace Steinberg::Vst;

// Define the static iid members for the host-side interfaces we implement
// (the headers only declare them). Exactly one TU per binary.
DEF_CLASS_IID (IPlugFrame)
DEF_CLASS_IID (Linux::IRunLoop)
DEF_CLASS_IID (Linux::IEventHandler)
DEF_CLASS_IID (Linux::ITimerHandler)

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf ("usage: editor_smoke <bundle.vst3> [soak-ms] [instances] [reopen-cycles]\n");
        return 2;
    }
    const std::string bundlePath = argv[1];
    const int soakMs = argc >= 3 ? std::atoi (argv[2]) : 500;
    const int instances = argc >= 4 ? std::atoi (argv[3]) : 1;
    const int reopenCycles = argc >= 5 ? std::atoi (argv[4]) : 0;
    if (instances < 1 || instances > 8)
    {
        std::printf ("[smoke] instances must be 1..8\n");
        return 2;
    }

    std::string err;
    auto module = VST3::Hosting::Module::create (bundlePath, err);
    if (! module)
    {
        std::printf ("[smoke] module load failed: %s\n", err.c_str ());
        return 1;
    }
    auto factory = module->getFactory ();
    auto f3 = U::cast<IPluginFactory3> (factory.get());

    // Create the native platform (window + host) before touching the plugin:
    // the platform OWNS the single host object behind platform->host(), which
    // is the IHostApplication + (on Linux) IRunLoop we pass to the plugin.
    // It is intentionally never freed -- the plugin's .so stores a
    // FUnknownPtr<IRunLoop> in a static that outlives main(), so our host
    // must outlive it (same as a real DAW host).
    //
    // One platform per instance: like a DAW engine process, all instances
    // share the one process-global X11 display/runloop but each has its own
    // native parent window. Opening instance 2's editor is exactly the
    // second-instance scenario that used to clobber the shared runloop
    // (see NeditEditor::open), so exercising >1 instance here both
    // reproduces the bug (pre-fix) and guards it going forward.
    std::vector<smoke::Platform*> platforms;
    std::vector<void*> parents;
    for (int i = 0; i < instances; ++i)
    {
        smoke::Platform* p = smoke::createPlatform (960, 800);
        if (! p)
        {
            std::printf ("[smoke] platform %d creation failed (no X display?)\n", i);
            return 1;
        }
        platforms.push_back (p);
        parents.push_back (reinterpret_cast<void*> (p->parentHandle ()));
    }
    auto* host = platforms[0]->host ();
    std::printf ("[smoke] %d instance(s), %d ms soak\n", instances, soakMs);
    fflush (stdout);

    if (f3)
    {
        if (f3->setHostContext (host->unknownCast ()) != kResultOk)
            std::printf ("[smoke] setHostContext failed (continuing)\n");
    }

    // Open all instances first (each editor open() runs setupVSTGUIRunloop
    // guarded by the first-setter-wins check), THEN pump: pumping together
    // drives every instance's idle timer + X11 fd handlers through the one
    // shared runloop, which is where the multi-instance corruption used to
    // fire. Attaching all first, then pumping all, maximizes the chance of
    // catching a mis-routed handler.
    std::vector<IPtr<IEditController>> controllers;
    std::vector<IPlugView*> views;
    for (int i = 0; i < instances; ++i)
    {
        IPtr<IComponent> component;
        for (const auto& ci : factory.classInfos ())
        {
            if (ci.category () == kVstAudioEffectClass)
            {
                component = factory.createInstance<IComponent> (ci.ID ());
                break;
            }
        }
        if (! component)
        {
            std::printf ("[smoke] instance %d: no audio component class\n", i);
            return 1;
        }
        if (component->initialize (host->unknownCast ()) != kResultOk)
        {
            std::printf ("[smoke] instance %d: component initialize failed\n", i);
            return 1;
        }

        FUnknownPtr<IEditController> controller (component);
        if (! controller)
        {
            std::printf ("[smoke] instance %d: no IEditController\n", i);
            return 1;
        }

        IPlugView* view = controller->createView (ViewType::kEditor);
        if (! view)
        {
            std::printf ("[smoke] instance %d: createView(kEditor) returned null\n", i);
            return 1;
        }

        ViewRect vr {};
        view->getSize (&vr);
        std::printf ("[smoke] instance %d: editor size %d x %d\n", i,
                     vr.right - vr.left, vr.bottom - vr.top);

        view->setFrame (host);

        std::printf ("[smoke] instance %d: attaching to platform parent\n", i);
        fflush (stdout);
        const tresult attached =
            view->attached (parents[static_cast<std::size_t> (i)],
                            platforms[static_cast<std::size_t> (i)]->platformType ());

        // A vtable/ODR mismatch (issue #25) crashes open() before we ever get
        // here -- that shows up as a nonzero exit / SIGSEGV on the whole run.
        if (attached != kResultOk)
        {
            std::printf ("[smoke] instance %d: editor attached() failed with %d\n",
                         i, (int) attached);
            platforms[static_cast<std::size_t> (i)]->pump (50);
            view->removed ();
            view->release ();
            return 1;
        }
        controllers.push_back (controller);
        views.push_back (view);
    }

    std::printf ("[smoke] all editors opened; soaking %d ms\n", soakMs);
    fflush (stdout);
    for (int i = 0; i < instances; ++i)
        platforms[static_cast<std::size_t> (i)]->pump (soakMs);
    std::printf ("[smoke] detaching\n");
    fflush (stdout);
    for (int i = 0; i < instances; ++i)
    {
        views[static_cast<std::size_t> (i)]->removed ();
        views[static_cast<std::size_t> (i)]->release ();
    }
    // Reopen cycles: repeatedly create + attach + soak + remove a fresh
    // editor view on the SAME component/controller (the DAW "close editor
    // window, reopen it" flow). This drives RunLoop::exit()/init() (xcb
    // disconnect/reconnect) each cycle -- the exact reopening pattern that
    // used to assert in cairo-xcb-screen.c (issue #334/#249) and is guarded
    // here on every OS.
    for (int cycle = 0; cycle < reopenCycles; ++cycle)
    {
        std::printf ("[smoke] reopen cycle %d/%d\n", cycle + 1, reopenCycles);
        fflush (stdout);
        for (int i = 0; i < instances; ++i)
        {
            IPlugView* view =
                controllers[static_cast<std::size_t> (i)]->createView (ViewType::kEditor);
            if (! view)
            {
                std::printf ("[smoke] reopen %d: createView returned null\n", i);
                return 1;
            }
            view->setFrame (host);
            const tresult attached =
                view->attached (parents[static_cast<std::size_t> (i)],
                                platforms[static_cast<std::size_t> (i)]->platformType ());
            if (attached != kResultOk)
            {
                std::printf ("[smoke] reopen %d: attached() failed with %d\n",
                             i, (int) attached);
                view->removed ();
                view->release ();
                return 1;
            }
            platforms[static_cast<std::size_t> (i)]->pump (soakMs);
            view->removed ();
            view->release ();
        }
    }

    for (int i = 0; i < instances; ++i)
    {
        FUnknownPtr<IComponent> c (controllers[static_cast<std::size_t> (i)]);
        if (c)
            c->terminate ();
    }

    // Note: the platforms (and their hosts) are intentionally leaked -- see
    // the comment at createPlatform. The plugin's .so holds static refs into
    // the host that outlive main(); freeing them here would be a use-after-
    // free at process exit.
    std::printf ("[smoke] OK\n");
    fflush (stdout);
    return 0;
}