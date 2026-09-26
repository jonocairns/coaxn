#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "player/delivery_telemetry.hpp"

using namespace coax;
using Catch::Approx;

namespace {

constexpr core::Generation kGeneration{3};
constexpr core::LoadAttempt kAttempt{1};
constexpr double kBin = player::DeliveryTelemetry::kGapBinSeconds;

core::TimePoint at(double seconds) { return core::TimePoint{core::seconds(seconds)}; }

double seconds(core::Duration value) {
    return std::chrono::duration<double>(value).count();
}

player::DeliverySample sample(double observed, std::optional<double> cache_end,
                              std::optional<double> buffer = 4.0,
                              std::optional<double> target = 20.0,
                              core::LoadAttempt attempt = kAttempt) {
    return {
        .generation = kGeneration,
        .load_attempt = attempt,
        .observed_at = at(observed),
        .cache_end_seconds = cache_end,
        .buffer_seconds = buffer,
        .buffer_target_seconds = target,
    };
}

// The cache end held still at every half-second sample from `from` to `to`.
void hold(player::DeliveryTelemetry& delivery, double from, double to, double cache_end,
          double buffer = 4.0) {
    for (double now = from; now <= to + 1e-9; now += 0.5) {
        delivery.observe(sample(now, cache_end, buffer));
    }
}

// Chunks arrive at the given times, each moving the cache end by six seconds,
// with the cache end held still at every half-second sample in between.
void chunked(player::DeliveryTelemetry& delivery, std::initializer_list<double> gaps,
             double start = 0.5, double buffer = 4.0) {
    double cache_end = 6.0;
    double now = start;
    delivery.observe(sample(now, cache_end, buffer));
    for (const double gap : gaps) {
        for (double step = 0.5; step < gap; step += 0.5) {
            delivery.observe(sample(now + step, cache_end, buffer));
        }
        now += gap;
        cache_end += 6.0;
        delivery.observe(sample(now, cache_end, buffer));
    }
}

}  // namespace

TEST_CASE("first data is reported once, measured from load issue") {
    player::DeliveryTelemetry delivery;
    CHECK_FALSE(delivery.begin_load(kGeneration, kAttempt, at(10.0)));

    // Opening: nothing readable, then a cache end with no buffered media.
    CHECK_FALSE(delivery.observe(sample(10.5, std::nullopt, std::nullopt)));
    CHECK_FALSE(delivery.observe(sample(11.0, 0.0, 0.0)));

    const auto first = delivery.observe(sample(12.5, 1.2, 1.2));
    REQUIRE(first);
    CHECK(seconds(*first) == Approx(2.5));
    CHECK_FALSE(delivery.observe(sample(13.0, 1.8, 1.4)));
}

TEST_CASE("chunked delivery is summarised as the silences between movements") {
    player::DeliveryTelemetry delivery;
    delivery.begin_load(kGeneration, kAttempt, at(0.0));
    chunked(delivery, {5.0, 5.0, 5.0, 11.0});

    const auto summary = delivery.end_load(at(0.0));
    REQUIRE(summary);
    CHECK(summary->kind == player::DeliveryReportKind::Final);
    REQUIRE(summary->source_gaps);
    CHECK(summary->source_gaps->count == 4);
    CHECK(summary->source_gaps->p50_seconds == Approx(5.0).margin(kBin));
    CHECK(summary->source_gaps->p99_seconds == Approx(11.0).margin(kBin));
    CHECK(summary->source_gaps->max_seconds == Approx(11.0));
    CHECK(seconds(summary->observed_for) == Approx(26.5));
    CHECK(summary->throttled_gaps == 0);
}

TEST_CASE("silences with the buffer held at its target are counted apart") {
    player::DeliveryTelemetry delivery;
    delivery.begin_load(kGeneration, kAttempt, at(0.0));
    // A full 20s buffer throughout: mpv's own throttling explains the quiet.
    chunked(delivery, {5.0, 7.0}, 0.5, 19.5);

    const auto summary = delivery.end_load(at(0.0));
    REQUIRE(summary);
    CHECK_FALSE(summary->source_gaps);
    CHECK(summary->throttled_gaps == 2);
    CHECK_FALSE(summary->silent_at_end);
}

