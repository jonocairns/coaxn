#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "player/delivery_telemetry.hpp"

using namespace coax;
using Catch::Approx;

namespace {

constexpr core::Generation kGeneration{3};
constexpr core::LoadAttempt kAttempt{1};

core::TimePoint at(double seconds) { return core::TimePoint{core::seconds(seconds)}; }

double seconds(core::Duration value) {
    return std::chrono::duration<double>(value).count();
}

player::DeliverySample sample(double observed, std::optional<double> cache_end,
                              std::optional<double> buffer = 4.0,
                              core::LoadAttempt attempt = kAttempt) {
    return {
        .generation = kGeneration,
        .load_attempt = attempt,
        .observed_at = at(observed),
        .cache_end_seconds = cache_end,
        .buffer_seconds = buffer,
    };
}

}  // namespace

TEST_CASE("first data is reported once, measured from load issue") {
    player::DeliveryTelemetry delivery;
    CHECK_FALSE(delivery.begin_load(kGeneration, kAttempt, at(10.0)));

    // Opening: nothing readable yet, then a cache end with no buffered media.
    CHECK_FALSE(delivery.observe(sample(10.5, std::nullopt, std::nullopt)));
    CHECK_FALSE(delivery.observe(sample(11.0, 0.0, 0.0)));

    const auto first = delivery.observe(sample(12.5, 1.2, 1.2));
    REQUIRE(first);
    CHECK(seconds(*first) == Approx(2.5));
    CHECK_FALSE(delivery.observe(sample(13.0, 1.8, 1.4)));
}

TEST_CASE("chunked delivery is summarised as the gaps between arrivals") {
    player::DeliveryTelemetry delivery;
    delivery.begin_load(kGeneration, kAttempt, at(0.0));
    delivery.observe(sample(0.5, 6.0));

    // A 6s chunk every 5s, then one late chunk after 11s of silence. Samples in
    // between see the cache end hold still.
    double cache_end = 6.0;
    double now = 0.5;
    for (const double gap : {5.0, 5.0, 5.0, 11.0}) {
        for (double step = 0.5; step < gap; step += 0.5) {
            delivery.observe(sample(now + step, cache_end));
        }
        now += gap;
        cache_end += 6.0;
        delivery.observe(sample(now, cache_end));
    }

    const auto summary = delivery.end_load();
    REQUIRE(summary);
    REQUIRE(summary->arrival_gaps);
    CHECK(summary->arrival_gaps->count == 4);
    CHECK(summary->arrival_gaps->p50_seconds == Approx(5.0));
    CHECK(summary->arrival_gaps->max_seconds == Approx(11.0));
    CHECK(summary->arrival_gaps->p99_seconds == Approx(11.0));
    CHECK(seconds(summary->observed_for) == Approx(26.5));
}

TEST_CASE("input silence runs from the last arrival and is absent before any data") {
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

TEST_CASE("a timestamp reset still counts as an arrival") {
    player::DeliveryTelemetry delivery;
    delivery.begin_load(kGeneration, kAttempt, at(0.0));
    delivery.observe(sample(0.5, 90.0));
    delivery.observe(sample(3.0, 2.0));

    const auto summary = delivery.end_load();
    REQUIRE(summary);
    REQUIRE(summary->arrival_gaps);
    CHECK(summary->arrival_gaps->count == 1);
    CHECK(summary->arrival_gaps->max_seconds == Approx(2.5));
}

TEST_CASE("samples from another load or out of order are ignored") {
    player::DeliveryTelemetry delivery;
    delivery.begin_load(kGeneration, kAttempt, at(0.0));
    delivery.observe(sample(1.0, 2.0));

    CHECK_FALSE(delivery.observe(sample(2.0, 50.0, 4.0, core::LoadAttempt{2})));
    delivery.observe(sample(0.5, 9.0));

    const auto summary = delivery.end_load();
    REQUIRE(summary);
    CHECK_FALSE(summary->arrival_gaps);
}

TEST_CASE("buffer depth range covers only samples after first data") {
    player::DeliveryTelemetry delivery;
    delivery.begin_load(kGeneration, kAttempt, at(0.0));
    delivery.observe(sample(0.5, std::nullopt, 0.0));
    delivery.observe(sample(1.0, 1.0, 1.0));
    delivery.observe(sample(1.5, 10.0, 9.5));
    delivery.observe(sample(2.0, 10.0, 0.3));

    const auto summary = delivery.end_load();
    REQUIRE(summary);
    CHECK(*summary->buffer_min_seconds == Approx(0.3));
    CHECK(*summary->buffer_max_seconds == Approx(9.5));
}

TEST_CASE("beginning a load hands back the previous load's summary") {
    player::DeliveryTelemetry delivery;
    delivery.begin_load(kGeneration, kAttempt, at(0.0));
    delivery.observe(sample(1.0, 2.0));

    const auto previous = delivery.begin_load(kGeneration, core::LoadAttempt{2}, at(5.0));
    REQUIRE(previous);
    CHECK(previous->load_attempt == kAttempt);

    // A load with no readable sample says nothing about delivery.
    CHECK_FALSE(delivery.end_load());
    CHECK_FALSE(delivery.end_load());
}

TEST_CASE("the summary line carries every decision field") {
    player::DeliveryTelemetry delivery;
    delivery.begin_load(kGeneration, kAttempt, at(0.0));
    delivery.observe(sample(3.0, 6.0));
    delivery.observe(sample(8.0, 12.0));

    const auto line = player::format_delivery_summary(*delivery.end_load());
    CHECK(line.find("Delivery summary generation=3 load-attempt=1") == 0);
    CHECK(line.find(" load-to-first-data=3000ms") != std::string::npos);
    CHECK(line.find(" arrivals=1 gap-p50=5.00s") != std::string::npos);
    CHECK(line.find(" gap-max=5.00s buffer-min=4.00s buffer-max=4.00s") != std::string::npos);
    CHECK(line.find(" schema=delivery-observability-v1") != std::string::npos);
}
