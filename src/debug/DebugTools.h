// Nedit -- debug/profiling utilities.
//
// Everything in this header compiles to NOTHING unless NEDIT_DEBUG_TOOLS
// is defined (controlled by the CMake option of the same name), so debug
// instrumentation can be left in place in the codebase and excluded from
// release builds with zero cost.
//
// Rules of engagement (learned the hard way in the original codebase,
// where DBG/console I/O on the audio thread while holding a lock froze
// the whole DAW):
//   * NEVER log, allocate, or take locks on the audio thread.
//   * Audio-thread diagnostics must go through lock-free mailboxes/
//     counters that a UI/background thread drains (see Mailbox below).
//   * ScopedTimer is for message-thread / test profiling only.

#pragma once

#if NEDIT_DEBUG_TOOLS

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>

namespace nedit::debug {

// Message-thread / test-code profiling helper. Prints on destruction.
class ScopedTimer
{
public:
    explicit ScopedTimer (const char* label)
        : name (label), start (std::chrono::steady_clock::now()) {}

    ~ScopedTimer()
    {
        const auto elapsed = std::chrono::steady_clock::now() - start;
        const auto us = std::chrono::duration_cast<std::chrono::microseconds> (elapsed).count();
        std::fprintf (stderr, "[nedit timer] %s: %lld us\n", name,
                      static_cast<long long> (us));
    }

    ScopedTimer (const ScopedTimer&) = delete;
    ScopedTimer& operator= (const ScopedTimer&) = delete;

private:
    const char* name;
    std::chrono::steady_clock::time_point start;
};

// Lock-free single-value mailbox: audio thread stores, UI thread drains.
// The only sanctioned way to get diagnostics off the audio thread.
template <typename T>
class Mailbox
{
public:
    void post (T value) noexcept { slot.store (value, std::memory_order_relaxed); }
    [[nodiscard]] T read() const noexcept { return slot.load (std::memory_order_relaxed); }

private:
    std::atomic<T> slot {};
};

} // namespace nedit::debug

#define NEDIT_SCOPED_TIMER(label) ::nedit::debug::ScopedTimer neditScopedTimer_ (label)
#define NEDIT_LOG(...) std::fprintf (stderr, "[nedit] " __VA_ARGS__)

#else // NEDIT_DEBUG_TOOLS

#define NEDIT_SCOPED_TIMER(label) ((void) 0)
#define NEDIT_LOG(...) ((void) 0)

#endif // NEDIT_DEBUG_TOOLS
