// Cross-platform editor-open smoke test: host an engine-free VST3 session,
// create the editor view and attach it to a real native parent window, then
// verify it completes open()/idle without crashing. Used in CI on Linux
// (xcb + IRunLoop), macOS (NSView) and Windows (HWND).

#ifndef NEDIT_EDITOR_SMOKE_PLATFORM_H
#define NEDIT_EDITOR_SMOKE_PLATFORM_H

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/vsttypes.h"

#include <cstdint>
#include <cstring>

// Shared host object: implements IHostApplication + IPlugFrame + Linux::IRunLoop
// with manual FUnknown (identical across every platform).
namespace Steinberg
{
class PlatformHost : public Vst::IHostApplication,
                           public IPlugFrame,
                           public Linux::IRunLoop
{
public:
    tresult PLUGIN_API queryInterface (const TUID iid, void** obj) override
    {
        if (FUnknownPrivate::iidEqual (iid, FUnknown::iid) ||
            FUnknownPrivate::iidEqual (iid, Vst::IHostApplication::iid))
        { *obj = static_cast<Vst::IHostApplication*> (this); addRef (); return kResultOk; }
        if (FUnknownPrivate::iidEqual (iid, IPlugFrame::iid))
        { *obj = static_cast<IPlugFrame*> (this); addRef (); return kResultOk; }
        if (FUnknownPrivate::iidEqual (iid, Linux::IRunLoop::iid))
        { *obj = static_cast<Linux::IRunLoop*> (this); addRef (); return kResultOk; }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef () override { return 1000; }
    uint32 PLUGIN_API release () override { return 1000; }

    tresult PLUGIN_API getName (Vst::String128 name) override
    {
        static const char16_t n[] = u"NeditSmoke";
        std::memcpy (name, n, sizeof (n));
        return kResultOk;
    }
    tresult PLUGIN_API createInstance (TUID, TUID, void**) override { return kNotImplemented; }
    tresult PLUGIN_API resizeView (IPlugView*, ViewRect*) override { return kResultTrue; }

    // Linux::IRunLoop -- no-op defaults keep PlatformHost concrete; the Linux
    // platform subclass overrides with a real poll-based dispatcher.
    tresult PLUGIN_API registerEventHandler (Linux::IEventHandler* handler, Linux::FileDescriptor fd) override
    { return kResultFalse; }
    tresult PLUGIN_API unregisterEventHandler (Linux::IEventHandler* handler) override
    { return kResultFalse; }
    tresult PLUGIN_API registerTimer (Linux::ITimerHandler* handler, Linux::TimerInterval milliseconds) override
    { return kResultFalse; }
    tresult PLUGIN_API unregisterTimer (Linux::ITimerHandler* handler) override
    { return kResultFalse; }

    // Disambiguate the triple FUnknown base (IHostApplication / IPlugFrame /
    // IRunLoop all inherit from FUnknown) -- needed for setHostContext and
    // component->initialize which take FUnknown*.
    FUnknown* unknownCast () { return static_cast<Vst::IHostApplication*> (this); }
};
} // namespace Steinberg

namespace smoke
{

// Native platform glue needed to open the VSTGUI editor outside a DAW.
struct Platform
{
    virtual ~Platform () = default;

    // Parent window handle for IPlugView::attached(). The concrete type
    // matches kPlatformType* (xcb_window_t / HWND / NSView*), stored as
    // an integer.
    virtual std::uint64_t parentHandle () const = 0;

    // String passed as the platform type to IPlugView::attached().
    virtual const char* platformType () const = 0;

    // Dummy IPlugFrame + (on Linux) IRunLoop so VSTGUI's backend can
    // register its fd/timer wakeups -- exactly what a real host supplies.
    // The driver owns one heap host and passes it to every call that needs
    // an FUnknown (setHostContext / initialize / setFrame) AND to the
    // platform, which drives its registered fds on pump().  Never freed.
    virtual Steinberg::PlatformHost* host () = 0;

    // Editor size (from view->getSize) to size the native parent.
    virtual std::int32_t width () const = 0;
    virtual std::int32_t height () const = 0;

    // Run the platform's native event loop for `ms` milliseconds so the
    // editor's timers (idle, rendering) actually fire.
    virtual void pump (int ms) = 0;
};

// Creates the platform implementation (per-OS TU). Returns nullptr on
// failure (e.g. no X display).
Platform* createPlatform (std::int32_t width, std::int32_t height);

} // namespace smoke

#endif