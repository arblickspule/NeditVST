// Windows editor-smoke platform: a plain (offscreen-ok) HWND parent.
// VSTGUI's GDI backend hooking the host loop itself, so the smoke only
// needs to create a real window and pump the message queue.

#include "editor_smoke_platform.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>

namespace smoke
{

namespace
{

class WinPlatform final : public Platform
{
public:
    WinPlatform (std::int32_t w, std::int32_t h)
        : w_ (w), h_ (h)
    {
        hInst_ = GetModuleHandleW (nullptr);
        WNDCLASSEXW wc {};
        wc.cbSize = sizeof (wc);
        wc.lpfnWndProc = &WinPlatform::wndProc;
        wc.hInstance = hInst_;
        wc.lpszClassName = L"NeditEditorSmoke";
        if (! RegisterClassExW (&wc) && GetLastError () != ERROR_CLASS_ALREADY_EXISTS)
        {
            std::fprintf (stderr, "[smoke] RegisterClassExW failed: %lu\n",
                          (unsigned long) GetLastError ());
            return;
        }
        win_ = CreateWindowExW (0, wc.lpszClassName, L"Nedit Editor Smoke",
                                WS_POPUP | WS_VISIBLE,
                                0, 0, w > 0 ? w : 800, h > 0 ? h : 600,
                                nullptr, nullptr, hInst_, nullptr);
        if (win_ == nullptr)
        {
            std::fprintf (stderr, "[smoke] CreateWindowExW failed: %lu\n",
                          (unsigned long) GetLastError ());
            return;
        }
        ok_ = true;
    }
    ~WinPlatform () override
    {
        if (win_)
            DestroyWindow (win_);
    }

    bool ok () const { return ok_; }
    std::uint64_t parentHandle () const override { return (std::uint64_t) win_; }
    const char* platformType () const override { return Steinberg::kPlatformTypeHWND; }
    Steinberg::PlatformHost* host () override { return &host_; }
    std::int32_t width () const override { return w_; }
    std::int32_t height () const override { return h_; }

    void pump (int ms) override
    {
        const DWORD start = GetTickCount ();
        while (static_cast<long> (GetTickCount () - start) < ms)
        {
            MSG msg;
            while (PeekMessageW (&msg, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage (&msg);
                DispatchMessageW (&msg);
            }
            Sleep (5);
        }
    }

private:
    static LRESULT CALLBACK wndProc (HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        return DefWindowProcW (hwnd, msg, wParam, lParam);
    }

    std::int32_t w_ = 0, h_ = 0;
    HINSTANCE hInst_ = nullptr;
    HWND win_ = nullptr;
    Steinberg::PlatformHost host_;
    bool ok_ = false;
};

} // namespace

Platform* createPlatform (std::int32_t width, std::int32_t height)
{
    auto* p = new WinPlatform (width, height);
    if (! p->ok ())
    {
        delete p;
        return nullptr;
    }
    return p;
}

} // namespace smoke