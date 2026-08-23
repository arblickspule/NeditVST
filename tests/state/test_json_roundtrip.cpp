// JSON debug export/import round-trips.

#include <catch2/catch_test_macros.hpp>

#include <state/JsonIO.h>

#include "TestStateBuilder.h"

using namespace nedit::state;

TEST_CASE ("default state round-trips through JSON", "[json]")
{
    const PluginState original;

    const auto text = toJson (original);
    REQUIRE (! text.empty());

    const auto restored = fromJson (text);
    REQUIRE (restored.has_value());
    CHECK (*restored == original);
}

TEST_CASE ("fully mutated state round-trips through JSON exactly", "[json]")
{
    const auto original = nedit::test::makeFullyMutatedState();

    const auto restored = fromJson (toJson (original));
    REQUIRE (restored.has_value());
    CHECK (*restored == original);
}

TEST_CASE ("malformed JSON is rejected", "[json]")
{
    CHECK_FALSE (fromJson ("").has_value());
    CHECK_FALSE (fromJson ("{ not json").has_value());
    CHECK_FALSE (fromJson ("[1, 2, 3]").has_value());  // wrong document type
    CHECK_FALSE (fromJson ("42").has_value());
}

TEST_CASE ("missing fields keep defaults", "[json]")
{
    const auto restored = fromJson ("{}");
    REQUIRE (restored.has_value());
    CHECK (*restored == PluginState {});

    const auto partial = fromJson (R"({ "control": { "baseNote": 60 } })");
    REQUIRE (partial.has_value());
    CHECK (partial->control.baseNote == 60);
    CHECK (partial->control.gateMode == false);
}

TEST_CASE ("out-of-range JSON values are sanitized", "[json]")
{
    const auto restored = fromJson (R"({
        "sample": { "sensitivity": 42.0 },
        "control": { "baseNote": 999, "activeStyle": -5 }
    })");

    REQUIRE (restored.has_value());
    CHECK (restored->sample.sensitivity == 1.0f);
    CHECK (restored->control.baseNote == 127);
    CHECK (restored->control.activeStyle == 0);
}

TEST_CASE ("JSON export is human-oriented (named parameters)", "[json]")
{
    const auto text = toJson (nedit::test::makeFullyMutatedState());

    // Parameter maps use display names, not indices.
    CHECK (text.find ("\"Resonance\"") != std::string::npos);
    CHECK (text.find ("\"Sample Rate Reduction\"") != std::string::npos);
    CHECK (text.find ("\"samplePath\"") != std::string::npos);
}
