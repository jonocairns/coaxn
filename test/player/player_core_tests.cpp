#include <catch2/catch_test_macros.hpp>

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "player/buffer_phase_gate.hpp"
#include "player/load_diagnostics.hpp"
#include "player/player_event_adapter.hpp"
#include "player/playback_control.hpp"
#include "player/playback_observability.hpp"
#include "player/session_target_registry.hpp"
#include "player/timeline_recovery.hpp"
#include "player/transport_log_classifier.hpp"

using namespace coax;

namespace {

player::TimelineRecoveryObservation timeline_observation(
    double at, double playback, std::optional<double> cache,
    std::optional<double> playback_movement,
    std::optional<double> cache_movement,
    bool healthy = true,
    core::LoadAttempt load_attempt = core::LoadAttempt{1}) {
    player::TimelineRecoveryObservation observation;
    observation.generation = core::Generation{1};
    observation.load_attempt = load_attempt;
    observation.observed_at = core::TimePoint{core::seconds(at)};
    observation.timeline.generation = observation.generation;
    observation.timeline.load_attempt = observation.load_attempt;
    observation.timeline.elapsed_seconds = playback_movement
        ? std::optional<double>{0.5} : std::nullopt;
    observation.timeline.playback_movement_seconds = playback_movement;
    observation.timeline.playback_deviation_seconds = playback_movement
        ? std::optional<double>{*playback_movement - 0.5} : std::nullopt;
    observation.timeline.cache_end_movement_seconds = cache_movement;
    observation.playback_time_seconds = playback;
    observation.cache_end_seconds = cache;
    observation.healthy = healthy;
    return observation;
}

}  // namespace

TEST_CASE("timeline recovery ignores a shared MPEG-TS clock reset") {
    player::TimelineRecovery recovery;
    recovery.begin_load(core::Generation{1}, core::LoadAttempt{1});
    (void)recovery.observe(timeline_observation(0.0, 100.0, 104.0,
                                                0.5, 0.5), true);

    const auto step = recovery.observe(timeline_observation(
        0.5, 76.5, 81.0, -23.5, -23.0, false), true);
    CHECK_FALSE(step.recover);
    CHECK(step.outcome == player::TimelineRecoveryOutcome::CommonClockReset);

    const auto repeated = recovery.observe(timeline_observation(
        1.0, 53.0, 57.5, -23.5, -23.5, false), true);
    CHECK_FALSE(repeated.recover);
    CHECK(repeated.outcome == player::TimelineRecoveryOutcome::CommonClockReset);

    const auto settled = recovery.observe(timeline_observation(
        1.5, 53.5, 58.0, 0.5, 0.5), true);
    CHECK_FALSE(settled.recover);
    CHECK(settled.outcome == player::TimelineRecoveryOutcome::None);
}

TEST_CASE("a shared clock reset does not arm an unrelated later rewind") {
    player::TimelineRecovery recovery;
    recovery.begin_load(core::Generation{1}, core::LoadAttempt{1});
    (void)recovery.observe(timeline_observation(
        0.0, 100.0, 104.0, 0.5, 0.5), true);

    const auto reset = recovery.observe(timeline_observation(
        0.5, 76.5, 81.0, -23.5, -23.0, false), true);
    REQUIRE(reset.outcome == player::TimelineRecoveryOutcome::CommonClockReset);

    const auto rewind = recovery.observe(timeline_observation(
        1.0, 70.0, 81.5, -6.5, 0.5, false), true);
    CHECK_FALSE(rewind.recover);
    CHECK(rewind.outcome == player::TimelineRecoveryOutcome::CandidateArmed);
}

TEST_CASE("timeline recovery ignores a cache-first split clock reset") {
    player::TimelineRecovery recovery;
    recovery.begin_load(core::Generation{1}, core::LoadAttempt{1});
    (void)recovery.observe(timeline_observation(0.0, 100.0, 104.0,
                                                0.5, 0.5), true);

    const auto cache_reset = recovery.observe(timeline_observation(
        0.5, 100.5, 81.0, 0.5, -23.0), true);
    CHECK_FALSE(cache_reset.recover);
    CHECK(cache_reset.outcome == player::TimelineRecoveryOutcome::None);

    const auto playback_reset = recovery.observe(timeline_observation(
        1.0, 77.0, 81.5, -23.5, 0.5, false), true);
    CHECK_FALSE(playback_reset.recover);
    CHECK(playback_reset.outcome == player::TimelineRecoveryOutcome::CandidateArmed);
    REQUIRE(playback_reset.baseline_live_gap_seconds);
    CHECK(*playback_reset.baseline_live_gap_seconds == 4.0);

    const auto settled = recovery.observe(timeline_observation(
        1.5, 77.5, 82.0, 0.5, 0.5), true);
    CHECK_FALSE(settled.recover);
    CHECK(settled.outcome == player::TimelineRecoveryOutcome::CandidateCleared);
}

TEST_CASE("timeline recovery ignores a playback-first split clock reset") {
    player::TimelineRecovery recovery;
    recovery.begin_load(core::Generation{1}, core::LoadAttempt{1});
    (void)recovery.observe(timeline_observation(0.0, 100.0, 104.0,
                                                0.5, 0.5), true);

    const auto playback_reset = recovery.observe(timeline_observation(
        0.5, 77.0, 104.5, -23.0, 0.5, false), true);
    CHECK_FALSE(playback_reset.recover);
    CHECK(playback_reset.outcome == player::TimelineRecoveryOutcome::CandidateArmed);

    const auto cache_reset = recovery.observe(timeline_observation(
        1.0, 77.5, 81.0, 0.5, -23.5), true);
    CHECK_FALSE(cache_reset.recover);
    CHECK(cache_reset.outcome == player::TimelineRecoveryOutcome::CandidateCleared);

    const auto settled = recovery.observe(timeline_observation(
        1.5, 78.0, 81.5, 0.5, 0.5), true);
    CHECK_FALSE(settled.recover);
    CHECK(settled.outcome == player::TimelineRecoveryOutcome::None);
}

TEST_CASE("timeline recovery confirms cache-relative lost ground on a later sample") {
    player::TimelineRecovery recovery;
    recovery.begin_load(core::Generation{1}, core::LoadAttempt{1});
    (void)recovery.observe(timeline_observation(0.0, 100.0, 104.0,
                                                0.5, 0.5), true);

    const auto armed = recovery.observe(timeline_observation(
        0.5, 95.02, 104.19, -4.98, 0.19, false), true);
    CHECK_FALSE(armed.recover);
    CHECK(armed.outcome == player::TimelineRecoveryOutcome::CandidateArmed);
    REQUIRE(armed.cache_relative_loss_seconds);
    CHECK(*armed.cache_relative_loss_seconds > 5.0);

    const auto confirmed = recovery.observe(timeline_observation(
        1.0, 95.52, 104.69, 0.5, 0.5), true);
    CHECK(confirmed.recover);
    CHECK(confirmed.outcome == player::TimelineRecoveryOutcome::Recover);
    REQUIRE(confirmed.live_gap_increase_seconds);
    CHECK(*confirmed.live_gap_increase_seconds > 5.0);
}

