#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <optional>

#include "player/live_sync.hpp"
#include "player/live_sync_gate.hpp"

using namespace coax;
using Catch::Approx;

// The control law. Constants come from LiveSyncConfig's ExoPlayer-derived
// defaults, so the numbers below are the shipped policy rather than a fixture.

TEST_CASE("the first update controls immediately and later ones are rate limited") {
    player::LiveSync sync;

    // 0.5s above the 4s target asks for 1.05, which the range caps at 1.03.
    CHECK(sync.update(4.5, 100.0) == Approx(1.03));

    // Inside the one-second interval nothing is written, even though the error
    // has changed enough to compute a different speed.
    CHECK_FALSE(sync.update(4.2, 100.9));
    CHECK(sync.speed() == Approx(1.03));

    CHECK(sync.update(4.2, 101.0) == Approx(1.02));
}

TEST_CASE("speed is clamped to the configured range at both ends") {
    player::LiveSync fast;
    CHECK(fast.update(100.0, 0.0) == Approx(1.03));

    player::LiveSync slow;
    CHECK(slow.update(0.0, 0.0) == Approx(0.97));
}

TEST_CASE("the deadband holds unity within 20ms of the target") {
    player::LiveSync sync;

    // 10ms of error is noise, so the controller does not chase it.
    CHECK_FALSE(sync.update(4.010, 0.0));
    CHECK(sync.speed() == Approx(1.0));

    // Just outside the band it does act, however small the correction.
    CHECK(sync.update(4.030, 1.0) == Approx(1.003));

    // And returning inside the band takes the correction back off.
    CHECK(sync.update(4.010, 2.0) == Approx(1.0));
}

TEST_CASE("a rebuffer concedes 500ms and recomputes without waiting out the interval") {
    player::LiveSync sync;
    CHECK_FALSE(sync.update(4.0, 0.0));
    CHECK(sync.target_offset_seconds() == Approx(4.0));

    sync.notify_rebuffer();
    CHECK(sync.target_offset_seconds() == Approx(4.5));

    // 0.1s later, well inside the rate limit: the target moved, so the speed
    // that was computed against the old one is stale.
    CHECK(sync.update(4.0, 0.1) == Approx(0.97));
}

TEST_CASE("the target is bounded however many rebuffers arrive") {
    player::LiveSync sync;
    for (int i = 0; i < 200; ++i) sync.notify_rebuffer();
    CHECK(sync.target_offset_seconds() == Approx(30.0));
}

TEST_CASE("reset returns the target, the speed and the rate limiter to their initial state") {
    player::LiveSync sync;
    CHECK(sync.update(10.0, 0.0) == Approx(1.03));
    sync.notify_rebuffer();
    sync.notify_rebuffer();
    CHECK(sync.target_offset_seconds() == Approx(5.0));

    sync.reset();
    CHECK(sync.target_offset_seconds() == Approx(4.0));
    CHECK(sync.speed() == Approx(1.0));

    // Learned latency is not carried across channels, and neither is the
    // interval: the next channel's first sample controls immediately.
    CHECK(sync.update(4.5, 0.1) == Approx(1.03));
}

TEST_CASE("holding unity takes an installed correction off exactly once") {
    player::LiveSync sync;
    CHECK(sync.update(0.0, 0.0) == Approx(0.97));

    CHECK(sync.hold_unity_speed() == Approx(1.0));
    CHECK(sync.speed() == Approx(1.0));

    // Nothing further to write while it stays held.
    CHECK_FALSE(sync.hold_unity_speed());
    CHECK_FALSE(sync.hold_unity_speed());
}

TEST_CASE("a correction is reissued after a hold, not suppressed as unchanged") {
    player::LiveSync sync;
    CHECK(sync.update(0.0, 0.0) == Approx(0.97));
    CHECK(sync.hold_unity_speed() == Approx(1.0));

    // The same conditions compute the same 0.97 as before the hold. It has to
    // be written again: playback is at 1.0 now, so treating it as an unchanged
    // speed would leave the controller's record and mpv's disagreeing forever.
    CHECK(sync.update(0.0, 1.0) == Approx(0.97));
}

// The gate. These are the rules that decide what a turn may learn from mpv's
// cache signalling, which is where both live-sync defects lived.

namespace {

player::LiveSyncSample playing(std::optional<double> buffered) {
    return {.buffered_seconds = buffered,
            .paused_for_cache = false,
            .core_idle        = false,
            .first_frame_seen = true};
}

player::LiveSyncSample buffering(bool first_frame_seen) {
    return {.buffered_seconds = std::nullopt,
            .paused_for_cache = true,
            .core_idle        = false,
            .first_frame_seen = first_frame_seen};
}

}  // namespace

TEST_CASE("the initial fill before the first frame is not a rebuffer") {
    player::LiveSyncGate gate;

    // cache-pause-initial=yes publishes paused-for-cache for a normal opening
    // fill. Nothing has been interrupted, so nothing may be conceded.
    CHECK_FALSE(gate.observe(buffering(false)).rebuffered);
    CHECK_FALSE(gate.observe(buffering(false)).rebuffered);

    CHECK_FALSE(gate.observe(playing(4.0)).rebuffered);

    // The same state, once playback has started, is a real underrun.
    CHECK(gate.observe(buffering(true)).rebuffered);
}

TEST_CASE("a stall is counted on entry rather than once per turn") {
    player::LiveSyncGate gate;
    REQUIRE_FALSE(gate.observe(playing(4.0)).rebuffered);

    CHECK(gate.observe(buffering(true)).rebuffered);
    CHECK_FALSE(gate.observe(buffering(true)).rebuffered);
    CHECK_FALSE(gate.observe(buffering(true)).rebuffered);

    // Leaving and re-entering is a second one.
    CHECK_FALSE(gate.observe(playing(4.0)).rebuffered);
    CHECK(gate.observe(buffering(true)).rebuffered);
}