TEST_CASE("a silence is charged to the source from when the buffer leaves its target") {
    player::DeliveryTelemetry delivery;
    delivery.begin_load(kGeneration, kAttempt, at(0.0));
    delivery.observe(sample(0.5, 6.0, 19.6));
    delivery.observe(sample(1.0, 6.0, 19.1));
    // Below target from here: mpv would read anything that arrived.
    delivery.observe(sample(1.5, 6.0, 18.6));
    hold(delivery, 2.0, 6.0, 6.0, 17.0);
    delivery.observe(sample(6.5, 12.0, 19.5));

    const auto summary = delivery.end_load(at(0.0));
    REQUIRE(summary);
    CHECK(summary->throttled_gaps == 0);
    REQUIRE(summary->source_gaps);
    CHECK(summary->source_gaps->max_seconds == Approx(5.0));
    // The raw demuxer view still runs from the last movement.
    constexpr core::LoadAttempt next{2};
    delivery.begin_load(kGeneration, next, at(0.0));
    delivery.observe(sample(0.5, 6.0, 19.6, 20.0, next));
    delivery.observe(sample(2.0, 6.0, 17.0, 20.0, next));
    CHECK(seconds(*delivery.input_silence(at(4.0))) == Approx(3.5));
    CHECK(seconds(*delivery.snapshot(at(4.0))->silent_at_end) == Approx(2.0));
}

TEST_CASE("a silence still running when the load ends is reported as censored") {
    player::DeliveryTelemetry delivery;
    delivery.begin_load(kGeneration, kAttempt, at(0.0));
    chunked(delivery, {5.0});
    for (double now = 6.0; now <= 30.0; now += 0.5) delivery.observe(sample(now, 12.0));

    const auto summary = delivery.end_load(at(0.0));
    REQUIRE(summary);
    REQUIRE(summary->silent_at_end);
    CHECK(seconds(*summary->silent_at_end) == Approx(24.5));
    // Only the completed gap enters the percentiles.
    CHECK(summary->source_gaps->count == 1);
    CHECK(summary->source_gaps->max_seconds == Approx(5.0));
}

TEST_CASE("a cache end moving at every sample is delivery, not a silence") {
    for (const double buffer : {4.0, 19.8}) {
        player::DeliveryTelemetry delivery;
        delivery.begin_load(kGeneration, kAttempt, at(0.0));
        // A continuous stream: the cache end advances between every sample.
        for (double now = 0.5; now <= 60.0; now += 0.5) {
            delivery.observe(sample(now, now, buffer));
        }

        const auto summary = delivery.end_load(at(60.0));
        REQUIRE(summary);
        CHECK_FALSE(summary->source_gaps);
        CHECK(summary->throttled_gaps == 0);
    }
}

TEST_CASE("a pause in sampling is not charged to the source") {
    player::DeliveryTelemetry delivery;
    delivery.begin_load(kGeneration, kAttempt, at(0.0));
    // Continuous delivery, then 8s with no samples -- a window drag holding the
    // loop -- while mpv went on reading.
    for (double now = 0.5; now <= 10.0; now += 0.5) delivery.observe(sample(now, now));
    for (double now = 18.0; now <= 20.0; now += 0.5) delivery.observe(sample(now, now));
    auto summary = delivery.end_load(at(20.0));
    REQUIRE(summary);
    CHECK_FALSE(summary->source_gaps);
    CHECK(summary->sampling_pauses == 1);

    // A silence that ends inside a pause is known only to the sample before it.
    delivery.begin_load(kGeneration, kAttempt, at(0.0));
    delivery.observe(sample(0.5, 6.0));
    hold(delivery, 1.0, 5.0, 6.0);
    delivery.observe(sample(13.0, 12.0));
    // One the cache end stayed still across is still the source's in full.
    hold(delivery, 13.5, 14.0, 12.0);
    delivery.observe(sample(22.0, 12.0));
    delivery.observe(sample(22.5, 18.0));
    summary = delivery.end_load(at(23.0));
    REQUIRE(summary->source_gaps);
    CHECK(summary->source_gaps->count == 2);
    CHECK(summary->source_gaps->p50_seconds == Approx(4.5).margin(kBin));
    CHECK(summary->source_gaps->max_seconds == Approx(9.5));
    CHECK(summary->sampling_pauses == 2);
}