TEST_CASE("timeline recovery confirms a rewind with missing cache on one later sample") {
    player::TimelineRecovery recovery;
    recovery.begin_load(core::Generation{1}, core::LoadAttempt{1});
    (void)recovery.observe(timeline_observation(0.0, 100.0, 104.0,
                                                0.5, 0.5), true);

    const auto armed = recovery.observe(timeline_observation(
        0.5, 75.0, std::nullopt, -25.0, std::nullopt, false), true);
    CHECK_FALSE(armed.recover);
    CHECK(armed.outcome == player::TimelineRecoveryOutcome::CandidateArmed);

    const auto confirmed = recovery.observe(timeline_observation(
        1.0, 75.5, 84.5, 0.5, std::nullopt), true);
    CHECK(confirmed.recover);
    CHECK(confirmed.outcome == player::TimelineRecoveryOutcome::Recover);
    REQUIRE(confirmed.live_gap_increase_seconds);
    CHECK(*confirmed.live_gap_increase_seconds == 5.0);
}

TEST_CASE("consecutive material rewinds confirm when cache telemetry stays unavailable") {
    player::TimelineRecovery recovery;
    recovery.begin_load(core::Generation{1}, core::LoadAttempt{1});

    const auto armed = recovery.observe(timeline_observation(
        0.5, 94.0, std::nullopt, -6.0, std::nullopt, false), true);
    CHECK_FALSE(armed.recover);
    CHECK(armed.outcome == player::TimelineRecoveryOutcome::CandidateArmed);

    const auto confirmed = recovery.observe(timeline_observation(
        1.0, 88.0, std::nullopt, -6.0, std::nullopt, false), true);
    CHECK(confirmed.recover);
    CHECK(confirmed.outcome == player::TimelineRecoveryOutcome::Recover);
    CHECK_FALSE(confirmed.live_gap_increase_seconds);
}

TEST_CASE("contradictory live-gap evidence clears consecutive rewinds") {
    player::TimelineRecovery recovery;
    recovery.begin_load(core::Generation{1}, core::LoadAttempt{1});
    (void)recovery.observe(timeline_observation(
        0.0, 100.0, 110.0, 0.5, 0.5), true);

    const auto armed = recovery.observe(timeline_observation(
        0.5, 94.0, 100.0, -6.0, -10.0, false), true);
    REQUIRE(armed.outcome == player::TimelineRecoveryOutcome::CandidateArmed);
    REQUIRE(armed.baseline_live_gap_seconds);
    CHECK(*armed.baseline_live_gap_seconds == 10.0);

    const auto contradicted = recovery.observe(timeline_observation(
        1.0, 88.0, 96.0, -6.0, -4.0, false), true);
    CHECK_FALSE(contradicted.recover);
    CHECK(contradicted.outcome == player::TimelineRecoveryOutcome::CandidateCleared);
    REQUIRE(contradicted.live_gap_increase_seconds);
    CHECK(*contradicted.live_gap_increase_seconds == -2.0);
}

TEST_CASE("timeline recovery requires persistent live-gap loss after cache resume") {
    player::TimelineRecovery recovery;
    recovery.begin_load(core::Generation{1}, core::LoadAttempt{1});
    (void)recovery.observe(timeline_observation(0.0, 100.0, 104.0,
                                                0.5, 0.5), true);

    auto rewind = timeline_observation(
        0.5, 68.42, 74.74, -31.58, -29.26, false);
    rewind.timeline.previous_cache_paused = true;
    rewind.rebuffer_age_seconds = 0.55;
    const auto armed = recovery.observe(rewind, true);
    REQUIRE(armed.outcome == player::TimelineRecoveryOutcome::CandidateArmed);
    CHECK(armed.cache_resume_related);
    CHECK(armed.rebuffer_age_seconds == 0.55);

    const auto refill = recovery.observe(timeline_observation(
        1.0, 68.92, 83.051, 0.5, 8.311), true);
    CHECK_FALSE(refill.recover);
    CHECK(refill.outcome ==
          player::TimelineRecoveryOutcome::CandidatePersistencePending);
    REQUIRE(refill.live_gap_increase_seconds);
    CHECK(*refill.live_gap_increase_seconds > 9.0);

    const auto persisted = recovery.observe(timeline_observation(
        1.5, 69.42, 83.551, 0.5, 0.5), true);
    CHECK(persisted.recover);
    CHECK(persisted.outcome == player::TimelineRecoveryOutcome::Recover);
    REQUIRE(persisted.baseline_live_gap_seconds);
    CHECK(*persisted.baseline_live_gap_seconds == 4.0);
    REQUIRE(persisted.current_live_gap_seconds);
    CHECK(*persisted.current_live_gap_seconds > 14.0);
}

TEST_CASE("persistence gets a fresh window after missing refill telemetry") {
    player::TimelineRecovery recovery;
    recovery.begin_load(core::Generation{1}, core::LoadAttempt{1});
    (void)recovery.observe(timeline_observation(
        0.0, 100.0, 104.0, 0.5, 0.5), true);

    auto rewind = timeline_observation(
        0.5, 68.42, std::nullopt, -31.58, std::nullopt, false);
    rewind.timeline.previous_cache_paused = true;
    REQUIRE(recovery.observe(rewind, true).outcome ==
            player::TimelineRecoveryOutcome::CandidateArmed);

    const auto unavailable = recovery.observe(timeline_observation(
        1.0, 68.92, std::nullopt, 0.5, std::nullopt), true);
    REQUIRE(unavailable.outcome ==
            player::TimelineRecoveryOutcome::CandidateAwaitingTelemetry);

    const auto refill = recovery.observe(timeline_observation(
        1.6, 69.42, 83.551, 0.5, std::nullopt), true);
    REQUIRE(refill.outcome ==
            player::TimelineRecoveryOutcome::CandidatePersistencePending);

    const auto persisted = recovery.observe(timeline_observation(
        2.2, 70.02, 84.151, 0.6, 0.6), true);
    CHECK(persisted.recover);
    CHECK(persisted.outcome == player::TimelineRecoveryOutcome::Recover);
}

TEST_CASE("a transient post-resume cache refill does not reconnect") {
    player::TimelineRecovery recovery;
    recovery.begin_load(core::Generation{1}, core::LoadAttempt{1});
    (void)recovery.observe(timeline_observation(0.0, 100.0, 104.0,
                                                0.5, 0.5), true);

    auto rewind = timeline_observation(
        0.5, 68.42, 74.74, -31.58, -29.26, false);
    rewind.timeline.previous_cache_paused = true;
    REQUIRE(recovery.observe(rewind, true).outcome ==
            player::TimelineRecoveryOutcome::CandidateArmed);
    REQUIRE(recovery.observe(timeline_observation(
        1.0, 68.92, 83.051, 0.5, 8.311), true).outcome ==
            player::TimelineRecoveryOutcome::CandidatePersistencePending);

    const auto settled = recovery.observe(timeline_observation(
        1.5, 69.42, 73.42, 0.5, -9.631), true);
    CHECK_FALSE(settled.recover);
    CHECK(settled.outcome == player::TimelineRecoveryOutcome::CandidateCleared);
}

