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

TEST_CASE ("note value palette partitions into plain / dotted / triplet groups", "[types]")
{
    // The "n=0 / nd=0 / nt=0" quick-clears zero whole variants at once, so
    // the group membership table must mirror the palette's name suffixes:
    //   plain   {128n, 64n, 32n, 16n, 8n, 4n, 2n, 1n}         -> 8
    //   dotted  {64nd, 32nd, 16nd, 8nd, 4nd, 2nd}             -> 6
    //   triplet {32nt, 16nt, 8nt, 4nt, 2nt, 1nt}              -> 6
    // Any reordering of the %d/%n name suffixes in the palette must trip
    // this check (name suffix always matches the declared variant).
    const NoteValueVariant expected[kNumNoteValues] {
        NoteValueVariant::plain,    // 0:  128n
        NoteValueVariant::plain,    // 1:  64n
        NoteValueVariant::triplet,  // 2:  32nt
        NoteValueVariant::dotted,   // 3:  64nd
        NoteValueVariant::plain,    // 4:  32n
        NoteValueVariant::triplet,  // 5:  16nt
        NoteValueVariant::dotted,   // 6:  32nd
        NoteValueVariant::plain,    // 7:  16n
        NoteValueVariant::triplet,  // 8:  8nt
        NoteValueVariant::dotted,   // 9:  16nd
        NoteValueVariant::plain,    // 10: 8n
        NoteValueVariant::triplet,  // 11: 4nt
        NoteValueVariant::dotted,   // 12: 8nd
        NoteValueVariant::plain,    // 13: 4n
        NoteValueVariant::triplet,  // 14: 2nt
        NoteValueVariant::dotted,   // 15: 4nd
        NoteValueVariant::plain,    // 16: 2n
        NoteValueVariant::triplet,  // 17: 1nt
        NoteValueVariant::dotted,   // 18: 2nd
        NoteValueVariant::plain     // 19: 1n
    };

    std::array<int, 3> counts {};
    for (int i = 0; i < kNumNoteValues; ++i)
    {
        const auto v = kNoteValueVariant[static_cast<std::size_t> (i)];
        CHECK (v == expected[static_cast<std::size_t> (i)]);
        CHECK (std::string (kNoteValues[static_cast<std::size_t> (i)].name).substr (
                   std::string (kNoteValues[static_cast<std::size_t> (i)].name).size() - 1)
               == (v == NoteValueVariant::plain ? "n"
                      : v == NoteValueVariant::dotted ? "d" : "t"));
        counts[static_cast<std::size_t> (v)] += 1;
    }
    CHECK (counts[static_cast<std::size_t> (NoteValueVariant::plain)] == 8);
    CHECK (counts[static_cast<std::size_t> (NoteValueVariant::dotted)] == 6);
    CHECK (counts[static_cast<std::size_t> (NoteValueVariant::triplet)] == 6);
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
