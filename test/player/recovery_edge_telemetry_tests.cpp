#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "player/playback_observability.hpp"
#include "player/recovery_edge_telemetry.hpp"

using namespace coax;
using Catch::Approx;

namespace {

player::RecoveryEdgeAnchor anchor() {
    return {
        .generation = core::Generation{4},
        .outgoing_load_attempt = core::LoadAttempt{1},
        .recovered_load_attempt = core::LoadAttempt{2},
        .observed_at = core::TimePoint{core::seconds(10.0)},
        .playback_time_seconds = 100.0,
        .cache_end_seconds = 104.0,
        .cache_paused = false,
        .recovery_reason = core::DetectionReason::CacheStall,
    };
}

player::RecoveryEdgeObservation observation(
    double at, double playback, double cache_end,
    core::LoadAttempt attempt = core::LoadAttempt{2}) {
    return {
        .generation = core::Generation{4},
        .load_attempt = attempt,
        .observed_at = core::TimePoint{core::seconds(at)},
        .playback_time_seconds = playback,
        .cache_end_seconds = cache_end,
        .cache_paused = false,
        .point = player::RecoveryEdgeObservationPoint::HealthSample,
        .phase = player::RecoveryEdgePhase::Probation,
    };
}

}  // namespace

TEST_CASE("recovery edge telemetry records neutral wall projection residuals") {
    player::RecoveryEdgeObserver edge;
    edge.begin_recovery(anchor());

    const auto first = edge.observe(observation(13.0, 103.0, 107.0));
    REQUIRE(first);
    CHECK(first->data_status == player::RecoveryEdgeDataStatus::Complete);
    CHECK(first->projection_basis ==
          player::RecoveryEdgeProjectionBasis::AnchorPlusWallClock);
    CHECK(first->first_readable);
    CHECK(first->readable_sample_index == 1);
    REQUIRE(first->playback_wall_residual_seconds);
    REQUIRE(first->cache_end_wall_residual_seconds);
    REQUIRE(first->local_live_gap_change_seconds);
    CHECK(*first->playback_wall_residual_seconds == Approx(0.0));
    CHECK(*first->cache_end_wall_residual_seconds == Approx(0.0));
    CHECK(*first->local_live_gap_change_seconds == Approx(0.0));

    const auto second = edge.observe(observation(14.0, 104.0, 108.0));
    REQUIRE(second);
    CHECK_FALSE(second->first_readable);
    CHECK(second->readable_sample_index == 2);
}

TEST_CASE("recovery edge telemetry retains measurements without freshness labels") {
    SECTION("a coordinate behind the wall projection stays a signed residual") {
        player::RecoveryEdgeObserver edge;
        edge.begin_recovery(anchor());

        const auto report = edge.observe(observation(13.0, 93.0, 97.0));
        REQUIRE(report);
        CHECK(report->data_status == player::RecoveryEdgeDataStatus::Complete);
        REQUIRE(report->playback_wall_residual_seconds);
        REQUIRE(report->cache_end_wall_residual_seconds);
        CHECK(*report->playback_wall_residual_seconds == Approx(-10.0));
        CHECK(*report->cache_end_wall_residual_seconds == Approx(-10.0));
    }

    SECTION("a large coherent shift remains data rather than a clock verdict") {
        player::RecoveryEdgeObserver edge;
        edge.begin_recovery(anchor());

        const auto report = edge.observe(observation(13.0, -897.0, -893.0));
        REQUIRE(report);
        CHECK(report->data_status == player::RecoveryEdgeDataStatus::Complete);
        REQUIRE(report->playback_wall_residual_seconds);
        REQUIRE(report->cache_end_wall_residual_seconds);
        CHECK(*report->playback_wall_residual_seconds == Approx(-1000.0));
        CHECK(*report->cache_end_wall_residual_seconds == Approx(-1000.0));
    }
}

