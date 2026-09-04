// Binary serialization round-trips.

#include <catch2/catch_test_macros.hpp>

#include <state/Serialization.h>
#include <state/StreamIO.h>

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

TEST_CASE ("v2 chunks (scalar volume) load with per-style volume defaulted", "[serialize]")
{
    // v3 replaced the scalar Volume/Volume Mode params (ids 19/20) with a
    // per-style volume array. A genuine v2 chunk carries the scalar pair in
    // its styleParams block and NO per-style array; the reader must drop
    // the scalar volume (per-style defaults to 1.0) without desyncing the
    // section, while v2 fields like render.grainSpeed are preserved.
    using namespace nedit::state;

    const auto fourcc = [] (char a, char b, char c, char d) {
        return static_cast<std::uint32_t> (static_cast<std::uint8_t> (a))
             | (static_cast<std::uint32_t> (static_cast<std::uint8_t> (b)) << 8)
             | (static_cast<std::uint32_t> (static_cast<std::uint8_t> (c)) << 16)
             | (static_cast<std::uint32_t> (static_cast<std::uint8_t> (d)) << 24);
    };

    const auto kTagRender   = fourcc ('R', 'N', 'D', 'R');
    const auto kTagGenerate = fourcc ('G', 'N', 'R', 'T');

    auto beginSection = [] (StreamWriter& out, std::uint32_t tag, std::size_t& sizeOff) {
        out.writeU32 (tag);
        sizeOff = out.size();
        out.writeU32 (0);
    };
    auto endSection = [] (StreamWriter& out, std::size_t sizeOff, std::size_t payloadStart) {
        out.patchU32 (sizeOff, static_cast<std::uint32_t> (out.size() - payloadStart));
    };

    StreamWriter out;
    out.writeU32 (fourcc ('N', 'E', 'D', 'T'));
    out.writeU32 (2);   // v2

    // Render section (v2 format: includes grainSpeed).
    {
        std::size_t sizeOff = 0;
        beginSection (out, kTagRender, sizeOff);
        const auto payloadStart = out.size();
        out.writeF32 (5.0f);    // fadeInMs
        out.writeF32 (10.0f);   // fadeOutMs
        out.writeU8 (0);        // pitchMode repitch
        out.writeF32 (15.0f);   // grainSizeMs
        out.writeF32 (3.5f);    // grainSpeed (v2)
        out.writeU8 (0);        // grainWindowShape hann
        out.writeF32 (0.0f);    // pitchShiftSemitones
        out.writeBool (false);  // beatQuantizeTimeStretch
        out.writeBool (false);  // beatQuantizeRepitch
        endSection (out, sizeOff, payloadStart);
    }

    // Generate section (v2 format: styleParams carry 21 scalar params,
    // scalar Volume at id 19 = 0.4, Volume Mode at id 20 = 1; no volume
    // array).
    {
        std::size_t sizeOff = 0;
        beginSection (out, kTagGenerate, sizeOff);
        const auto payloadStart = out.size();
        out.writeU8 (0);        // generateMode sliceLength
        out.writeU32 (0);       // sliceWeights count
        for (int i = 0; i < kNumPlaybackStyles; ++i)
            out.writeF32 (1.0f);   // styleWeights

        out.writeU32 (21);      // styleParams count (v2 vocabulary)
        for (int i = 0; i < 21; ++i)
        {
            // id 4 = grainSpeed (exercises the shared-field read); id 19 =
            // scalar Volume, id 20 = Volume Mode -- both now dropped.
            if (i == 4)
                out.writeF32 (2.0f);
            else if (i == 19)
                out.writeF32 (0.4f);
            else if (i == 20)
                out.writeF32 (1.0f);
            else
                out.writeF32 (0.0f);
        }

        out.writeI32 (2);       // resetBarsIndex
        out.writeI32 (10);      // clockReferenceIndex
        for (int i = 0; i < kNumNoteValues; ++i)
            out.writeF32 (0.0f);   // subdivisionWeights
        out.writeU8 (0);        // tapeStopScope
        out.writeU8 (1);        // filterSweepScope
        endSection (out, sizeOff, payloadStart);
    }

    auto bytes = out.take();
    REQUIRE (bytes.size() >= 8);

    const auto restored = deserialize (bytes.data(), bytes.size());
    REQUIRE (restored.has_value());

    // v2 fields survive...
    CHECK (restored->render.grainSpeed == 3.5f);
    CHECK (restored->generate.styleParams.grainSpeed == 2.0f);

    // ...while the scalar volume is dropped: every style volume defaults to 1.0.
    for (const auto v : restored->generate.styleParams.styleVolume)
        CHECK (v == 1.0f);
}

TEST_CASE ("v3 chunks (no sequencer viewport) load with viewport defaulted", "[serialize]")
{
    // v3 wrote the sequencer section WITHOUT the trailing grid viewport the
    // v4 writer appends. Rewriting the version word of a v4 chunk to 3 must
    // reproduce that: the section parses, the trailing doubles are skipped,
    // and the viewport keeps its defaults.
    using namespace nedit::state;

    PluginState original;
    original.sequencer.viewport = { 2.5, 0.5, 0.75, 0.25 };
    original.sample.samplePath = "/tmp/old-session.wav";

    auto bytes = serialize (original);
    REQUIRE (bytes.size() >= 12);

    // bytes[4..8) is the format version u32 right after the magic.
    const std::uint32_t v3 = 3;
    for (std::size_t i = 0; i < 4; ++i)
        bytes[4 + i] = static_cast<std::uint8_t> ((v3 >> (8 * i)) & 0xff);

    const auto restored = deserialize (bytes);
    REQUIRE (restored.has_value());

    CHECK (restored->sample.samplePath == original.sample.samplePath);
    CHECK (restored->sequencer.viewport.zoomX == 1.0);
    CHECK (restored->sequencer.viewport.zoomY == 1.0);
    CHECK (restored->sequencer.viewport.originX == 0.0);
    CHECK (restored->sequencer.viewport.originY == 1.0);
}