TEST_CASE("the second soak regression also waits beyond its refill sample") {
    player::TimelineRecovery recovery;
    recovery.begin_load(core::Generation{1}, core::LoadAttempt{1});
    (void)recovery.observe(timeline_observation(0.0, 100.0, 104.0,
                                                0.5, 0.5), true);

    const auto split_rebase = recovery.observe(timeline_observation(
        0.5, 100.5, 76.88, 0.5, -27.12), true);
    CHECK_FALSE(split_rebase.recover);
    CHECK(split_rebase.outcome == player::TimelineRecoveryOutcome::None);

    auto rewind = timeline_observation(
        1.0, 76.56, 80.56, -23.94, 3.68, false);
    rewind.timeline.previous_cache_paused = true;
    REQUIRE(recovery.observe(rewind, true).outcome ==
            player::TimelineRecoveryOutcome::CandidateArmed);

    const auto refill = recovery.observe(timeline_observation(
        1.5, 76.8, 90.606, 0.24, 10.046), true);
    CHECK_FALSE(refill.recover);
    CHECK(refill.outcome ==
          player::TimelineRecoveryOutcome::CandidatePersistencePending);
    REQUIRE(refill.live_gap_increase_seconds);
    CHECK(*refill.live_gap_increase_seconds > 9.8);

    const auto persisted = recovery.observe(timeline_observation(
        2.0, 77.3, 91.106, 0.5, 0.5), true);
    CHECK(persisted.recover);
    CHECK(persisted.outcome == player::TimelineRecoveryOutcome::Recover);
}

TEST_CASE("missing confirmation telemetry retains a candidate within its window") {
    player::TimelineRecovery recovery;
    recovery.begin_load(core::Generation{1}, core::LoadAttempt{1});
    (void)recovery.observe(timeline_observation(0.0, 100.0, 104.0,
                                                0.5, 0.5), true);
    REQUIRE(recovery.observe(timeline_observation(
        0.5, 75.0, std::nullopt, -25.0, std::nullopt, false), true).outcome ==
            player::TimelineRecoveryOutcome::CandidateArmed);

    const auto unavailable = recovery.observe(timeline_observation(
        1.0, 75.5, std::nullopt, 0.5, std::nullopt), true);
    CHECK_FALSE(unavailable.recover);
    CHECK(unavailable.outcome ==
          player::TimelineRecoveryOutcome::CandidateAwaitingTelemetry);

    const auto confirmed = recovery.observe(timeline_observation(
        1.5, 76.0, 85.0, 0.5, std::nullopt), true);
    CHECK(confirmed.recover);
    CHECK(confirmed.outcome == player::TimelineRecoveryOutcome::Recover);
}

TEST_CASE("timeline recovery cooldown and rolling cap stop reconnect churn") {
    player::TimelineRecovery recovery;
    recovery.begin_load(core::Generation{1}, core::LoadAttempt{1});
    (void)recovery.observe(timeline_observation(0.0, 100.0, 104.0,
                                                0.5, 0.5), true);
    const auto first_armed = recovery.observe(timeline_observation(
        0.5, 95.0, 105.0, -5.0, 1.0, false), true);
    REQUIRE(first_armed.outcome == player::TimelineRecoveryOutcome::CandidateArmed);
    REQUIRE(recovery.observe(timeline_observation(
        1.0, 95.5, 105.5, 0.5, 0.5), true).recover);
    recovery.note_recovered_first_frame(core::TimePoint{core::seconds(1.0)});

    recovery.begin_load(core::Generation{1}, core::LoadAttempt{2});
    (void)recovery.observe(timeline_observation(
        1.5, 100.0, 104.0, 0.5, 0.5, true,
        core::LoadAttempt{2}), true);
    const auto armed = recovery.observe(timeline_observation(
        2.0, 95.0, 105.0, -5.0, 1.0, false,
        core::LoadAttempt{2}), true);
    REQUIRE(armed.outcome == player::TimelineRecoveryOutcome::CandidateArmed);
    const auto cooled = recovery.observe(timeline_observation(
        2.5, 95.5, 105.5, 0.5, 0.5, true,
        core::LoadAttempt{2}), true);
    CHECK_FALSE(cooled.recover);
    CHECK(cooled.outcome == player::TimelineRecoveryOutcome::SuppressedCooldown);

    (void)recovery.observe(timeline_observation(
        31.0, 200.0, 204.0, 0.5, 0.5, true,
        core::LoadAttempt{2}), true);
    REQUIRE(recovery.observe(timeline_observation(
        31.5, 195.0, 205.0, -5.0, 1.0, false,
        core::LoadAttempt{2}), true).outcome ==
        player::TimelineRecoveryOutcome::CandidateArmed);
    const auto second = recovery.observe(timeline_observation(
        32.0, 195.5, 205.5, 0.5, 0.5, true,
        core::LoadAttempt{2}), true);
    REQUIRE(second.recover);
    recovery.note_recovered_first_frame(core::TimePoint{core::seconds(32.0)});

    (void)recovery.observe(timeline_observation(
        62.5, 300.0, 304.0, 0.5, 0.5, true,
        core::LoadAttempt{2}), true);
    REQUIRE(recovery.observe(timeline_observation(
        63.0, 295.0, 305.0, -5.0, 1.0, false,
        core::LoadAttempt{2}), true).outcome ==
        player::TimelineRecoveryOutcome::CandidateArmed);
    const auto capped = recovery.observe(timeline_observation(
        63.5, 295.5, 305.5, 0.5, 0.5, true,
        core::LoadAttempt{2}), true);
    CHECK_FALSE(capped.recover);
    CHECK(capped.outcome == player::TimelineRecoveryOutcome::SuppressedRateLimit);

    recovery.reset();
    recovery.begin_load(core::Generation{2}, core::LoadAttempt{1});
    auto new_generation = timeline_observation(
        64.0, 400.0, 404.0, 0.5, 0.5);
    new_generation.generation = core::Generation{2};
    new_generation.timeline.generation = core::Generation{2};
    (void)recovery.observe(new_generation, true);
    auto fresh_rewind = timeline_observation(
        64.5, 395.0, 405.0, -5.0, 1.0, false);
    fresh_rewind.generation = core::Generation{2};
    fresh_rewind.timeline.generation = core::Generation{2};
    REQUIRE(recovery.observe(fresh_rewind, true).outcome ==
            player::TimelineRecoveryOutcome::CandidateArmed);
    auto fresh_confirm = timeline_observation(
        65.0, 395.5, 405.5, 0.5, 0.5);
    fresh_confirm.generation = core::Generation{2};
    fresh_confirm.timeline.generation = core::Generation{2};
    CHECK(recovery.observe(fresh_confirm, true).recover);
}

