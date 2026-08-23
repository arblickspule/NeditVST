#pragma once

// Immutable per-block state snapshots -- the audio thread's ONLY view of
// plugin state.
//
// Threading contract:
// - publish() is called from the UI/main thread. It clones the given state
//   (the allocation happens HERE, never on the audio thread) and swaps it
//   in atomically. Rapid successive publishes coalesce naturally: readers
//   observe whatever is latest when they look, nothing in between.
// - acquire() is called from the audio thread once per block. The returned
//   shared_ptr keeps that snapshot alive for as long as it is held, so a
//   publish landing mid-block can never mutate the state under process()'s
//   feet; the block simply keeps rendering the older coherent view.
//
// No locks are taken by this class beyond the (very short) internal
// critical section of std::atomic<std::shared_ptr>; there is no allocation
// on the reader path after construction.

#include "state/PluginState.h"

#include <atomic>
#include <memory>
#include <utility>

namespace nedit::engine
{

class SnapshotProvider
{
public:
    explicit SnapshotProvider (const state::PluginState& initialState)
    {
        current_.store (std::make_shared<const state::PluginState> (initialState),
                        std::memory_order_release);
    }

    SnapshotProvider (const SnapshotProvider&) = delete;
    SnapshotProvider& operator= (const SnapshotProvider&) = delete;
    SnapshotProvider (SnapshotProvider&&) = delete;
    SnapshotProvider& operator= (SnapshotProvider&&) = delete;

    // UI/main thread: replace the published state with a clone of `next`.
    // Allocates (a PluginState copy) -- UI thread only.
    void publish (const state::PluginState& next)
    {
        current_.store (std::make_shared<const state::PluginState> (next),
                        std::memory_order_release);
    }

    // Audio thread: the coherent state view for one block. Never null.
    [[nodiscard]] std::shared_ptr<const state::PluginState> acquire() const noexcept
    {
        return current_.load (std::memory_order_acquire);
    }

private:
    std::atomic<std::shared_ptr<const state::PluginState>> current_;
};

} // namespace nedit::engine
