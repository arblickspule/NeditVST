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

TEST_CASE ("v1 chunks (no grainSpeed field) load with the default", "[serialize]")
{
    // Version 2 appended grainSpeed to the render section. A v1 chunk is
    // just the v2 serialization with the version word rewritten: the
    // reader must skip the trailing field and keep the default.
    const auto original = nedit::test::makeFullyMutatedState();
    CHECK (original.render.grainSpeed == 3.5f);

    auto bytes = serialize (original);

    // bytes[4..8) is the version word (magic first).
    REQUIRE (bytes.size() >= 8);
    REQUIRE (bytes[0] == 'N');
    REQUIRE (bytes[1] == 'E');
    REQUIRE (bytes[2] == 'D');
    REQUIRE (bytes[3] == 'T');
    bytes[4] = 1;
    bytes[5] = 0;
    bytes[6] = 0;
    bytes[7] = 0;

    const auto restored = deserialize (bytes);
    REQUIRE (restored.has_value());
    CHECK (restored->render.grainSpeed == RenderState::kMinGrainSpeed);
    CHECK (restored->render.grainSizeMs == original.render.grainSizeMs);
}