TEST_CASE("adapter drains every edge once in mpv order with issue-time generations") {
    player::PlayerEventAdapter adapter;
    adapter.track_load(10, core::Generation{1}, core::LoadAttempt{1});
    adapter.command_result(10, 0, 100);
    adapter.start_file(100);
    adapter.playback_restart(100);
    adapter.track_load(11, core::Generation{2}, core::LoadAttempt{1});
    adapter.command_result(11, 0, 101);
    adapter.start_file(101);
    adapter.end_file(100, player::PlayerEndReason::Stop, 0);
    adapter.playback_restart(101);

    const auto events = adapter.drain();
    REQUIRE(events.size() == 5);
    CHECK(events[0].generation == core::Generation{1});
    CHECK(std::holds_alternative<player::LoadCommandResult>(events[0].payload));
    CHECK(events[1].generation == core::Generation{1});
    CHECK(std::holds_alternative<player::FirstPlaybackStart>(events[1].payload));
    CHECK(events[2].generation == core::Generation{2});
    CHECK(std::holds_alternative<player::LoadCommandResult>(events[2].payload));
    CHECK(events[3].generation == core::Generation{1});
    CHECK(std::get<player::PlaybackStopped>(events[3].payload).kind ==
          player::IntentionalStopKind::Replaced);
    CHECK(events[4].generation == core::Generation{2});
    CHECK(std::holds_alternative<player::FirstPlaybackStart>(events[4].payload));
    CHECK(adapter.drain().empty());
}

TEST_CASE("a superseded load cannot publish a late first playback start") {
    player::PlayerEventAdapter adapter;
    adapter.track_load(10, core::Generation{4}, core::LoadAttempt{1});
    adapter.command_result(10, 0, 100);
    adapter.start_file(100);
    REQUIRE(adapter.drain().size() == 1);

    // Recovery retains the generation. Until its START_FILE arrives, the old
    // entry is still active and may publish a delayed restart; that edge belongs
    // to the load being replaced and must not arm the new load's rebuffer gate.
    adapter.track_load(11, core::Generation{4}, core::LoadAttempt{2});
    adapter.command_result(11, 0, 101);
    REQUIRE(adapter.drain().size() == 1);
    adapter.playback_restart(100);
    CHECK(adapter.drain().empty());

    adapter.start_file(101);
    adapter.end_file(100, player::PlayerEndReason::Stop, 0);
    adapter.playback_restart(101);
    const auto events = adapter.drain();
    REQUIRE(events.size() == 2);
    CHECK(std::get<player::PlaybackStopped>(events[0].payload).kind ==
          player::IntentionalStopKind::Replaced);
    CHECK(std::holds_alternative<player::FirstPlaybackStart>(events[1].payload));
    CHECK(events[1].generation == core::Generation{4});
    CHECK(events[1].load_attempt == core::LoadAttempt{2});
}

TEST_CASE("structured end reason stays attached to its load generation") {
    player::PlayerEventAdapter adapter;
    adapter.track_load(10, core::Generation{7}, core::LoadAttempt{1});
    adapter.command_result(10, 0, 70);
    adapter.start_file(70);
    adapter.end_file(70, player::PlayerEndReason::Error, -13);
    const auto events = adapter.drain();
    REQUIRE(events.size() == 2);
    const auto& ended = std::get<player::EndFileEvent>(events[1].payload);
    CHECK(events[1].generation == core::Generation{7});
    CHECK(ended.reason == player::PlayerEndReason::Error);
    CHECK(ended.error == -13);
}

TEST_CASE("HLS redirect transfers generation ownership to inserted entries") {
    player::PlayerEventAdapter adapter;
    adapter.track_load(12, core::Generation{8}, core::LoadAttempt{1});
    adapter.command_result(12, 0, 80);
    adapter.start_file(80);
    adapter.end_file(80, player::PlayerEndReason::Redirect, 0, 81, 2);
    adapter.start_file(81);
    adapter.playback_restart(81);
    adapter.end_file(81, player::PlayerEndReason::Eof, 0);
    const auto events = adapter.drain();
    REQUIRE(events.size() == 4);
    for (const auto& event : events) CHECK(event.generation == core::Generation{8});
    CHECK(std::get<player::EndFileEvent>(events[1].payload).reason ==
          player::PlayerEndReason::Redirect);
    CHECK(std::holds_alternative<player::FirstPlaybackStart>(events[2].payload));
    CHECK(std::get<player::EndFileEvent>(events[3].payload).reason ==
          player::PlayerEndReason::Eof);
}

TEST_CASE("redirect rehash preserves stopped-entry load identity") {
    player::PlayerEventAdapter adapter;
    adapter.track_load(12, core::Generation{8}, core::LoadAttempt{3});
    adapter.command_result(12, 0, 80);
    adapter.start_file(80);
    adapter.intentional_stop(80, core::Generation{9},
                             player::IntentionalStopKind::Requested);

    // More entries than libstdc++'s initial bucket count makes a rehash likely;
    // correctness must not depend on whether it happens.
    adapter.end_file(80, player::PlayerEndReason::Redirect, 0, 81, 64);
    adapter.start_file(81);
    adapter.playback_restart(81);

    const auto events = adapter.drain();
    REQUIRE(events.size() == 3);
    CHECK(events[1].generation == core::Generation{9});
    CHECK(events[1].load_attempt == core::LoadAttempt{3});
    CHECK(std::get<player::PlaybackStopped>(events[1].payload).kind ==
          player::IntentionalStopKind::Requested);
    CHECK(events[2].generation == core::Generation{8});
    CHECK(events[2].load_attempt == core::LoadAttempt{3});
    CHECK(std::holds_alternative<player::FirstPlaybackStart>(events[2].payload));
}

TEST_CASE("load and buffer property command rejection are observable") {
    player::PlayerEventAdapter adapter;
    adapter.track_load(10, core::Generation{3}, core::LoadAttempt{1});
    adapter.command_rejected_immediately(10, -4);
    adapter.track_property(20, core::Generation{3}, core::BufferPhase::Steady,
                           player::BufferProperty::CacheSeconds);
    adapter.command_result(20, -9);
    const auto events = adapter.drain();
    REQUIRE(events.size() == 2);
    const auto& load = std::get<player::LoadCommandResult>(events[0].payload);
    CHECK_FALSE(load.accepted);
    CHECK(load.error == -4);
    const auto& property = std::get<player::PropertyCommandResult>(events[1].payload);
    CHECK_FALSE(property.accepted);
    CHECK(property.phase == core::BufferPhase::Steady);
    CHECK(property.property == player::BufferProperty::CacheSeconds);
}

TEST_CASE("one backend failure produces one edge until a new load is issued") {
    player::PlayerEventAdapter adapter;
    adapter.backend_failed(core::Generation{2}, core::LoadAttempt{1}, -1);
    adapter.backend_failed(core::Generation{2}, core::LoadAttempt{1}, -2);
    auto events = adapter.drain();
    REQUIRE(events.size() == 1);
    CHECK(std::holds_alternative<player::BackendFailed>(events[0].payload));
    adapter.track_load(30, core::Generation{2}, core::LoadAttempt{2});
    adapter.backend_failed(core::Generation{2}, core::LoadAttempt{2}, -3);
    events = adapter.drain();
    REQUIRE(events.size() == 1);
    CHECK(std::get<player::BackendFailed>(events[0].payload).error == -3);
}

