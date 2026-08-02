#include <catch2/catch_test_macros.hpp>

#include "core/playback_types.hpp"

using namespace coax::core;

TEST_CASE("generation decisions accept only the latest request") {
    CHECK(decide_generation(Generation{31}, Generation{30}) == GenerationDecision::Stale);
    CHECK(decide_generation(Generation{31}, Generation{31}) == GenerationDecision::Current);
    CHECK(decide_generation(Generation{31}, Generation{32}) == GenerationDecision::Future);
    CHECK_FALSE(should_apply_generation(Generation{31}, Generation{30}));
    CHECK(should_apply_generation(Generation{31}, Generation{31}));
    CHECK(should_apply_generation(Generation{31}, Generation{32}));
}
