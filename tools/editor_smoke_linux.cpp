// Linux editor-smoke platform: an xcb parent window plus an IRunLoop that
// actually services VSTGUI's registered file descriptors and timers (no
// separate X11 event loop -- VSTGUI's xcb backend drives the window).

#include "editor_smoke_platform.h"

#include <xcb/xcb.h>

#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

#include <poll.h>
#include <unistd.h>

using namespace Steinberg;

namespace smoke
{

namespace
{

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#endif

// IRunLoop implementation backed by a real poll() loop. The CFrame's xcb
// window registers its file descriptor + timers here; pump() waits on all
// of them together so editor idle/redraw actually happen.
class LinuxHost final : public PlatformHost
{
public:
    tresult PLUGIN_API registerEventHandler (Linux::IEventHandler* h,
                                             Linux::FileDescriptor fd) override
    {
        for (const auto& e : events_)
            if (e.first == h)
                return kResultFalse;
        events_.emplace_back (h, fd);
        return kResultOk;
    }
    tresult PLUGIN_API unregisterEventHandler (Linux::IEventHandler* h) override
    {
        for (auto it = events_.begin (); it != events_.end (); ++it)
        {
            if (it->first == h)
            {
                events_.erase (it);
                return kResultOk;
            }
        }
        return kResultFalse;
    }
    tresult PLUGIN_API registerTimer (Linux::ITimerHandler* h,
                                      Linux::TimerInterval ms) override
    {
        for (const auto& t : timers_)
            if (t.first == h)
                return kResultFalse;
        timers_.emplace_back (h, ms);
        return kResultOk;
    }
    tresult PLUGIN_API unregisterTimer (Linux::ITimerHandler* h) override
    {
        for (auto it = timers_.begin (); it != timers_.end (); ++it)
        {
            if (it->first == h)
            {
                timers_.erase (it);
                return kResultOk;
            }
        }
        return kResultFalse;
    }

    // Poll the VSTGUI xcb fd + any plugin-registered fds; fire due timers.
    void pump (int ms)
    {
        const int xfd = conn_ ? xcb_get_file_descriptor (conn_) : -1;
        std::vector<pollfd> pf (events_.size () + (xfd >= 0 ? 1 : 0));
        std::size_t n = 0;
        if (xfd >= 0)
        {
            pf[n].fd = xfd;
            pf[n].events = POLLIN;
            ++n;
        }
        for (std::size_t i = 0; i < events_.size (); ++i)
        {
            pf[n + i].fd = events_[i].second;
            pf[n + i].events = POLLIN;
        }
        if (::poll (pf.data (), pf.size (), ms) < 0)
            return;

        // VSTGUI's xcb window wants its events drained each cycle.
        if (xfd >= 0 && (pf[0].revents & POLLIN))
        {
            xcb_generic_event_t* ev;
            while ((ev = xcb_poll_for_event (conn_)))
                std::free (ev);
        }
        const std::size_t evBase = xfd >= 0 ? 1 : 0;
        for (std::size_t i = 0; i < events_.size (); ++i)
            if (pf[evBase + i].revents & POLLIN)
                events_[i].first->onFDIsSet (events_[i].second);
        for (const auto& t : timers_)
            t.first->onTimer ();
    }

    xcb_connection_t* conn_ = nullptr;
    std::vector<std::pair<Linux::IEventHandler*, Linux::FileDescriptor>> events_;
    std::vector<std::pair<Linux::ITimerHandler*, int>> timers_;
};

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

class XcbPlatform final : public Platform
{
public:
    XcbPlatform (std::int32_t w, std::int32_t h) : w_ (w), h_ (h)
    {
        conn_ = xcb_connect (nullptr, nullptr);
        if (conn_ == nullptr || xcb_connection_has_error (conn_))
        {
            std::fprintf (stderr, "[smoke] xcb_connect failed (headless?)\n");
            conn_ = nullptr;
            return;
        }
        const xcb_setup_t* setup = xcb_get_setup (conn_);
        screen_ = xcb_setup_roots_iterator (setup).data;
        win_ = xcb_generate_id (conn_);
        const uint16_t uw = (uint16_t) w;
        const uint16_t uh = (uint16_t) h;
        uint32_t vals[1] = { XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY };
        xcb_create_window (conn_, XCB_COPY_FROM_PARENT, win_, screen_->root,
                           0, 0, uw, uh, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                           screen_->root_visual, XCB_CW_EVENT_MASK, vals);
        xcb_map_window (conn_, win_);
        xcb_flush (conn_);
        host_.conn_ = conn_;
        ok_ = true;
    }
    ~XcbPlatform () override
    {
        if (conn_)
        {
            xcb_destroy_window (conn_, win_);
            xcb_disconnect (conn_);
        }
    }

    bool ok () const { return ok_; }
    std::uint64_t parentHandle () const override { return (std::uint64_t) win_; }
    const char* platformType () const override { return kPlatformTypeX11EmbedWindowID; }
    PlatformHost* host () override { return &host_; }
    std::int32_t width () const override { return w_; }
    std::int32_t height () const override { return h_; }
    void pump (int ms) override { host_.pump (ms); }

private:
    std::int32_t w_ = 0, h_ = 0;
    xcb_connection_t* conn_ = nullptr;
    xcb_screen_t* screen_ = nullptr;
    xcb_window_t win_ = 0;
    LinuxHost host_;
    bool ok_ = false;
};

} // namespace

Platform* createPlatform (std::int32_t width, std::int32_t height)
{
    auto* p = new XcbPlatform (width, height);
    if (! p->ok ())
    {
        delete p;
        return nullptr;
    }
    return p;
}

} // namespace smoke