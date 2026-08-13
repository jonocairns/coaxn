#include <catch2/catch_test_macros.hpp>

#include "core/playback_power.hpp"

using namespace coax::core;

TEST_CASE("active playback intent keeps both the system and display awake") {
    for (const bool terminal_failure : {false, true}) {
        const PlaybackPowerContext context{
            .session_active = true,
            .terminal_failure = terminal_failure,
            .terminal_failure_grace_active = terminal_failure,
        };
        CHECK(decide_playback_power_mode(context) ==
              PlaybackPowerMode::DisplayAndSystemRequired);
    }
}

TEST_CASE("buffering and recovery do not enter the power decision") {
    // Their absence is the contract: a selected, unpaused session keeps its
    // request regardless of frame movement or mpv's cache-pause state.
    const PlaybackPowerContext context{.session_active = true};
    CHECK(decide_playback_power_mode(context) ==
          PlaybackPowerMode::DisplayAndSystemRequired);
}

TEST_CASE("live TS stop releases power through Idle and a fresh start reacquires it") {
    CHECK(decide_playback_power_mode({
              .session_active = false,
              .user_paused = false,
          }) == PlaybackPowerMode::AllowSleep);
    CHECK(decide_playback_power_mode({
              .session_active = true,
              .user_paused = false,
          }) == PlaybackPowerMode::DisplayAndSystemRequired);
}

TEST_CASE("minimized playback lets the display sleep but keeps recovery running") {
    const PlaybackPowerContext context{
        .session_active = true,
        .window_minimized = true,
    };
    CHECK(decide_playback_power_mode(context) == PlaybackPowerMode::SystemRequired);
}

TEST_CASE("explicit inactivity releases every power request") {
    CHECK(decide_playback_power_mode({}) == PlaybackPowerMode::AllowSleep);
    // user_paused remains the policy seam for a future source that can resume
    // from position. Live TS Stop reaches this result through session_active.
    CHECK(decide_playback_power_mode({.session_active = true, .user_paused = true}) ==
          PlaybackPowerMode::AllowSleep);
    CHECK(decide_playback_power_mode({
              .session_active = true,
              .terminal_failure = true,
              .terminal_failure_grace_active = false,
          }) == PlaybackPowerMode::AllowSleep);
}

TEST_CASE("terminal failure power grace is bounded") {
    CHECK(kTerminalFailurePowerGrace == seconds(30));
}
