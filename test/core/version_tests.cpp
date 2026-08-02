#include <catch2/catch_test_macros.hpp>

#include "core/version.hpp"

using namespace coax::core;

TEST_CASE("a plain release tag parses with or without its v prefix") {
    REQUIRE(parse_version("1.2.3") == Version{1, 2, 3});
    REQUIRE(parse_version("v1.2.3") == Version{1, 2, 3});
    REQUIRE(parse_version("V1.2.3") == Version{1, 2, 3});
}

TEST_CASE("omitted components read as zero") {
    REQUIRE(parse_version("v2") == Version{2, 0, 0});
    REQUIRE(parse_version("v2.5") == Version{2, 5, 0});
    REQUIRE(parse_version("v0.1.0") == Version{0, 1, 0});
}

TEST_CASE("a malformed tag parses to nothing rather than a guess") {
    REQUIRE_FALSE(parse_version("").has_value());
    REQUIRE_FALSE(parse_version("v").has_value());
    REQUIRE_FALSE(parse_version("1.").has_value());
    REQUIRE_FALSE(parse_version("1.2.").has_value());
    REQUIRE_FALSE(parse_version(".1.2").has_value());
    REQUIRE_FALSE(parse_version("1.2.3.4").has_value());
    REQUIRE_FALSE(parse_version("nightly").has_value());
    REQUIRE_FALSE(parse_version("1.2.3 ").has_value());
    REQUIRE_FALSE(parse_version("1234567").has_value());
}

TEST_CASE("pre-release tags are not treated as releases") {
    REQUIRE_FALSE(parse_version("v1.2.3-rc1").has_value());
    REQUIRE_FALSE(parse_version("v1.2.3+build7").has_value());
}

TEST_CASE("versions order by major, then minor, then patch") {
    REQUIRE(*parse_version("v1.0.0") > *parse_version("v0.9.9"));
    REQUIRE(*parse_version("v0.2.0") > *parse_version("v0.1.9"));
    REQUIRE(*parse_version("v0.1.2") > *parse_version("v0.1.1"));
    // Numeric, not lexicographic: the bug this guards against reads 10 < 9.
    REQUIRE(*parse_version("v0.10.0") > *parse_version("v0.9.0"));
}

TEST_CASE("an update is offered only for a strictly newer release") {
    REQUIRE(is_update_available("v0.2.0", "0.1.0"));
    REQUIRE_FALSE(is_update_available("v0.1.0", "0.1.0"));
    // A downgrade is never offered, which is what a re-tagged or rolled-back
    // release looks like from here.
    REQUIRE_FALSE(is_update_available("v0.0.9", "0.1.0"));
}

TEST_CASE("an unreadable version on either side offers nothing") {
    REQUIRE_FALSE(is_update_available("nightly", "0.1.0"));
    REQUIRE_FALSE(is_update_available("v9.9.9", "not-a-version"));
    REQUIRE_FALSE(is_update_available("", ""));
}
