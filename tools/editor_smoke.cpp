// editor_smoke -- cross-platform editor-open smoke test for CI.
//
// Loads a nedit.vst3 bundle through the real VST3 hosting API, creates the
// component, opens its editor on a native parent window (xcb / NSView /
// HWND), pumps the platform event loop so VSTGUI's open()/idle actually
// run, then detaches and exits. A nonzero exit (including a SIGSEGV mid-
// open, like the vtable-mismatch crash in issue #25) fails the CI run.
//
// Usage: editor_smoke <bundle.vst3> [soak-ms]
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
        std::printf ("usage: editor_smoke <bundle.vst3> [soak-ms]\n");
        return 2;
    }
    const std::string bundlePath = argv[1];
    const int soakMs = argc >= 3 ? std::atoi (argv[2]) : 500;

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
    smoke::Platform* platform = smoke::createPlatform (960, 800);
    if (! platform)
    {
        std::printf ("[smoke] platform creation failed (no X display?)\n");
        return 1;
    }
    auto* host = platform->host ();

    if (f3)
    {
        if (f3->setHostContext (host->unknownCast ()) != kResultOk)
            std::printf ("[smoke] setHostContext failed (continuing)\n");
    }

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
        std::printf ("[smoke] no audio component class\n");
        return 1;
    }

    if (component->initialize (host->unknownCast ()) != kResultOk)
    {
        std::printf ("[smoke] component initialize failed\n");
        return 1;
    }

    FUnknownPtr<IEditController> controller (component);
    if (! controller)
    {
        std::printf ("[smoke] component has no IEditController\n");
        return 1;
    }

    IPlugView* view = controller->createView (ViewType::kEditor);
    if (! view)
    {
        std::printf ("[smoke] createView(kEditor) returned null\n");
        return 1;
    }

    ViewRect vr {};
    view->getSize (&vr);
    std::printf ("[smoke] editor size %d x %d\n",
                 vr.right - vr.left, vr.bottom - vr.top);

    view->setFrame (host);

    std::printf ("[smoke] attaching editor to platform parent (%s)\n",
                 platform->platformType ());
    fflush (stdout);
    const tresult attached =
        view->attached (reinterpret_cast<void*> (platform->parentHandle ()),
                        platform->platformType ());

    // The key check: without opening the editor to actual native geometry a
    // vtable/ODR mismatch (issue #25) crashes open() before we ever get
    // here -- that shows up as a nonzero exit / SIGSEGV on the whole run.
    if (attached != kResultOk)
    {
        std::printf ("[smoke] editor attached() failed with %d\n", (int) attached);
        platform->pump (50);
        view->removed ();
        view->release ();
        return 1;
    }

    std::printf ("[smoke] editor opened OK; soaking %d ms\n", soakMs);
    fflush (stdout);
    platform->pump (soakMs);

    std::printf ("[smoke] detaching\n");
    fflush (stdout);
    view->removed ();
    view->release ();
    component->terminate ();

    // Note: the platform (and its host) is intentionally leaked -- see the
    // comment at createPlatform. The plugin's .so holds static refs into the
    // host that outlive main(); freeing it here would be a use-after-free at
    // process exit.
    std::printf ("[smoke] OK\n");
    fflush (stdout);
    return 0;
}