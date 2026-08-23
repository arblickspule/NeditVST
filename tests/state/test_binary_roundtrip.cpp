// Binary serialization round-trips.

#include <catch2/catch_test_macros.hpp>

#include <state/Serialization.h>

#include "TestStateBuilder.h"

using namespace nedit::state;

TEST_CASE ("default state round-trips", "[serialize]")
{
    const PluginState original;

    const auto bytes = serialize (original);
    REQUIRE (! bytes.empty());

    const auto restored = deserialize (bytes);
    REQUIRE (restored.has_value());
    CHECK (*restored == original);
}

TEST_CASE ("fully mutated state round-trips exactly", "[serialize]")
{
    const auto original = nedit::test::makeFullyMutatedState();

    // Guard: the builder must survive sanitize unchanged, otherwise the
    // round-trip comparison is not meaningful.
    {
        auto sanitized = original;
        sanitized.sanitize();
        REQUIRE (sanitized == original);
    }

    const auto restored = deserialize (serialize (original));
    REQUIRE (restored.has_value());
    CHECK (*restored == original);
}

TEST_CASE ("round-trip is stable across a second pass", "[serialize]")
{
    const auto original = nedit::test::makeFullyMutatedState();

    const auto once = serialize (original);
    const auto restored = deserialize (once);
    REQUIRE (restored.has_value());

    const auto twice = serialize (*restored);
    CHECK (once == twice);
}

TEST_CASE ("unicode and special characters in the sample path survive", "[serialize]")
{
    PluginState original;
    original.sample.samplePath = "/tmp/日本語/sämple \"quoted\" \\ path.wav";

    const auto restored = deserialize (serialize (original));
    REQUIRE (restored.has_value());
    CHECK (restored->sample.samplePath == original.sample.samplePath);
}

TEST_CASE ("out-of-range values in the chunk are sanitized on load", "[serialize]")
{
    PluginState original;
    original.sample.samplePath = "/tmp/x.wav";
    original.sample.sampleLengthFrames = 44100;
    original.sample.trimEndFrame = 44100;
    original.sample.sensitivity = 0.5f;

    auto bytes = serialize (original);

    // Serialize a state that was deliberately NOT sanitized.
    PluginState hostile = original;
    hostile.sample.sensitivity = 99.0f;
    hostile.control.baseNote = 1000;
    bytes = serialize (hostile);

    const auto restored = deserialize (bytes);
    REQUIRE (restored.has_value());
    CHECK (restored->sample.sensitivity == 1.0f);
    CHECK (restored->control.baseNote == 127);
}