TEST_CASE("input silence runs from the last movement and is absent before any data") {
    player::DeliveryTelemetry delivery;
    delivery.begin_load(kGeneration, kAttempt, at(0.0));
    CHECK_FALSE(delivery.input_silence(at(1.0)));

    delivery.observe(sample(1.0, 2.0));
    delivery.observe(sample(1.5, 2.0));
    delivery.observe(sample(2.0, 2.0));
    REQUIRE(delivery.input_silence(at(4.0)));
    CHECK(seconds(*delivery.input_silence(at(4.0))) == Approx(3.0));

    delivery.observe(sample(4.5, 8.0));
    CHECK(seconds(*delivery.input_silence(at(4.5))) == Approx(0.0));
}

TEST_CASE("a timestamp reset is counted without closing or opening a silence") {
    player::DeliveryTelemetry delivery;
    delivery.begin_load(kGeneration, kAttempt, at(0.0));
    delivery.observe(sample(0.5, 90.0));
    hold(delivery, 1.0, 2.5, 90.0);
    delivery.observe(sample(3.0, 2.0));
    CHECK(seconds(*delivery.input_silence(at(3.0))) == Approx(2.5));
    hold(delivery, 3.5, 4.5, 2.0);
    delivery.observe(sample(5.0, 8.0));

    const auto summary = delivery.end_load(at(0.0));
    REQUIRE(summary);
    CHECK(summary->timestamp_resets == 1);
    REQUIRE(summary->source_gaps);
    CHECK(summary->source_gaps->count == 1);
    CHECK(summary->source_gaps->max_seconds == Approx(4.5));
}

TEST_CASE("unreadable samples are counted rather than read as silence ending") {
    player::DeliveryTelemetry delivery;
    delivery.begin_load(kGeneration, kAttempt, at(0.0));
    // Opening samples have no cache end yet; that is normal, not missing.
    delivery.observe(sample(0.2, std::nullopt, std::nullopt));
    delivery.observe(sample(0.5, 2.0));
    delivery.observe(sample(1.0, std::nullopt));
    delivery.observe(sample(1.5, std::nullopt));

    const auto summary = delivery.end_load(at(0.0));
    REQUIRE(summary);
    CHECK(summary->missing_samples == 2);
    CHECK_FALSE(summary->source_gaps);
}

TEST_CASE("samples from another load or out of order are ignored") {
    player::DeliveryTelemetry delivery;
    delivery.begin_load(kGeneration, kAttempt, at(0.0));
    delivery.observe(sample(1.0, 2.0));

    CHECK_FALSE(delivery.observe(sample(2.0, 50.0, 4.0, 20.0, core::LoadAttempt{2})));
    delivery.observe(sample(0.5, 9.0));

    const auto summary = delivery.end_load(at(0.0));
    REQUIRE(summary);
    CHECK_FALSE(summary->source_gaps);
}

TEST_CASE("gaps beyond the binned range keep an exact maximum") {
    player::DeliveryTelemetry delivery;
    delivery.begin_load(kGeneration, kAttempt, at(0.0));
    delivery.observe(sample(0.5, 1.0));
    hold(delivery, 1.0, 200.0, 1.0);
    delivery.observe(sample(200.5, 7.0));

    const auto summary = delivery.end_load(at(0.0));
    REQUIRE(summary->source_gaps);
    CHECK(summary->source_gaps->max_seconds == Approx(200.0));
    CHECK(summary->source_gaps->p50_seconds == Approx(200.0));
}