TEST_CASE("backend failure and disposal release stale correlations") {
    player::PlayerEventAdapter adapter;
    adapter.track_load(10, core::Generation{2}, core::LoadAttempt{1});
    adapter.track_property(20, core::Generation{2}, core::BufferPhase::Zap,
                           player::BufferProperty::CacheSeconds);
    adapter.backend_failed(core::Generation{2}, core::LoadAttempt{1}, -7);
    adapter.command_result(10, 0);
    adapter.command_result(20, 0);
    adapter.start_file(200);
    auto events = adapter.drain();
    REQUIRE(events.size() == 1);
    CHECK(std::get<player::BackendFailed>(events[0].payload).error == -7);
    CHECK_FALSE(adapter.active_generation());

    adapter.track_load(30, core::Generation{3}, core::LoadAttempt{1});
    adapter.command_result(30, 0, 300);
    adapter.dispose();
    CHECK(adapter.drain().empty());
    adapter.start_file(300);
    CHECK_FALSE(adapter.active_generation());
}

TEST_CASE("retiring a generation before start-file fences every late edge") {
    player::PlayerEventAdapter adapter;
    adapter.track_load(10, core::Generation{4}, core::LoadAttempt{1});
    adapter.track_property(20, core::Generation{4}, core::BufferPhase::Zap,
                           player::BufferProperty::CacheSeconds);
    adapter.command_result(10, 0, 400);
    adapter.retire_generation(core::Generation{4});

    // A rapid restart is already queued when the old backend edges arrive.
    adapter.track_load(11, core::Generation{5}, core::LoadAttempt{1});
    adapter.command_result(20, 0);
    CHECK_FALSE(adapter.start_file(400));
    adapter.playback_restart(400);
    adapter.end_file(400, player::PlayerEndReason::Error, -13);

    adapter.command_result(11, 0, 500);
    CHECK(adapter.start_file(500));
    adapter.playback_restart(500);
    const auto events = adapter.drain();
    REQUIRE(events.size() == 2);
    CHECK(events[0].generation == core::Generation{5});
    CHECK(std::holds_alternative<player::LoadCommandResult>(events[0].payload));
    CHECK(events[1].generation == core::Generation{5});
    CHECK(std::holds_alternative<player::FirstPlaybackStart>(events[1].payload));
    CHECK(adapter.active_generation() == core::Generation{5});
    CHECK(adapter.active_load_attempt() == core::LoadAttempt{1});
}

TEST_CASE("an accepted retired load may produce no start-file before the fresh load") {
    player::PlayerEventAdapter adapter;
    adapter.track_load(10, core::Generation{4}, core::LoadAttempt{1});
    adapter.retire_generation(core::Generation{4});
    adapter.track_load(11, core::Generation{5}, core::LoadAttempt{1});

    // mpv can accept the old loadfile, then let Stop clear it before startup.
    // The next START_FILE is therefore the fresh entry, not an old tombstone.
    adapter.command_result(10, 0, 400);
    adapter.command_result(11, 0, 500);
    CHECK(adapter.start_file(500));
    adapter.playback_restart(500);

    const auto events = adapter.drain();
    REQUIRE(events.size() == 2);
    CHECK(events[0].generation == core::Generation{5});
    CHECK(std::holds_alternative<player::LoadCommandResult>(events[0].payload));
    CHECK(events[1].generation == core::Generation{5});
    CHECK(std::holds_alternative<player::FirstPlaybackStart>(events[1].payload));
    CHECK(adapter.active_generation() == core::Generation{5});
}

TEST_CASE("a rejected retired load cannot consume the next start-file edge") {
    player::PlayerEventAdapter adapter;
    adapter.track_load(10, core::Generation{4}, core::LoadAttempt{1});
    adapter.retire_generation(core::Generation{4});
    adapter.track_load(11, core::Generation{5}, core::LoadAttempt{1});

    adapter.command_result(10, -1);
    adapter.command_result(11, 0, 500);
    CHECK(adapter.start_file(500));
    adapter.playback_restart(500);
    const auto events = adapter.drain();
    REQUIRE(events.size() == 2);
    CHECK(events[0].generation == core::Generation{5});
    CHECK(events[1].generation == core::Generation{5});
    CHECK(adapter.active_generation() == core::Generation{5});
}

TEST_CASE("retiring an active generation makes its stop acknowledgement cleanup-only") {
    player::PlayerEventAdapter adapter;
    adapter.track_load(10, core::Generation{4}, core::LoadAttempt{1});
    adapter.command_result(10, 0, 400);
    adapter.start_file(400);
    adapter.playback_restart(400);
    REQUIRE(adapter.drain().size() == 2);

    adapter.retire_generation(core::Generation{4});
    adapter.end_file(400, player::PlayerEndReason::Stop, 0);
    CHECK(adapter.drain().empty());
    CHECK_FALSE(adapter.active_generation());
    CHECK_FALSE(adapter.active_load_attempt());
}

TEST_CASE("explicit and replacement stops are classified separately") {
    player::PlayerEventAdapter adapter;
    adapter.track_load(1, core::Generation{4}, core::LoadAttempt{1});
    adapter.command_result(1, 0, 40);
    adapter.start_file(40);
    adapter.intentional_stop(40, core::Generation{5},
                             player::IntentionalStopKind::Requested);
    adapter.end_file(40, player::PlayerEndReason::Stop, 0);
    const auto events = adapter.drain();
    REQUIRE(events.size() == 2);
    CHECK(events[1].generation == core::Generation{5});
    CHECK(std::get<player::PlaybackStopped>(events[1].payload).kind ==
          player::IntentionalStopKind::Requested);
}

TEST_CASE("buffer phases apply once per load and reject stale generations") {
    player::BufferPhaseGate gate;
    gate.begin_load(core::Generation{4});
    CHECK(gate.begin(core::Generation{4}, core::BufferPhase::Zap));
    CHECK_FALSE(gate.begin(core::Generation{4}, core::BufferPhase::Zap));
    CHECK_FALSE(gate.begin(core::Generation{3}, core::BufferPhase::Steady));
    CHECK_FALSE(gate.settle(core::Generation{4}, core::BufferPhase::Zap,
                            player::BufferPhaseProperty::CacheSeconds, true));
    const auto applied = gate.settle(
        core::Generation{4}, core::BufferPhase::Zap,
        player::BufferPhaseProperty::ReadaheadSeconds, true);
    REQUIRE(applied);
    CHECK(*applied == player::BufferPhaseCommandState::Applied);
    CHECK(gate.state(core::Generation{4}, core::BufferPhase::Zap) ==
          player::BufferPhaseCommandState::Applied);
    CHECK(gate.begin(core::Generation{4}, core::BufferPhase::Steady));

    // A recovery reload is a new load epoch even though it retains generation.
    gate.begin_load(core::Generation{4});
    CHECK(gate.begin(core::Generation{4}, core::BufferPhase::Zap));
    CHECK(gate.begin(core::Generation{4}, core::BufferPhase::Steady));
}

