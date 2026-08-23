// The deserializer must never crash, hang, or produce out-of-range state,
// no matter what bytes the host hands us.

#include <catch2/catch_test_macros.hpp>

#include <state/Serialization.h>
#include <state/StreamIO.h>

#include "TestStateBuilder.h"

#include <random>

using namespace nedit::state;

TEST_CASE ("empty and null input are rejected", "[robustness]")
{
    CHECK_FALSE (deserialize (nullptr, 0).has_value());
    CHECK_FALSE (deserialize (nullptr, 100).has_value());

    const std::vector<std::uint8_t> empty;
    CHECK_FALSE (deserialize (empty).has_value());
}

TEST_CASE ("wrong magic is rejected", "[robustness]")
{
    auto bytes = serialize (PluginState {});
    bytes[0] = 'X';
    CHECK_FALSE (deserialize (bytes).has_value());
}

TEST_CASE ("future format version is rejected", "[robustness]")
{
    auto bytes = serialize (PluginState {});
    bytes[4] = 0xff;  // version LSB
    CHECK_FALSE (deserialize (bytes).has_value());
}

TEST_CASE ("truncation at every byte never crashes", "[robustness]")
{
    const auto bytes = serialize (nedit::test::makeFullyMutatedState());

    for (std::size_t len = 0; len < bytes.size(); ++len)
    {
        const auto result = deserialize (bytes.data(), len);

        // Any truncation inside the header or a section must be rejected;
        // what matters here is: no crash, and if something IS returned it
        // is sane (truncation on a section boundary yields a valid,
        // partially-default state -- that is by design).
        if (result.has_value())
        {
            auto sanitized = *result;
            sanitized.sanitize();
            CHECK (sanitized == *result);
        }
    }
}

TEST_CASE ("random garbage never crashes", "[robustness]")
{
    std::mt19937 rng (12345);
    std::uniform_int_distribution<int> byteDist (0, 255);
    std::uniform_int_distribution<std::size_t> lenDist (0, 4096);

    for (int i = 0; i < 200; ++i)
    {
        std::vector<std::uint8_t> garbage (lenDist (rng));
        for (auto& b : garbage)
            b = static_cast<std::uint8_t> (byteDist (rng));

        (void) deserialize (garbage);  // must simply not crash
    }
}

TEST_CASE ("bit-flipped real chunks never crash and never yield out-of-range state", "[robustness]")
{
    const auto bytes = serialize (nedit::test::makeFullyMutatedState());
    std::mt19937 rng (67890);
    std::uniform_int_distribution<std::size_t> posDist (0, bytes.size() - 1);
    std::uniform_int_distribution<int> bitDist (0, 7);

    for (int i = 0; i < 500; ++i)
    {
        auto corrupted = bytes;
        corrupted[posDist (rng)] ^= static_cast<std::uint8_t> (1 << bitDist (rng));

        const auto result = deserialize (corrupted);

        if (result.has_value())
        {
            // Whatever came back must already be sanitized.
            auto sanitized = *result;
            sanitized.sanitize();
            CHECK (sanitized == *result);
        }
    }
}

TEST_CASE ("unknown sections are skipped (forward compatibility)", "[robustness]")
{
    const auto original = nedit::test::makeFullyMutatedState();
    auto bytes = serialize (original);

    // Append a section tag this build does not know about.
    StreamWriter extra;
    extra.writeU32 (0x5A5A5A5Au);  // unknown tag
    extra.writeU32 (12);           // payload size
    for (int i = 0; i < 12; ++i)
        extra.writeU8 (0xEE);

    bytes.insert (bytes.end(), extra.data().begin(), extra.data().end());

    const auto restored = deserialize (bytes);
    REQUIRE (restored.has_value());
    CHECK (*restored == original);
}

TEST_CASE ("missing sections keep defaults (backward compatibility)", "[robustness]")
{
    // A minimal chunk: header only, no sections at all -- as an older
    // (or newer, differently-sectioned) writer might produce.
    StreamWriter out;
    out.writeU32 (0x5444454Eu);  // 'NEDT' little-endian
    out.writeU32 (1);            // version

    const auto restored = deserialize (out.data());
    REQUIRE (restored.has_value());
    CHECK (*restored == PluginState {});
}
