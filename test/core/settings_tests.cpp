#include <catch2/catch_test_macros.hpp>

#include "core/settings.hpp"

using namespace coax::core;

TEST_CASE("settings survive a round trip") {
    Settings written;
    written.minimal_mode = false;
    written.volume       = 73;
    const auto read = parse(serialize(written));
    REQUIRE(read.minimal_mode == false);
    REQUIRE(read.volume == 73);

    written.minimal_mode = true;
    written.volume       = 0;
    const auto silent = parse(serialize(written));
    REQUIRE(silent.minimal_mode == true);
    REQUIRE(silent.volume == 0);
}

TEST_CASE("volume is clamped to the range the player accepts") {
    // Clamped rather than rejected: a file naming a volume off the end is
    // stating an intention, and the nearest honourable answer beats silently
    // restoring something unrelated.
    REQUIRE(parse("volume=99999").volume == kMaxVolume);
    REQUIRE(parse("volume=0").volume == 0);
    REQUIRE(parse("volume=130").volume == kMaxVolume);
}

TEST_CASE("a volume that is not a number leaves the field alone") {
    // Including the forms that look numeric. A negative or fractional reading
    // is not a volume out of range, it is a file this build cannot read.
    REQUIRE(parse("volume=loud").volume == Settings{}.volume);
    REQUIRE(parse("volume=-40").volume == Settings{}.volume);
    REQUIRE(parse("volume=40.5").volume == Settings{}.volume);
    REQUIRE(parse("volume=").volume == Settings{}.volume);
}

TEST_CASE("one damaged setting does not take the other with it") {
    const auto settings = parse("minimal_mode=false\nvolume=nonsense\n");
    REQUIRE(settings.minimal_mode == false);
    REQUIRE(settings.volume == Settings{}.volume);
}

TEST_CASE("an absent key keeps its default") {
    // The whole point of the format: a file written before a setting existed
    // still produces a usable application.
    REQUIRE(parse("").minimal_mode == Settings{}.minimal_mode);
    REQUIRE(parse("something_else=false").minimal_mode == Settings{}.minimal_mode);
}

TEST_CASE("a key this build does not know is ignored") {
    // What a downgrade reads. The settings it does understand have to survive.
    const auto settings = parse("future_setting=42\nminimal_mode=false\n");
    REQUIRE(settings.minimal_mode == false);
}

TEST_CASE("a malformed line costs one setting rather than the file") {
    const auto settings = parse("this line has no separator\nminimal_mode=false\n");
    REQUIRE(settings.minimal_mode == false);
}

TEST_CASE("a value that is not a boolean leaves the field alone") {
    // Distinct from absent: the key was recognised and the value was not, so the
    // default stands rather than the field flipping to false.
    REQUIRE(parse("minimal_mode=perhaps").minimal_mode == Settings{}.minimal_mode);
    REQUIRE(parse("minimal_mode=").minimal_mode == Settings{}.minimal_mode);
}

TEST_CASE("both boolean spellings are accepted") {
    REQUIRE(parse("minimal_mode=1").minimal_mode == true);
    REQUIRE(parse("minimal_mode=0").minimal_mode == false);
    REQUIRE(parse("minimal_mode=true").minimal_mode == true);
    REQUIRE(parse("minimal_mode=false").minimal_mode == false);
}

TEST_CASE("whitespace, comments and Windows line endings are tolerated") {
    // The file is meant to be editable by hand, which is where all three come
    // from. The carriage return is the one that matters: it arrives on the
    // value, where it would otherwise make every boolean unreadable.
    const auto settings = parse("# a comment\r\n\r\n  minimal_mode  =  false  \r\n");
    REQUIRE(settings.minimal_mode == false);
}

TEST_CASE("a final line without a newline is still read") {
    REQUIRE(parse("minimal_mode=false").minimal_mode == false);
}

TEST_CASE("the last assignment of a key wins") {
    // Not a case anything writes, but a hand-edited file can hold it and the
    // answer should be the one nearest the bottom rather than undefined.
    REQUIRE(parse("minimal_mode=true\nminimal_mode=false\n").minimal_mode == false);
}
