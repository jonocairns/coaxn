#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "player/playback_observability.hpp"
#include "player/recovery_freshness.hpp"

using namespace coax;
using Catch::Approx;

namespace {

player::RecoveryFreshnessAnchor anchor() {
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

player::RecoveryFreshnessObservation observation(
    double at, double playback, double cache_end,
    core::LoadAttempt attempt = core::LoadAttempt{2}) {
    return {
        .generation = core::Generation{4},
        .load_attempt = attempt,
        .observed_at = core::TimePoint{core::seconds(at)},
        .playback_time_seconds = playback,
        .cache_end_seconds = cache_end,
        .cache_paused = false,
        .point = player::RecoveryFreshnessObservationPoint::HealthSample,
        .phase = player::RecoveryFreshnessPhase::Probation,
    };
}

}  // namespace

TEST_CASE("recovery freshness recognizes a recovered head near the projected edge") {
    player::RecoveryFreshnessObserver freshness;
    freshness.begin_recovery(anchor());

    const auto report = freshness.observe(observation(13.0, 103.0, 107.0));
    REQUIRE(report);
    CHECK(report->classification == player::RecoveryFreshnessClassification::Fresh);
    CHECK(report->unverifiable_reason ==
          player::RecoveryFreshnessUnverifiableReason::None);
    CHECK(report->first_readable);
    CHECK(report->comparable_samples == 1);
    REQUIRE(report->cache_end_deficit_seconds);
    CHECK(*report->cache_end_deficit_seconds == Approx(0.0));
    REQUIRE(report->local_live_gap_change_seconds);
    CHECK(*report->local_live_gap_change_seconds == Approx(0.0));
}

TEST_CASE("recovery freshness distinguishes convergence from a persistent deficit") {
    SECTION("deficit shrinks") {
        player::RecoveryFreshnessObserver freshness;
        freshness.begin_recovery(anchor());
        REQUIRE(freshness.observe(observation(13.0, 93.0, 97.0)));

        const auto report = freshness.observe(observation(14.0, 95.0, 99.0));
        REQUIRE(report);
        CHECK(report->classification ==
              player::RecoveryFreshnessClassification::Converging);
        REQUIRE(report->deficit_improvement_seconds);
        CHECK(*report->deficit_improvement_seconds == Approx(1.0));
    }

    SECTION("deficit persists through the observation window") {
        player::RecoveryFreshnessObserver freshness;
        freshness.begin_recovery(anchor());
        const auto initial = freshness.observe(observation(13.0, 93.0, 97.0));
        REQUIRE(initial);
        CHECK(initial->classification ==
              player::RecoveryFreshnessClassification::Unverifiable);
        CHECK(initial->unverifiable_reason ==
              player::RecoveryFreshnessUnverifiableReason::InsufficientHistory);

        const auto report = freshness.observe(observation(18.0, 98.0, 102.0));
        REQUIRE(report);
        CHECK(report->classification == player::RecoveryFreshnessClassification::Stale);
        REQUIRE(report->comparable_for);
        CHECK(report->comparable_for->count() == Approx(5.0));
    }

    SECTION("a deficit that stops shrinking is no longer reported as converging") {
        player::RecoveryFreshnessObserver freshness;
        freshness.begin_recovery(anchor());
        REQUIRE(freshness.observe(observation(13.0, 93.0, 97.0)));
        const auto converging = freshness.observe(observation(14.0, 95.0, 99.0));
        REQUIRE(converging);
        REQUIRE(converging->classification ==
                player::RecoveryFreshnessClassification::Converging);

        const auto stationary = freshness.observe(observation(19.0, 100.0, 104.0));
        REQUIRE(stationary);
        CHECK(stationary->classification ==
              player::RecoveryFreshnessClassification::Stale);
    }
}

TEST_CASE("recovery freshness requires coherent playback and cache clocks") {
    player::RecoveryFreshnessObserver freshness;
    freshness.begin_recovery(anchor());

    const auto report = freshness.observe(observation(13.0, 3.0, 107.0));
    REQUIRE(report);
    CHECK(report->classification ==
          player::RecoveryFreshnessClassification::Unverifiable);
    CHECK(report->unverifiable_reason ==
          player::RecoveryFreshnessUnverifiableReason::ClockDomainUnclear);
}

TEST_CASE("recovery freshness keeps a coherent large clock shift separate from staleness") {
    player::RecoveryFreshnessObserver freshness;
    freshness.begin_recovery(anchor());

    const auto report = freshness.observe(observation(13.0, -897.0, -893.0));
    REQUIRE(report);
    CHECK(report->classification ==
          player::RecoveryFreshnessClassification::ClockRebased);
    REQUIRE(report->playback_deficit_seconds);
    REQUIRE(report->cache_end_deficit_seconds);
    CHECK(*report->playback_deficit_seconds == Approx(1000.0));
    CHECK(*report->cache_end_deficit_seconds == Approx(1000.0));
}

TEST_CASE("recovery freshness reports missing telemetry and rejects stale identities") {
    player::RecoveryFreshnessObserver freshness;
    freshness.begin_recovery(anchor());

    auto missing = observation(13.0, 103.0, 107.0);
    missing.cache_end_seconds.reset();
    const auto missing_report = freshness.observe(missing);
    REQUIRE(missing_report);
    CHECK(missing_report->classification ==
          player::RecoveryFreshnessClassification::Unverifiable);
    CHECK(missing_report->unverifiable_reason ==
          player::RecoveryFreshnessUnverifiableReason::MissingTelemetry);
    CHECK(missing_report->comparable_samples == 0);

    const auto stale = freshness.observe(
        observation(13.0, 103.0, 107.0, core::LoadAttempt{1}));
    REQUIRE(stale);
    CHECK(stale->unverifiable_reason ==
          player::RecoveryFreshnessUnverifiableReason::StaleIdentity);
    CHECK(stale->comparable_samples == 0);

    const auto current = freshness.observe(observation(13.0, 103.0, 107.0));
    REQUIRE(current);
    CHECK(current->first_readable);
    CHECK(current->comparable_samples == 1);

    freshness.reset();
    CHECK_FALSE(freshness.observe(observation(13.5, 103.5, 107.5)));
}

TEST_CASE("recovery freshness telemetry retains only closed identity and numeric deltas") {
    player::RecoveryFreshnessObserver freshness;
    freshness.begin_recovery(anchor());
    const auto report = freshness.observe(observation(13.0, 103.0, 107.0));
    REQUIRE(report);

    const std::string raw =
        "https://alice:password@provider.invalid/live/alice/password/42.ts?token=secret";
    const auto request = player::inspect_request_shape(
        raw, core::LoadIntent::RecoveryReopen, core::LoadAttempt{2},
        core::RecoveryTransport::MpegTs, false,
        {.provider_session = 3, .channel_session = 7});
    const auto retained =
        player::format_recovery_freshness_telemetry(*report, request);

    CHECK(retained.find("provider-session=3") != std::string::npos);
    CHECK(retained.find("channel-session=7") != std::string::npos);
    CHECK(retained.find("outgoing-load-attempt=1") != std::string::npos);
    CHECK(retained.find("recovered-load-attempt=2") != std::string::npos);
    CHECK(retained.find("classification=fresh") != std::string::npos);
    CHECK(retained.find("cache-end-deficit=+0.000s") != std::string::npos);
    CHECK(retained.find("policy=recovery-freshness-observability-v1") !=
          std::string::npos);
    for (const std::string_view forbidden : {
             "provider.invalid", "alice", "password", "token", "secret",
             "https://", "100.000", "104.000"}) {
        CHECK(retained.find(forbidden) == std::string::npos);
    }
}