TEST_CASE("percentiles among gaps past the binned range stay exact") {
    player::DeliveryTelemetry delivery;
    delivery.begin_load(kGeneration, kAttempt, at(0.0));
    double now = 0.5;
    double cache_end = 1.0;
    delivery.observe(sample(now, cache_end));
    for (const double gap : {130.0, 140.0, 150.0, 160.0, 500.0}) {
        hold(delivery, now + 0.5, now + gap - 0.5, cache_end);
        now += gap;
        cache_end += 6.0;
        delivery.observe(sample(now, cache_end));
    }

    const auto summary = delivery.end_load(at(0.0));
    REQUIRE(summary->source_gaps);
    CHECK(summary->source_gaps->p50_seconds == Approx(150.0));
    CHECK(summary->source_gaps->p90_seconds == Approx(500.0));
    CHECK(summary->source_gaps->max_seconds == Approx(500.0));
}

TEST_CASE("a final report measures a running silence to when the load ended") {
    player::DeliveryTelemetry delivery;
    delivery.begin_load(kGeneration, kAttempt, at(0.0));
    chunked(delivery, {5.0});

    // No sample after 5.5s -- supervision stopped -- but the load ends at 20s.
    const auto summary = delivery.end_load(at(20.0));
    REQUIRE(summary->silent_at_end);
    CHECK(seconds(*summary->silent_at_end) == Approx(14.5));
    CHECK(seconds(summary->observed_for) == Approx(20.0));
}

TEST_CASE("a suspended load stops measuring silence where observation stopped") {
    player::DeliveryTelemetry delivery;
    delivery.begin_load(kGeneration, kAttempt, at(0.0));
    chunked(delivery, {5.0});
    delivery.suspend(at(15.0));

    // Left failed for minutes, then stopped: only the watched 9.5s counts.
    CHECK(seconds(*delivery.snapshot(at(200.0))->silent_at_end) == Approx(9.5));
    const auto summary = delivery.end_load(at(300.0));
    REQUIRE(summary->silent_at_end);
    CHECK(seconds(*summary->silent_at_end) == Approx(9.5));
    CHECK(seconds(summary->observed_for) == Approx(15.0));
}

TEST_CASE("reviving a suspended load resumes it without spanning the gap") {
    player::DeliveryTelemetry delivery;
    delivery.begin_load(kGeneration, kAttempt, at(0.0));
    chunked(delivery, {5.0});
    delivery.suspend(at(15.0));

    CHECK_FALSE(delivery.begin_load(kGeneration, kAttempt, at(100.0)));
    hold(delivery, 101.0, 102.5, 12.0);
    delivery.observe(sample(103.0, 18.0));

    const auto summary = delivery.end_load(at(104.0));
    REQUIRE(summary->source_gaps);
    // 5s before the failure, then 3s from the revival -- not 97.5s across it.
    CHECK(summary->source_gaps->count == 2);
    CHECK(summary->source_gaps->max_seconds == Approx(5.0));
    CHECK(seconds(*summary->silent_at_end) == Approx(1.0));
    // 15s watched before the failure plus 4s after the revival.
    CHECK(seconds(summary->observed_for) == Approx(19.0));
}

TEST_CASE("input silence is frozen while suspended and restarts on revival") {
    player::DeliveryTelemetry delivery;
    delivery.begin_load(kGeneration, kAttempt, at(0.0));
    chunked(delivery, {5.0});
    delivery.suspend(at(15.0));
    CHECK(seconds(*delivery.input_silence(at(90.0))) == Approx(9.5));

    delivery.begin_load(kGeneration, kAttempt, at(100.0));
    CHECK(seconds(*delivery.input_silence(at(102.0))) == Approx(2.0));
}

TEST_CASE("the at-target band scales down for the one-second zap target") {
    player::DeliveryTelemetry delivery;
    delivery.begin_load(kGeneration, kAttempt, at(0.0));
    // A 1s target with 0.3s buffered: mpv would read, so the quiet is the source's.
    delivery.observe(sample(0.5, 1.0, 0.3, 1.0));
    delivery.observe(sample(3.0, 1.0, 0.1, 1.0));
    delivery.observe(sample(3.5, 2.0, 0.9, 1.0));

    const auto summary = delivery.end_load(at(0.0));
    CHECK(summary->throttled_gaps == 0);
    REQUIRE(summary->source_gaps);
    CHECK(summary->source_gaps->max_seconds == Approx(3.0));
}