TEST_CASE("buffer phase settles only after both results and exposes partial failure") {
    player::BufferPhaseGate gate;
    gate.begin_load(core::Generation{5});
    REQUIRE(gate.begin(core::Generation{5}, core::BufferPhase::Steady));
    CHECK_FALSE(gate.settle(core::Generation{4}, core::BufferPhase::Steady,
                            player::BufferPhaseProperty::CacheSeconds, true));
    CHECK_FALSE(gate.settle(core::Generation{5}, core::BufferPhase::Steady,
                            player::BufferPhaseProperty::CacheSeconds, true));
    CHECK(gate.state(core::Generation{5}, core::BufferPhase::Steady) ==
          player::BufferPhaseCommandState::Pending);
    const auto failed = gate.settle(
        core::Generation{5}, core::BufferPhase::Steady,
        player::BufferPhaseProperty::ReadaheadSeconds, false);
    REQUIRE(failed);
    CHECK(*failed == player::BufferPhaseCommandState::Failed);
    CHECK(gate.state(core::Generation{5}, core::BufferPhase::Steady) ==
          player::BufferPhaseCommandState::Failed);
    CHECK_FALSE(gate.begin(core::Generation{5}, core::BufferPhase::Steady));

    gate.begin_load(core::Generation{6});
    REQUIRE(gate.begin(core::Generation{6}, core::BufferPhase::Zap));
    const auto immediate_failure = gate.settle(
        core::Generation{6}, core::BufferPhase::Zap,
        player::BufferPhaseProperty::CacheSeconds, false);
    REQUIRE(immediate_failure);
    CHECK(*immediate_failure == player::BufferPhaseCommandState::Failed);
}

TEST_CASE("new loads clear observations but retain lifetime diagnostics") {
    player::Diagnostics diagnostics;
    diagnostics.video_codec = "old-codec";
    diagnostics.hwdec_active = "old-hwdec";
    diagnostics.video_width = 1920;
    diagnostics.video_height = 1080;
    diagnostics.core_idle = true;
    diagnostics.paused_for_cache = true;
    diagnostics.playback_time_seconds = 45.0;
    diagnostics.cache_end_seconds = 54.0;
    diagnostics.cache_duration_seconds = 9.0;
    diagnostics.input_rate_bytes_per_second = 20.0;
    diagnostics.container_fps = 50.0;
    diagnostics.buffer_phase = core::BufferPhase::Steady;
    diagnostics.buffer_commands_accepted = 7;
    diagnostics.mpv_playback_restart_events = 3;
    diagnostics.engine_message_count = 12;
    diagnostics.last_engine_message = player::SanitizedEngineWarning{
        player::EngineWarningComponent::Demuxer,
        player::EngineWarningCategory::CorruptPacket};
    diagnostics.unattributed_engine_message_count = 4;
    diagnostics.last_unattributed_engine_message = player::SanitizedEngineWarning{
        player::EngineWarningComponent::Http,
        player::EngineWarningCategory::NetworkTimeout};
    diagnostics.request_shape = player::inspect_request_shape(
        "https://user:secret@example.invalid/live/u/p/123.ts?token=secret",
        core::LoadIntent::FreshSelection, core::LoadAttempt{1},
        core::RecoveryTransport::MpegTs, false);

    player::reset_load_observations(diagnostics);

    CHECK(diagnostics.video_codec.empty());
    CHECK(diagnostics.hwdec_active.empty());
    CHECK(diagnostics.video_width == 0);
    CHECK(diagnostics.video_height == 0);
    CHECK_FALSE(diagnostics.core_idle);
    CHECK_FALSE(diagnostics.paused_for_cache);
    CHECK_FALSE(diagnostics.playback_time_seconds);
    CHECK_FALSE(diagnostics.cache_end_seconds);
    CHECK_FALSE(diagnostics.cache_duration_seconds);
    CHECK_FALSE(diagnostics.input_rate_bytes_per_second);
    CHECK_FALSE(diagnostics.container_fps);
    CHECK(diagnostics.buffer_phase == core::BufferPhase::Zap);
    CHECK(diagnostics.buffer_phase_command_state ==
          player::BufferPhaseCommandState::Unissued);
    CHECK(diagnostics.buffer_commands_accepted == 7);
    CHECK(diagnostics.mpv_playback_restart_events == 3);
    CHECK(diagnostics.engine_message_count == 0);
    CHECK_FALSE(diagnostics.last_engine_message);
    CHECK(diagnostics.unattributed_engine_message_count == 0);
    CHECK_FALSE(diagnostics.last_unattributed_engine_message);
    CHECK_FALSE(diagnostics.request_shape);
}

TEST_CASE("timeline presentation distinguishes jumps stops pauses and resumes") {
    core::TimelineEvidence evidence;
    evidence.elapsed_seconds = 2.0;
    evidence.playback_movement_seconds = -1.0;
    evidence.playback_deviation_seconds = -3.0;
    CHECK(player::classify_timeline(evidence, core::kDefaultHealthPolicy) ==
          player::TimelineClassification::Backward);

    evidence.playback_movement_seconds = 0.0;
    evidence.playback_deviation_seconds = -2.0;
    CHECK(player::classify_timeline(evidence, core::kDefaultHealthPolicy) ==
          player::TimelineClassification::NoProgress);
    evidence.cache_paused = true;
    CHECK(player::classify_timeline(evidence, core::kDefaultHealthPolicy) ==
          player::TimelineClassification::PausedNoProgress);

    evidence.cache_paused = false;
    evidence.previous_cache_paused = true;
    evidence.playback_movement_seconds = 0.25;
    evidence.playback_deviation_seconds = -1.75;
    CHECK(player::classify_timeline(evidence, core::kDefaultHealthPolicy) ==
          player::TimelineClassification::ResumeLag);

    evidence.previous_cache_paused = false;
    evidence.playback_movement_seconds = 3.0;
    evidence.playback_deviation_seconds = 1.5;
    CHECK(player::classify_timeline(evidence, core::kDefaultHealthPolicy) ==
          player::TimelineClassification::ForwardJump);
    evidence.playback_deviation_seconds = 0.0;
    CHECK(player::classify_timeline(evidence, core::kDefaultHealthPolicy) ==
          player::TimelineClassification::NormalAdvance);

    // At the shipped cadence, a modest behind-wall-clock advance remains raw
    // evidence rather than being mislabeled as a discontinuity-scale lag.
    evidence.elapsed_seconds = core::kDefaultHealthPolicy.sample_interval.count();
    evidence.playback_movement_seconds = 0.25;
    evidence.playback_deviation_seconds = -0.25;
    CHECK(player::classify_timeline(evidence, core::kDefaultHealthPolicy) ==
          player::TimelineClassification::NormalAdvance);

    auto sensitive = core::kDefaultHealthPolicy;
    sensitive.discontinuity_jump_seconds = 0.1;
    CHECK(player::classify_timeline(evidence, sensitive) ==
          player::TimelineClassification::ForwardLag);
}

