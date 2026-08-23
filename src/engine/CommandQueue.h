#pragma once

// Lock-free single-producer / single-consumer command queue -- the
// UI->engine side channel for requests that are NOT plugin state (state
// itself rides SnapshotProvider).
//
// Threading contract:
// - exactly ONE thread calls push() (the UI/main thread),
// - exactly ONE thread calls pop() (the audio thread or an engine worker),
// - both may spin/loop freely; neither ever blocks, allocates, or logs.
//
// When the queue is full the producer's push() returns false and the
// command is DROPPED by the caller's choice -- commands here are advisory
// pokes (regenerate caches, note a sample swap), never data that must not
// be lost. Free-running cursor arithmetic makes wraparound at 2^64 a
// non-issue.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace nedit::engine
{

struct EngineCommand
{
    enum class Type : std::uint32_t
    {
        none = 0,
        sampleSlotReplaced,  // payloadA = new sample generation counter
        invalidateAnalysis,  // transient/slice caches must be rebuilt
        quit                 // engine-side worker loops should exit
    };

    Type type = Type::none;
    std::uint32_t padding = 0;
    std::uint64_t payloadA = 0;
    std::uint64_t payloadB = 0;
};

static_assert (std::is_trivially_copyable_v<EngineCommand>,
               "commands cross threads as raw bytes");

template <std::size_t Capacity>
class SpscCommandQueue
{
    static_assert (Capacity > 0 && (Capacity & (Capacity - 1)) == 0,
                   "Capacity must be a power of two");

public:
    // Producer thread. Returns false (and pushes nothing) when full.
    [[nodiscard]] bool push (const EngineCommand& command) noexcept
    {
        const auto tail = tail_.load (std::memory_order_relaxed);
        const auto head = head_.load (std::memory_order_acquire);

        if (tail - head == Capacity)
            return false;   // full

        slots_[tail & kMask] = command;
        tail_.store (tail + 1, std::memory_order_release);
        return true;
    }

    // Consumer thread. Returns false (and writes nothing) when empty.
    [[nodiscard]] bool pop (EngineCommand& out) noexcept
    {
        const auto head = head_.load (std::memory_order_relaxed);
        const auto tail = tail_.load (std::memory_order_acquire);

        if (head == tail)
            return false;   // empty

        out = slots_[head & kMask];
        head_.store (head + 1, std::memory_order_release);
        return true;
    }

private:
    static constexpr std::size_t kMask = Capacity - 1;

    alignas (64) std::array<EngineCommand, Capacity> slots_ {};
    alignas (64) std::atomic<std::uint64_t> head_ { 0 };   // consumer cursor
    alignas (64) std::atomic<std::uint64_t> tail_ { 0 };   // producer cursor
};

} // namespace nedit::engine
