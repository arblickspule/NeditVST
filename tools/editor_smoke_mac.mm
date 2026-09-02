// macOS editor-smoke platform: an NSView parent inside a window owned by a
// minimal NSApplication. VSTGUI's Cocoa backend hooks the main run loop
// itself, so the smoke creates a real NSView and runs the run loop so
// editor open()/idle execute for real.

#include "editor_smoke_platform.h"

#import <AppKit/AppKit.h>

#include <cstdio>

namespace smoke
{

namespace
{

class MacPlatform final : public Platform
{
public:
    MacPlatform (std::int32_t w, std::int32_t h)
        : w_ (w), h_ (h)
    {
        // A bare host has no NSApplication unless we make one. VSTGUI's
        // cocoa backend requires a running app + a window to embed into.
        if ([NSApplication sharedApplication] == nil)
        {
            std::fprintf (stderr, "[smoke] no NSApplication\n");
            return;
        }

        NSRect content = NSMakeRect (0, 0, w > 0 ? w : 800, h > 0 ? h : 600);
        NSWindow* window = [[NSWindow alloc]
            initWithContentRect: content
                      styleMask: NSWindowStyleMaskBorderless
                        backing: NSBackingStoreBuffered
                          defer: YES];
        if (window == nil)
        {
            std::fprintf (stderr, "[smoke] NSWindow alloc failed\n");
            return;
        }
        [window setTitle: @"Nedit Editor Smoke"];
        [window setBackgroundColor: [NSColor darkGrayColor]];
        [window makeKeyAndOrderFront: nil];
        contentView_ = [window contentView];
        win_ = window;
        ok_ = true;
    }
    ~MacPlatform () override
    {
        if (win_)
            [win_ close];
    }

    bool ok () const { return ok_; }
    std::uint64_t parentHandle () const override { return (std::uint64_t) contentView_; }
    const char* platformType () const override { return Steinberg::kPlatformTypeNSView; }
    Steinberg::PlatformHost* host () override { return &host_; }
    std::int32_t width () const override { return w_; }
    std::int32_t height () const override { return h_; }

    void pump (int ms) override
    {
        NSApplication* app = [NSApplication sharedApplication];
        NSRunLoop* runLoop = [NSRunLoop mainRunLoop];
        NSDate* end =
            [NSDate dateWithTimeIntervalSinceNow: (NSTimeInterval) ms / 1000.0];
        // NSRunLoop runMode:beforeDate: drives the CFRunLoop, so VSTGUI's sources and
        // timers (idle, redraw) actually fire alongside window events.
        while ([[NSDate date] compare: end] == NSOrderedAscending)
        {
            [runLoop runMode: NSDefaultRunLoopMode
                   beforeDate: [NSDate dateWithTimeIntervalSinceNow: 0.02]];
            [app updateWindows];
        }
    }

private:
    std::int32_t w_ = 0, h_ = 0;
    NSWindow* win_ = nullptr;
    NSView* contentView_ = nullptr;
    Steinberg::PlatformHost host_;
    bool ok_ = false;
};

} // namespace

Platform* createPlatform (std::int32_t width, std::int32_t height)
{
    auto* p = new MacPlatform (width, height);
    if (! p->ok ())
    {
        delete p;
        return nullptr;
    }
    return p;
}

} // namespace smoke