TEST_CASE("engine warning summaries retain context without provider data") {
    const std::string raw =
        "Continuity check failed for pid 17 at "
        "https://alice:password@provider.invalid/live/alice/password/42.ts?token=secret "
        "Authorization: Bearer super-secret";
    const auto warning = player::sanitize_engine_warning("ffmpeg/demuxer", raw, "error");
    CHECK(warning.component == player::EngineWarningComponent::Demuxer);
    CHECK(warning.category == player::EngineWarningCategory::ContinuityError);
    CHECK(warning.severity == player::EngineLogSeverity::Error);

    const std::string retained = std::string(player::to_string(warning.severity)) + "/" +
                                 player::to_string(warning.component) + "/" +
                                 player::to_string(warning.category);
    for (const std::string_view forbidden : {
             "provider.invalid", "alice", "password", "token", "secret",
             "Authorization", "Bearer", "https://"}) {
        CHECK(retained.find(forbidden) == std::string::npos);
    }

    CHECK(player::sanitize_engine_warning(
              "ffmpeg/video", "Non-monotonous DTS in output stream https://host/?key=x",
              "warn")
              .category == player::EngineWarningCategory::NonMonotonicTimestamp);
    CHECK(player::sanitize_engine_warning(
              "ffmpeg/demuxer", "PES packet size mismatch at http://u:p@host/live/u/p/1.ts",
              "fatal")
              .category == player::EngineWarningCategory::CorruptPacket);
    const auto segment = player::sanitize_engine_warning(
        "ffmpeg/demuxer",
        "hls: keepalive request failed when opening url "
        "https://u:p@host/playlist.m3u8?token=x", "warn");
    CHECK(segment.category == player::EngineWarningCategory::HlsSegment);
    CHECK(segment.severity == player::EngineLogSeverity::Warning);
    CHECK(player::sanitize_engine_warning(
              "ffmpeg/demuxer", "hls: keepalive request failed when parsing playlist", "warn")
              .category == player::EngineWarningCategory::HlsPlaylist);
}

TEST_CASE("engine messages stay unattributed until the target entry is active") {
    const auto old_generation = std::optional{core::Generation{4}};
    const auto new_generation = std::optional{core::Generation{5}};

    CHECK(player::classify_engine_message_attribution(
              false, old_generation, new_generation) ==
          player::EngineMessageAttribution::Unattributed);
    // Recovery keeps the generation, so the START_FILE fence is independently
    // necessary to distinguish its handover window.
    CHECK(player::classify_engine_message_attribution(
              false, new_generation, new_generation) ==
          player::EngineMessageAttribution::Unattributed);
    CHECK(player::classify_engine_message_attribution(
              true, old_generation, new_generation) ==
          player::EngineMessageAttribution::Unattributed);
    CHECK(player::classify_engine_message_attribution(
              true, new_generation, new_generation) ==
          player::EngineMessageAttribution::ActiveEntry);
    CHECK(player::classify_engine_message_attribution(
              true, std::nullopt, new_generation) ==
          player::EngineMessageAttribution::Unattributed);
}

TEST_CASE("engine diagnostic logging keeps one line per triple and attribution") {
    player::EngineDiagnosticLogGate gate;
    const player::SanitizedEngineWarning warning{
        player::EngineWarningComponent::Demuxer,
        player::EngineWarningCategory::CorruptPacket,
        player::EngineLogSeverity::Warning};

    CHECK(gate.first_occurrence(player::EngineMessageAttribution::Unattributed,
                                warning));
    CHECK_FALSE(gate.first_occurrence(player::EngineMessageAttribution::Unattributed,
                                      warning));
    CHECK(gate.first_occurrence(player::EngineMessageAttribution::ActiveEntry,
                                warning));

    auto error = warning;
    error.severity = player::EngineLogSeverity::Error;
    CHECK(gate.first_occurrence(player::EngineMessageAttribution::ActiveEntry,
                                error));
    CHECK_FALSE(gate.first_occurrence(player::EngineMessageAttribution::ActiveEntry,
                                      error));

    gate.reset();
    CHECK(gate.first_occurrence(player::EngineMessageAttribution::ActiveEntry,
                                warning));
}

TEST_CASE("fresh selection request shape is useful and URL free") {
    const std::string raw =
        "https://alice:password@provider.invalid/live/alice/password/42.ts?token=secret";
    const auto shape = player::inspect_request_shape(
        raw, core::LoadIntent::FreshSelection, core::LoadAttempt{1},
        core::RecoveryTransport::MpegTs, false,
        {.provider_session = 3, .channel_session = 7});
    CHECK(shape.intent == core::LoadIntent::FreshSelection);
    CHECK(shape.scheme == player::RequestScheme::Https);
    CHECK(shape.target == player::RequestTargetShape::XtreamLive);
    CHECK(shape.query_present);
    CHECK(shape.userinfo_present);
    CHECK_FALSE(shape.forced_format);
    CHECK(shape.correlation.provider_session == 3);
    CHECK(shape.correlation.channel_session == 7);

    const std::string retained = std::string(core::to_string(shape.intent)) + "/" +
        player::to_string(shape.scheme) + "/" + player::to_string(shape.target);
    for (const std::string_view forbidden : {
             "provider.invalid", "alice", "password", "token", "secret", "https://"}) {
        CHECK(retained.find(forbidden) == std::string::npos);
    }
}

TEST_CASE("recovery telemetry is load scoped and cannot retain credentials") {
    const std::string raw =
        "https://alice:password@provider.invalid/live/alice/password/42.ts?token=secret";
    const auto shape = player::inspect_request_shape(
        raw, core::LoadIntent::RecoveryReopen, core::LoadAttempt{2},
        core::RecoveryTransport::MpegTs, true,
        {.provider_session = 3, .channel_session = 7});
    core::SupervisorTransition transition;
    transition.attempt = 2;
    transition.generation = core::Generation{9};
    transition.load_attempt = core::LoadAttempt{2};
    transition.load_intent = core::LoadIntent::RecoveryReopen;
    transition.escalation = core::RecoveryEscalation::SourceReopen;
    transition.outcome = core::RecoveryOutcome::RenewedEof;
    transition.last_progress_to_decision = core::Duration{1.25};
    transition.decision_to_command = core::Duration{0.5};
    transition.command_to_first_frame = core::Duration{0.8};
    transition.first_frame_to_outcome = core::Duration{1.1};
    transition.recovered_load_lifetime = core::Duration{1.9};
    const player::RecoveryDecisionEvidence evidence{
        .cache_paused = false,
        .playback_movement_seconds = -0.75,
        .cache_end_movement_seconds = 0.125,
        .engine_warning = player::sanitize_engine_warning(
            "ffmpeg/demuxer",
            "Continuity check failed at " + raw + " Authorization: Bearer hidden",
            "error"),
    };

    const std::string retained =
        player::format_recovery_telemetry(transition, shape, evidence);
    CHECK(retained.find("provider-session=3") != std::string::npos);
    CHECK(retained.find("channel-session=7") != std::string::npos);
    CHECK(retained.find("generation=9") != std::string::npos);
    CHECK(retained.find("load-attempt=2") != std::string::npos);
    CHECK(retained.find("intent=recovery-reopen") != std::string::npos);
    CHECK(retained.find("escalation=source-reopen") != std::string::npos);
    CHECK(retained.find("outcome=renewed-eof") != std::string::npos);
    CHECK(retained.find("last-progress-to-decision=1250ms") != std::string::npos);
    CHECK(retained.find("decision-to-command=500ms") != std::string::npos);
    CHECK(retained.find("command-to-first-frame=800ms") != std::string::npos);
    CHECK(retained.find("first-frame-to-outcome=1100ms") != std::string::npos);
    CHECK(retained.find("recovered-load-lifetime=1900ms") != std::string::npos);
    CHECK(retained.find("warning-category=continuity-error") != std::string::npos);
    for (const std::string_view forbidden : {
             "provider.invalid", "alice", "password", "token", "secret",
             "Authorization", "Bearer", "hidden", "https://"}) {
        CHECK(retained.find(forbidden) == std::string::npos);
    }
}

