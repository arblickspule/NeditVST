// Core constant tables -- faithfulness to the original NeditVST spec.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <state/Types.h>

#include <string>

using namespace nedit::state;

TEST_CASE ("note value palette matches the original 20-entry table", "[types]")
{
    REQUIRE (kNumNoteValues == 20);

    // Endpoints and well-known defaults.
    CHECK (std::string (kNoteValues[0].name) == "128n");
    CHECK (kNoteValues[0].beats == 1.0 / 32.0);
    CHECK (std::string (kNoteValues[19].name) == "1n");
    CHECK (kNoteValues[19].beats == 4.0);

    CHECK (std::string (kNoteValues[kNoteValue16n].name) == "16n");
    CHECK (kNoteValues[kNoteValue16n].beats == 0.25);
    CHECK (std::string (kNoteValues[kNoteValue4n].name) == "4n");
    CHECK (kNoteValues[kNoteValue4n].beats == 1.0);

    // Sorted shortest to longest.
    for (int i = 1; i < kNumNoteValues; ++i)
        CHECK (kNoteValues[static_cast<std::size_t> (i)].beats
               > kNoteValues[static_cast<std::size_t> (i - 1)].beats);
}

TEST_CASE ("note value index validation", "[types]")
{
    CHECK (isValidNoteValueIndex (0));
    CHECK (isValidNoteValueIndex (19));
    CHECK_FALSE (isValidNoteValueIndex (-1));
    CHECK_FALSE (isValidNoteValueIndex (20));
}

TEST_CASE ("playback styles match the original nine", "[types]")
{
    REQUIRE (kNumPlaybackStyles == 9);

    CHECK (std::string (playbackStyleName (PlaybackStyle::forward)) == "Forward");
    CHECK (std::string (playbackStyleName (PlaybackStyle::pingPong)) == "Ping-Pong");
    CHECK (std::string (playbackStyleName (PlaybackStyle::tapeStop)) == "Tape Stop");
    CHECK (std::string (playbackStyleName (PlaybackStyle::stretch)) == "Stretch");
    CHECK (std::string (playbackStyleName (PlaybackStyle::filterDown)) == "Filter Down");
    CHECK (std::string (playbackStyleName (PlaybackStyle::filterUp)) == "Filter Up");
    CHECK (std::string (playbackStyleName (PlaybackStyle::bitcrush)) == "Bitcrush");
    CHECK (std::string (playbackStyleName (PlaybackStyle::scratch)) == "Scratch");
    CHECK (std::string (playbackStyleName (PlaybackStyle::flanger)) == "Flanger");
}

TEST_CASE ("bar-count tables match the original", "[types]")
{
    CHECK (kResetBarsValues == std::array<int, 4> { 1, 2, 4, 8 });
    CHECK (kResetBarsValues[static_cast<std::size_t> (kDefaultResetBarsIndex)] == 4);

    CHECK (kPatternLengthBarsValues == std::array<int, 3> { 1, 2, 4 });
    CHECK (kPatternLengthBarsValues[static_cast<std::size_t> (kDefaultPatternLengthBarsIndex)] == 1);
}