TEST_CASE("cache pauses are retained as context without changing projection") {
    SECTION("a paused outgoing anchor still yields the soak measurement") {
        player::RecoveryEdgeObserver edge;
        auto paused_anchor = anchor();
        paused_anchor.cache_paused = true;
        edge.begin_recovery(paused_anchor);

        const auto report = edge.observe(observation(13.0, 93.0, 97.0));
        REQUIRE(report);
        CHECK(report->data_status == player::RecoveryEdgeDataStatus::Complete);
        CHECK(report->anchor_cache_paused);
        CHECK_FALSE(report->cache_paused);
        CHECK(report->pause_seen_since_anchor);
        REQUIRE(report->cache_end_wall_residual_seconds);
        CHECK(*report->cache_end_wall_residual_seconds == Approx(-10.0));
    }

    SECTION("a recovered pause does not reset or reinterpret the anchor") {
        player::RecoveryEdgeObserver edge;
        edge.begin_recovery(anchor());

        auto paused = observation(13.0, 93.0, 97.0);
        paused.cache_paused = true;
        const auto paused_report = edge.observe(paused);
        REQUIRE(paused_report);
        CHECK(paused_report->data_status == player::RecoveryEdgeDataStatus::Complete);
        CHECK(paused_report->cache_paused);
        CHECK(paused_report->pause_seen_since_anchor);
        CHECK(paused_report->readable_sample_index == 1);

        const auto resumed = edge.observe(observation(18.0, 98.0, 102.0));
        REQUIRE(resumed);
        CHECK(resumed->data_status == player::RecoveryEdgeDataStatus::Complete);
        CHECK_FALSE(resumed->cache_paused);
        CHECK(resumed->pause_seen_since_anchor);
        CHECK(resumed->readable_sample_index == 2);
        REQUIRE(resumed->cache_end_wall_residual_seconds);
        CHECK(*resumed->cache_end_wall_residual_seconds == Approx(-10.0));
    }
}

TEST_CASE("recovery edge telemetry reports only factual data-quality failures") {
    player::RecoveryEdgeObserver edge;
    edge.begin_recovery(anchor());

    auto missing = observation(13.0, 103.0, 107.0);
    missing.cache_end_seconds.reset();
    const auto missing_report = edge.observe(missing);
    REQUIRE(missing_report);
    CHECK(missing_report->data_status ==
          player::RecoveryEdgeDataStatus::MissingTelemetry);
    CHECK_FALSE(missing_report->cache_end_wall_residual_seconds);
    CHECK(missing_report->readable_sample_index == 0);

    const auto stale = edge.observe(
        observation(13.0, 103.0, 107.0, core::LoadAttempt{1}));
    REQUIRE(stale);
    CHECK(stale->data_status == player::RecoveryEdgeDataStatus::StaleIdentity);
    CHECK_FALSE(stale->playback_wall_residual_seconds);

    const auto backwards = edge.observe(observation(9.0, 99.0, 103.0));
    REQUIRE(backwards);
    CHECK(backwards->data_status ==
          player::RecoveryEdgeDataStatus::NonMonotonicTime);
    CHECK_FALSE(backwards->elapsed_since_anchor);

    const auto current = edge.observe(observation(13.0, 103.0, 107.0));
    REQUIRE(current);
    CHECK(current->first_readable);
    CHECK(current->readable_sample_index == 1);

    edge.reset();
    CHECK_FALSE(edge.observe(observation(13.5, 103.5, 107.5)));
}

TEST_CASE("recovery edge telemetry stops after its bounded capture window") {
    player::RecoveryEdgeObserver edge;
    edge.begin_recovery(anchor());

    const auto boundary = edge.observe(observation(40.0, 130.0, 134.0));
    REQUIRE(boundary);
    CHECK(boundary->data_status == player::RecoveryEdgeDataStatus::Complete);
    CHECK(edge.active());

    CHECK_FALSE(edge.observe(observation(40.1, 130.1, 134.1)));
    CHECK_FALSE(edge.active());
    CHECK_FALSE(edge.observe(observation(40.2, 130.2, 134.2)));
}

TEST_CASE("recovery edge telemetry retains only identity flags and numeric deltas") {
    player::RecoveryEdgeObserver edge;
    edge.begin_recovery(anchor());
    const auto report = edge.observe(observation(13.0, 103.0, 107.0));
    REQUIRE(report);

    const std::string raw =
        "https://alice:password@provider.invalid/live/alice/password/42.ts?token=secret";
    const auto request = player::inspect_request_shape(
        raw, core::LoadIntent::RecoveryReopen, core::LoadAttempt{2},
        core::RecoveryTransport::MpegTs, false,
        {.provider_session = 3, .channel_session = 7});
    const auto retained = player::format_recovery_edge_telemetry(*report, request);

    CHECK(retained.find("provider-session=3") != std::string::npos);
    CHECK(retained.find("channel-session=7") != std::string::npos);
    CHECK(retained.find("outgoing-load-attempt=1") != std::string::npos);
    CHECK(retained.find("recovered-load-attempt=2") != std::string::npos);
    CHECK(retained.find("data-status=complete") != std::string::npos);
    CHECK(retained.find("projection-basis=anchor-plus-wall-clock") !=
          std::string::npos);
    CHECK(retained.find("cache-end-wall-residual=+0.000s") != std::string::npos);
    CHECK(retained.find("schema=recovery-edge-observability-v1") !=
          std::string::npos);
    for (const std::string_view forbidden : {
             "provider.invalid", "alice", "password", "token", "secret",
             "https://", "100.000", "104.000", "classification="}) {
        CHECK(retained.find(forbidden) == std::string::npos);
    }
}