TEST_CASE("session target identities retain selection grouping and refresh on live Start") {
    player::SessionTargetRegistry registry;
    CHECK(registry.begin_provider_session() == 1);
    const auto first = registry.identify_channel("provider-stream-id-42");
    const auto other = registry.identify_channel("provider-stream-id-99");
    const auto reselected = registry.identify_channel("provider-stream-id-42");
    CHECK(first.provider_session == 1);
    CHECK(first.channel_session == 1);
    CHECK(other.channel_session == 2);
    CHECK(reselected.provider_session == first.provider_session);
    CHECK(reselected.channel_session == first.channel_session);

    const auto restarted = registry.identify_fresh_channel("provider-stream-id-42");
    CHECK(restarted.provider_session == first.provider_session);
    CHECK(restarted.channel_session == 3);
    CHECK(registry.identify_channel("provider-stream-id-42").channel_session ==
          restarted.channel_session);

    CHECK(registry.begin_provider_session() == 2);
    const auto replacement_provider = registry.identify_channel("provider-stream-id-42");
    CHECK(replacement_provider.provider_session == 2);
    CHECK(replacement_provider.channel_session == 1);
}

TEST_CASE("playback controls expose stop-start for TS and reserve pause-resume") {
    using player::PlaybackControl;
    using player::PlaybackControlCapability;
    using player::PlaybackIntent;

    CHECK(player::playback_control(PlaybackControlCapability::RestartAtLiveEdge,
                                   PlaybackIntent::Running) == PlaybackControl::Stop);
    CHECK(player::playback_control(PlaybackControlCapability::RestartAtLiveEdge,
                                   PlaybackIntent::StoppedByUser) == PlaybackControl::Start);
    CHECK(player::playback_control(PlaybackControlCapability::ResumeFromPosition,
                                   PlaybackIntent::Running) == PlaybackControl::Pause);
    CHECK(player::playback_control(PlaybackControlCapability::ResumeFromPosition,
                                   PlaybackIntent::SuspendedByUser) == PlaybackControl::Resume);
    CHECK_FALSE(player::position_preserving_pause_requested(
        PlaybackControlCapability::RestartAtLiveEdge, PlaybackIntent::StoppedByUser));
    CHECK(player::position_preserving_pause_requested(
        PlaybackControlCapability::ResumeFromPosition, PlaybackIntent::SuspendedByUser));
}

TEST_CASE("live start distinguishes no selection missing retention and a fresh request") {
    auto intent = player::PlaybackIntent::StoppedByUser;
    std::string none;
    CHECK(player::prepare_live_start(none, intent, false) ==
          player::LiveStartDecision::NoSelection);
    CHECK(intent == player::PlaybackIntent::StoppedByUser);

    std::string missing = "retained";
    CHECK(player::prepare_live_start(missing, intent, false) ==
          player::LiveStartDecision::RetainedChannelMissing);
    CHECK(missing.empty());
    CHECK(intent == player::PlaybackIntent::StoppedByUser);

    std::string retained = "retained";
    CHECK(player::prepare_live_start(retained, intent, true) ==
          player::LiveStartDecision::StartFresh);
    CHECK(retained == "retained");
    CHECK(intent == player::PlaybackIntent::Running);
}

TEST_CASE("pinned transport log patterns produce only sanitized classifications") {
    const auto classify = [](std::string_view text,
                             core::RecoveryTransport transport = core::RecoveryTransport::MpegTs,
                             bool loaded = false, bool forced = false) {
        return player::classify_transport_log(text, transport, loaded, forced);
    };
    const auto is_failure = [&](std::string_view text,
                                core::TransportFailureReason expected,
                                core::RecoveryTransport transport = core::RecoveryTransport::MpegTs,
                                bool loaded = false, bool forced = false) {
        const auto result = classify(text, transport, loaded, forced);
        return result && std::holds_alternative<core::TransportFailureReason>(*result) &&
               std::get<core::TransportFailureReason>(*result) == expected;
    };
    for (const auto text : {"HTTP error 401 Unauthorized https://user:secret@example.invalid/live",
                            "http_code=401"}) {
        const auto result = classify(text);
        REQUIRE(result);
        CHECK(std::holds_alternative<player::AuthenticationRejected>(*result));
    }
    for (const auto text : {"connection timed out", "Operation timed out",
                            "request timed out",
                            "Error reading HTTP response: Error number -138 occurred"}) {
        CHECK(is_failure(text, core::TransportFailureReason::HttpRequestTimeout));
    }
    for (const auto text : {"HTTP error 503 Service Unavailable",
                            "Error reading HTTP response: Error number -10054 occurred",
                            "hls: keepalive request failed when parsing playlist"}) {
        CHECK(is_failure(text, core::TransportFailureReason::HlsPlaylistFailed,
                         core::RecoveryTransport::Hls));
    }
    for (const auto text : {"failed to open media segment",
                            "unable to open next segment",
                            "hls: keepalive request failed when opening url"}) {
        CHECK(is_failure(text, core::TransportFailureReason::HlsSegmentUnavailable,
                         core::RecoveryTransport::Hls));
    }
    for (const auto text : {"Failed to recognize file format",
                            "Could not determine the input format",
                            "Could not find format"}) {
        CHECK(is_failure(text, core::TransportFailureReason::FormatProbeRequired));
    }
    CHECK(is_failure("Invalid data found when processing input",
                     core::TransportFailureReason::FormatProbeRequired));
    CHECK_FALSE(classify("Invalid data found when processing input",
                         core::RecoveryTransport::MpegTs, true, false));
    CHECK_FALSE(classify("Failed to recognize file format",
                         core::RecoveryTransport::MpegTs, false, true));
    CHECK_FALSE(classify("HTTP error 403 Forbidden"));
    CHECK_FALSE(classify("generic network error"));
    CHECK_FALSE(classify("HTTP error 503 Service Unavailable",
                         core::RecoveryTransport::MpegTs));
}

TEST_CASE("transport classifications remain generation-scoped adapter edges") {
    player::PlayerEventAdapter adapter;
    adapter.authentication_rejected(core::Generation{12}, core::LoadAttempt{1});
    adapter.transport_failure(core::Generation{13}, core::LoadAttempt{1},
                              core::TransportFailureReason::HlsPlaylistFailed);
    const auto events = adapter.drain();
    REQUIRE(events.size() == 2);
    CHECK(events[0].generation == core::Generation{12});
    CHECK(std::holds_alternative<player::PlayerAuthenticationRejected>(events[0].payload));
    CHECK(events[1].generation == core::Generation{13});
    CHECK(std::get<player::TransportFailureDetected>(events[1].payload).reason ==
          core::TransportFailureReason::HlsPlaylistFailed);
}