TEST_CASE("buffer depth range covers only samples after first data") {
    player::DeliveryTelemetry delivery;
    delivery.begin_load(kGeneration, kAttempt, at(0.0));
    delivery.observe(sample(0.5, std::nullopt, 0.0));
    delivery.observe(sample(1.0, 1.0, 1.0));
    delivery.observe(sample(1.5, 10.0, 9.5));
    delivery.observe(sample(2.0, 10.0, 0.3));

    const auto summary = delivery.end_load(at(0.0));
    REQUIRE(summary);
    CHECK(*summary->buffer_min_seconds == Approx(0.3));
    CHECK(*summary->buffer_max_seconds == Approx(9.5));
}

TEST_CASE("a snapshot reports the load so far without ending it") {
    player::DeliveryTelemetry delivery;
    CHECK_FALSE(delivery.snapshot(at(0.0)));
    delivery.begin_load(kGeneration, kAttempt, at(0.0));
    CHECK_FALSE(delivery.snapshot(at(0.2)));
    chunked(delivery, {5.0});

    const auto snapshot = delivery.snapshot(at(8.0));
    REQUIRE(snapshot);
    CHECK(snapshot->kind == player::DeliveryReportKind::Snapshot);
    CHECK(seconds(snapshot->observed_for) == Approx(8.0));
    CHECK(seconds(*snapshot->silent_at_end) == Approx(2.5));

    // The load carries on and its final summary includes later gaps.
    hold(delivery, 6.0, 10.0, 12.0);
    delivery.observe(sample(10.5, 18.0));
    const auto summary = delivery.end_load(at(0.0));
    REQUIRE(summary);
    CHECK(summary->source_gaps->count == 2);
}

TEST_CASE("beginning the same load again continues its record") {
    player::DeliveryTelemetry delivery;
    delivery.begin_load(kGeneration, kAttempt, at(0.0));
    delivery.observe(sample(1.0, 2.0));
    hold(delivery, 1.5, 3.0, 2.0);

    // A late revival restarts supervision for the same physical load.
    CHECK_FALSE(delivery.begin_load(kGeneration, kAttempt, at(3.0)));
    hold(delivery, 3.5, 4.5, 2.0);
    delivery.observe(sample(5.0, 8.0));

    const auto replaced = delivery.begin_load(kGeneration, core::LoadAttempt{2}, at(50.0));
    REQUIRE(replaced);
    CHECK(replaced->load_attempt == kAttempt);
    CHECK(seconds(*replaced->load_to_first_data) == Approx(1.0));
    CHECK(replaced->source_gaps->max_seconds == Approx(4.0));

    // A load with no readable sample says nothing about delivery.
    CHECK_FALSE(delivery.end_load(at(0.0)));
    CHECK_FALSE(delivery.end_load(at(0.0)));
}

TEST_CASE("the report line carries every decision field") {
    player::DeliveryTelemetry delivery;
    delivery.begin_load(kGeneration, kAttempt, at(0.0));
    delivery.observe(sample(3.0, 6.0));
    hold(delivery, 3.5, 7.5, 6.0);
    delivery.observe(sample(8.0, 12.0));

    const auto line = player::format_delivery_summary(*delivery.end_load(at(0.0)));
    CHECK(line.find("Delivery summary generation=3 load-attempt=1") == 0);
    CHECK(line.find(" load-to-first-data=3000ms") != std::string::npos);
    CHECK(line.find(" source-gaps=1 gap-p50=5.00s") != std::string::npos);
    CHECK(line.find(" gap-max=5.00s throttled-gaps=0 silent-at-end=0ms") != std::string::npos);
    CHECK(line.find(" timestamp-resets=0 missing-samples=0 sampling-pauses=0") !=
          std::string::npos);
    CHECK(line.find(" buffer-min=4.00s buffer-max=4.00s") != std::string::npos);
    CHECK(line.find(" schema=delivery-observability-v2") != std::string::npos);

    delivery.begin_load(kGeneration, kAttempt, at(0.0));
    delivery.observe(sample(1.0, 2.0));
    CHECK(player::format_delivery_summary(*delivery.snapshot(at(2.0)))
              .find("Delivery snapshot generation=3") == 0);
}