TEST_CASE("a recovery reopen concedes nothing before its own first frame") {
    player::LiveSyncGate gate;
    REQUIRE_FALSE(gate.observe(playing(4.0)).rebuffered);
    REQUIRE(gate.observe(buffering(true)).rebuffered);

    // Recovery reopens the stream: the load's first-frame flag clears, but the
    // controller and this gate deliberately keep their learned state. The
    // reopen's own fill must not be charged as a second rebuffer.
    CHECK_FALSE(gate.observe(buffering(false)).rebuffered);
    CHECK_FALSE(gate.observe(playing(1.0)).rebuffered);
    CHECK_FALSE(gate.observe(buffering(false)).rebuffered);

    // Once it is playing again, underruns count as they did before.
    CHECK_FALSE(gate.observe(playing(4.0)).rebuffered);
    CHECK(gate.observe(buffering(true)).rebuffered);
}

TEST_CASE("the controller is left alone while the cache drains or the core is idle") {
    player::LiveSyncGate gate;

    // Buffered duration is available here, and still must not be controlled
    // on: it is draining rather than mistimed.
    const auto stalled = gate.observe({.buffered_seconds = 0.5,
                                       .paused_for_cache = true,
                                       .core_idle        = false,
                                       .first_frame_seen = true});
    CHECK_FALSE(stalled.control_input);
    CHECK_FALSE(stalled.hold_unity_speed);

    const auto idle = gate.observe({.buffered_seconds = 4.0,
                                    .paused_for_cache = false,
                                    .core_idle        = true,
                                    .first_frame_seen = true});
    CHECK_FALSE(idle.control_input);
    CHECK_FALSE(idle.hold_unity_speed);
}

TEST_CASE("unavailable telemetry asks for unity instead of being read as zero") {
    player::LiveSyncGate gate;

    const auto lost = gate.observe(playing(std::nullopt));
    CHECK(lost.hold_unity_speed);
    CHECK_FALSE(lost.control_input);

    // A real zero reading is a different observation and is controlled on.
    const auto empty = gate.observe(playing(0.0));
    CHECK_FALSE(empty.hold_unity_speed);
    REQUIRE(empty.control_input);
    CHECK(*empty.control_input == Approx(0.0));

    const auto recovered = gate.observe(playing(4.2));
    CHECK_FALSE(recovered.hold_unity_speed);
    REQUIRE(recovered.control_input);
    CHECK(*recovered.control_input == Approx(4.2));
}

TEST_CASE("reset drops the previous load's pause state") {
    player::LiveSyncGate gate;
    REQUIRE_FALSE(gate.observe(playing(4.0)).rebuffered);
    REQUIRE(gate.observe(buffering(true)).rebuffered);
    REQUIRE_FALSE(gate.observe(buffering(true)).rebuffered);

    gate.reset();

    // A new channel starts from a not-paused baseline rather than inheriting
    // the state the last one happened to end in.
    CHECK(gate.observe(buffering(true)).rebuffered);
}

// Gate and controller together, over the sequences that produced the two
// defects. App::update_live_sync is these two objects and nothing else.

namespace {

// Mirrors App::update_live_sync: gate first, then whichever of the two
// controller entry points the step selected. Returns any speed to install.
std::optional<double> tick(player::LiveSyncGate& gate, player::LiveSync& sync,
                           const player::LiveSyncSample& sample, double now_seconds) {
    const auto step = gate.observe(sample);
    if (step.rebuffered) sync.notify_rebuffer();
    if (step.hold_unity_speed) return sync.hold_unity_speed();
    if (step.control_input) return sync.update(*step.control_input, now_seconds);
    return std::nullopt;
}

}  // namespace

TEST_CASE("a channel change costs no latency before its first frame") {
    player::LiveSyncGate gate;
    player::LiveSync     sync;

    // Zap: mpv pauses to fill, publishes no buffered duration yet, and has not
    // produced a frame. Previously this conceded 500ms per channel change.
    for (int turn = 0; turn < 30; ++turn) {
        CHECK_FALSE(tick(gate, sync, buffering(false), turn * 0.1));
    }
    CHECK(sync.target_offset_seconds() == Approx(4.0));
    CHECK(sync.speed() == Approx(1.0));
}

TEST_CASE("losing buffered duration holds unity instead of installing the minimum speed") {
    player::LiveSyncGate gate;
    player::LiveSync     sync;

    // Settled a little under target, so a real correction is running.
    REQUIRE(tick(gate, sync, playing(3.8), 0.0) == Approx(0.98));
    REQUIRE(sync.speed() == Approx(0.98));

    // mpv stops reporting the property. Read as 0.0 this is a -4s error, which
    // pins 0.97x and accumulates roughly 108 seconds of latency per hour.
    CHECK(tick(gate, sync, playing(std::nullopt), 1.0) == Approx(1.0));
    for (int turn = 2; turn < 60; ++turn) {
        CHECK_FALSE(tick(gate, sync, playing(std::nullopt), turn));
    }
    CHECK(sync.speed() == Approx(1.0));

    // The target is untouched by the outage: nothing rebuffered.
    CHECK(sync.target_offset_seconds() == Approx(4.0));

    // When the property comes back the controller resumes from the real value.
    CHECK(tick(gate, sync, playing(4.5), 60.0) == Approx(1.03));
}
