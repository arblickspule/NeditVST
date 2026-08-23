// SnapshotProvider + SpscCommandQueue: the audio-thread state-snapshot /
// message-passing mechanism. API semantics single-threaded, coherence
// under real concurrency via producer/consumer stress threads.

#include <catch2/catch_test_macros.hpp>

#include <engine/CommandQueue.h>
#include <engine/SnapshotProvider.h>

#include <atomic>
#include <algorithm>
#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

using namespace nedit::engine;
using namespace nedit::state;

// ===========================================================================
// SnapshotProvider
// ===========================================================================

TEST_CASE ("snapshot provider: initial state visible before any publish")
{
    PluginState initial;
    initial.control.baseNote = 42;

    SnapshotProvider box (initial);

    const auto view = box.acquire();
    REQUIRE (view != nullptr);
    CHECK (view->control.baseNote == 42);
}

TEST_CASE ("snapshot provider: publish swaps in a clone, original untouched")
{
    PluginState initial;
    SnapshotProvider box (initial);

    PluginState edited;
    edited.control.baseNote = 60;
    edited.performance.focusedSlot = 7;

    box.publish (edited);

    // Mutating the caller's copy afterwards must NOT leak into published
    // views -- publish clones.
    edited.control.baseNote = 1;

    const auto view = box.acquire();
    CHECK (view->control.baseNote == 60);
    CHECK (view->performance.focusedSlot == 7);
}

TEST_CASE ("snapshot provider: concurrent publishes never regress or tear",
           "[threading]")
{
    PluginState seed;
    SnapshotProvider box (seed);

    constexpr int kGenerations = 5000;

    std::atomic<bool> producerDone { false };
    std::thread producer ([&] {
        PluginState s;
        s.performance.bank[0].populated = true;
        for (int i = 1; i <= kGenerations; ++i)
        {
            s.performance.bank[0].trimStartFrame = i;
            box.publish (s);
        }
        producerDone.store (true, std::memory_order_release);
    });

    bool everRegressed = false;
    std::int64_t lastSeen = 0;

    while (! producerDone.load (std::memory_order_acquire))
    {
        const auto view = box.acquire();   // never null
        if (view == nullptr)
        {
            everRegressed = true;   // contract: acquire() is never null
            break;
        }

        const auto value = view->performance.bank[0].trimStartFrame;
        if (value < lastSeen)
            everRegressed = true;   // modification order forbids this
        lastSeen = std::max (lastSeen, value);
    }

    producer.join();

    CHECK_FALSE (everRegressed);
    CHECK (lastSeen <= kGenerations);   // saw only genuinely published values

    // After the producer joins, one more publish is definitely visible:
    // the final generation.
    box.publish ([&] {
        PluginState s;
        s.performance.bank[0].populated = true;
        s.performance.bank[0].trimStartFrame = kGenerations;
        return s;
    }());
    CHECK (box.acquire()->performance.bank[0].trimStartFrame == kGenerations);
}

// ===========================================================================
// SpscCommandQueue
// ===========================================================================

namespace
{

[[nodiscard]] EngineCommand makeCommand (EngineCommand::Type type,
                                         std::uint64_t payloadA,
                                         std::uint64_t payloadB = 0)
{
    EngineCommand command;
    command.type = type;
    command.payloadA = payloadA;
    command.payloadB = payloadB;
    return command;
}

} // namespace

TEST_CASE ("command queue: fifo round trip preserves order and payloads")
{
    SpscCommandQueue<8> queue;

    REQUIRE (queue.push (makeCommand (EngineCommand::Type::sampleSlotReplaced, 11, 22)));
    REQUIRE (queue.push (makeCommand (EngineCommand::Type::invalidateAnalysis, 33)));
    REQUIRE (queue.push (makeCommand (EngineCommand::Type::quit, 0)));

    EngineCommand out;
    REQUIRE (queue.pop (out));
    CHECK (out.type == EngineCommand::Type::sampleSlotReplaced);
    CHECK (out.payloadA == 11);
    CHECK (out.payloadB == 22);

    REQUIRE (queue.pop (out));
    CHECK (out.type == EngineCommand::Type::invalidateAnalysis);
    CHECK (out.payloadA == 33);

    REQUIRE (queue.pop (out));
    CHECK (out.type == EngineCommand::Type::quit);

    EngineCommand extra;
    CHECK_FALSE (queue.pop (extra));   // drained
}

TEST_CASE ("command queue: full rejects without corrupting indices")
{
    SpscCommandQueue<4> queue;

    for (std::uint64_t i = 1; i <= 4; ++i)
        REQUIRE (queue.push (makeCommand (EngineCommand::Type::sampleSlotReplaced, i)));

    EngineCommand overflow;
    CHECK_FALSE (queue.push (overflow));   // full: producer drops instead

    EngineCommand out;
    REQUIRE (queue.pop (out));             // make room
    CHECK (out.payloadA == 1);

    REQUIRE (queue.push (makeCommand (EngineCommand::Type::sampleSlotReplaced, 5)));

    std::uint64_t expected = 2;
    while (queue.pop (out))
        CHECK (out.payloadA == expected++);
    CHECK (expected == 6);                 // exactly the five pushed survived
}

TEST_CASE ("command queue: spsc stress delivers exactly once, in order",
           "[threading]")
{
    SpscCommandQueue<1024> queue;

    constexpr std::uint64_t kCount = 100000;

    std::thread consumer ([&] {
        EngineCommand command;
        std::uint64_t expected = 1;
        bool corrupted = false;

        while (expected <= kCount)
        {
            while (! queue.pop (command))
                std::this_thread::yield();

            if (command.payloadA != expected)
                corrupted = true;
            ++expected;
        }

        CHECK_FALSE (corrupted);
    });

    std::thread producer ([&] {
        for (std::uint64_t i = 1; i <= kCount; ++i)
            while (! queue.push (makeCommand (
                EngineCommand::Type::sampleSlotReplaced, i)))
                std::this_thread::yield();
    });

    producer.join();
    consumer.join();
}
