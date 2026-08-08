#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

#include "core/playback_health.hpp"

using namespace coax::core;
using Catch::Approx;

namespace {
constexpr double kContainerFps = 25.0;
const Duration kInterval = kDefaultHealthPolicy.sample_interval;

PlaybackHealthObservation healthy() {
    return {.av_sync_seconds = 0.0, .buffer_seconds = 8.0, .cache_end_seconds = 8.0,
            .cache_paused = false, .input_rate_bytes_per_second = 240'000.0,
            .ipc_round_trip_ms = 1.2, .playback_time_seconds = 0.0,
            .video_fps_estimate = kContainerFps};
}

std::vector<PlaybackHealthObservation> advancing(int count, double from = 0.0) {
    std::vector<PlaybackHealthObservation> result;
    for (int index = 0; index < count; ++index) {
        auto value = healthy();
        value.cache_end_seconds = from + 8.0 + (index + 1) * kInterval.count();
        value.playback_time_seconds = from + (index + 1) * kInterval.count();
        result.push_back(value);
    }
    return result;
}

std::vector<PlaybackHealthObservation> frozen(int count, double at) {
    std::vector<PlaybackHealthObservation> result;
    for (int index = 0; index < count; ++index) {
        auto value = healthy();
        value.buffer_seconds = 0.0;
        value.cache_end_seconds = at;
        value.playback_time_seconds = at;
        result.push_back(value);
    }
    return result;
}

std::vector<PlaybackHealthObservation> wedged(int count, double at) {
    std::vector<PlaybackHealthObservation> result;
    for (int index = 0; index < count; ++index) {
        auto value = healthy();
        value.buffer_seconds = 6.0;
        value.cache_end_seconds = at + 8.0 + (index + 1) * kInterval.count();
        value.playback_time_seconds = at;
        result.push_back(value);
    }
    return result;
}

struct ReplayResult {
    PlaybackHealthState state;
    std::vector<PlaybackHealthFold> folds;
};

ReplayResult replay(const std::vector<PlaybackHealthObservation>& values,
                    bool first_frame = true, TimePoint load_at = TimePoint{}) {
    auto state = initial_playback_health(BufferPhase::Zap, load_at);
    auto at = load_at;
    std::vector<PlaybackHealthFold> folds;
    for (const auto& value : values) {
        at += kInterval;
        PlaybackHealthFoldOptions options;
        options.container_fps = kContainerFps;
        options.first_frame_seen = first_frame;
        options.phase = BufferPhase::Zap;
        auto fold = fold_playback_health(state, value, at, options);
        state = fold.state;
        folds.push_back(fold);
    }
    return {state, folds};
}

void append(std::vector<PlaybackHealthObservation>& to,
            const std::vector<PlaybackHealthObservation>& from) {
    to.insert(to.end(), from.begin(), from.end());
}
}  // namespace

TEST_CASE("two phase buffer and health policy constants are pinned") {
    CHECK(buffer_phase_targets(BufferPhase::Zap).cache_seconds == 1.0);
    CHECK(buffer_phase_targets(BufferPhase::Zap).readahead_seconds == 1.0);
    CHECK(buffer_phase_targets(BufferPhase::Steady).cache_seconds == 10.0);
    CHECK(kDemuxerMaxBytes == 64U * 1024U * 1024U);
    CHECK(kDefaultHealthPolicy.stall_confirmation + 2 * kInterval < seconds(3.0));
}

TEST_CASE("advancing playback remains healthy despite a latched or zero byte rate") {
    auto values = advancing(6);
    for (auto& value : values) value.input_rate_bytes_per_second = 0.0;
    const auto result = replay(values);
    CHECK(result.state.verdict == PlaybackHealthVerdict::Healthy);
    CHECK(result.state.snapshot.progressing == true);
    CHECK_FALSE(result.state.snapshot.stalled_for);
}

TEST_CASE("a brief input interruption is absorbed while playback advances") {
    auto values = advancing(4);
    auto tail = advancing(6, 2.0);
    for (auto& value : tail) {
        value.buffer_seconds = 8.5;
        value.cache_end_seconds = 10.0;
        value.input_rate_bytes_per_second = 0.0;
    }
    append(values, tail);
    const auto result = replay(values);
    CHECK(result.state.verdict == PlaybackHealthVerdict::Healthy);
    for (const auto& fold : result.folds) CHECK_FALSE(fold.stalled);
}

TEST_CASE("stall needs progress buffer input duration and observation agreement") {
    auto values = advancing(4);
    append(values, frozen(6, 2.0));
    const auto result = replay(values);
    CHECK(result.state.verdict == PlaybackHealthVerdict::Stalled);
    const auto found = std::find_if(result.folds.begin(), result.folds.end(),
                                    [](const auto& fold) { return fold.stalled; });
    REQUIRE(found != result.folds.end());
    CHECK(found->state.observations >= kDefaultHealthPolicy.min_stall_observations);
    REQUIRE(found->state.snapshot.stalled_for);
    CHECK(*found->state.snapshot.stalled_for >= kDefaultHealthPolicy.stall_confirmation);
}

TEST_CASE("one stopped sample never confirms a stall") {
    auto values = advancing(4);
    append(values, frozen(1, 2.0));
    append(values, advancing(3, 2.0));
    for (const auto& fold : replay(values).folds) CHECK_FALSE(fold.stalled);
}

TEST_CASE("delivery advance makes a drained buffer a decode degradation") {
    auto values = advancing(4);
    auto arriving = frozen(8, 2.0);
    for (int index = 0; index < 8; ++index) {
        arriving[index].cache_end_seconds = 2.0 + (index + 1) * kInterval.count();
    }
    append(values, arriving);
    const auto result = replay(values);
    CHECK(result.state.verdict == PlaybackHealthVerdict::Degraded);
    for (const auto& fold : result.folds) CHECK_FALSE(fold.stalled);
}

TEST_CASE("missing cache properties or playback time cannot invent a stall") {
    auto cache_missing = advancing(4);
    auto tail = frozen(8, 2.0);
    for (auto& value : tail) { value.buffer_seconds.reset(); value.cache_end_seconds.reset(); }
    append(cache_missing, tail);
    auto result = replay(cache_missing);
    CHECK(result.state.verdict == PlaybackHealthVerdict::Degraded);
    for (const auto& fold : result.folds) CHECK_FALSE(fold.stalled);

    auto playback_missing = advancing(4);
    tail = frozen(8, 2.0);
    for (auto& value : tail) value.playback_time_seconds.reset();
    append(playback_missing, tail);
    result = replay(playback_missing);
    CHECK(result.state.verdict == PlaybackHealthVerdict::Unknown);
    for (const auto& fold : result.folds) CHECK_FALSE(fold.stalled);
}

TEST_CASE("an unreadable sample retains the frozen progress baseline") {
    auto values = advancing(4);
    append(values, frozen(2, 2.0));
    auto missing = frozen(1, 2.0);
    missing[0].playback_time_seconds.reset();
    append(values, missing);
    append(values, frozen(4, 2.0));
    const auto result = replay(values);
    CHECK(std::any_of(result.folds.begin(), result.folds.end(),
                      [](const auto& fold) { return fold.stalled; }));
}

TEST_CASE("cache pause is depleted even without buffer duration") {
    auto values = advancing(4);
    auto paused = frozen(6, 2.0);
    for (auto& value : paused) { value.buffer_seconds.reset(); value.cache_paused = true; }
    append(values, paused);
    CHECK(replay(values).state.verdict == PlaybackHealthVerdict::Stalled);
}

TEST_CASE("open stall runs from load issue and cannot arm after first frame") {
    PlaybackHealthObservation dead;
    dead.input_rate_bytes_per_second = 0.0;
    dead.ipc_round_trip_ms = 0.8;
    const int count = static_cast<int>(std::ceil(
        kDefaultHealthPolicy.open_stall_confirmation / kInterval)) + 2;
    const auto result = replay(std::vector<PlaybackHealthObservation>(count, dead), false);
    CHECK(std::any_of(result.folds.begin(), result.folds.end(),
                      [](const auto& fold) { return fold.stalled; }));
    CHECK(result.state.verdict == PlaybackHealthVerdict::OpenStalled);

    const auto played = replay(advancing(40), true);
    for (const auto& fold : played.folds) {
        CHECK(fold.state.verdict != PlaybackHealthVerdict::OpenStalled);
    }
}

TEST_CASE("an opening cache that keeps filling is not stalled") {
    std::vector<PlaybackHealthObservation> values;
    for (int index = 0; index < 40; ++index) {
        PlaybackHealthObservation value;
        value.buffer_seconds = 2.0;
        value.cache_end_seconds = (index + 1) * kInterval.count();
        value.input_rate_bytes_per_second = 400'000.0;
        value.ipc_round_trip_ms = 0.9;
        values.push_back(value);
    }
    for (const auto& fold : replay(values, false).folds) CHECK_FALSE(fold.stalled);
}

TEST_CASE("discontinuity is deviation from elapsed progress in either direction") {
    auto values = advancing(4);
    auto splice = healthy(); splice.cache_end_seconds = 3610.0; splice.playback_time_seconds = 3600.0;
    values.push_back(splice);
    splice.cache_end_seconds = 3610.5; splice.playback_time_seconds = 3600.5;
    values.push_back(splice);
    auto result = replay(values);
    CHECK(std::count_if(result.folds.begin(), result.folds.end(),
                        [](const auto& fold) { return fold.discontinuity; }) == 1);
    CHECK(result.state.discontinuities == 1);
    CHECK(result.state.verdict == PlaybackHealthVerdict::Healthy);

    values = advancing(4);
    auto backward = healthy(); backward.buffer_seconds = 6.0;
    backward.cache_end_seconds = 12.0; backward.playback_time_seconds = 0.5;
    values.push_back(backward);
    result = replay(values);
    CHECK(result.folds.back().discontinuity);
    CHECK(result.state.snapshot.degraded_reason == PlaybackDegradedReason::Discontinuity);

    values = advancing(4); append(values, wedged(3, 2.0));
    for (const auto& fold : replay(values).folds) CHECK_FALSE(fold.discontinuity);
}

TEST_CASE("degraded classification separates throttling decode damage and unknown fps") {
    auto values = advancing(4);
    std::vector<PlaybackHealthObservation> throttled;
    for (int index = 0; index < 6; ++index) {
        auto value = healthy(); value.buffer_seconds = 0.0;
        value.cache_end_seconds = 10.0 + (index + 1) * 0.2;
        value.playback_time_seconds = 2.0; throttled.push_back(value);
    }
    append(values, throttled);
    auto result = replay(values);
    CHECK(result.state.snapshot.degraded_reason == PlaybackDegradedReason::InputThrottled);
    REQUIRE(result.state.snapshot.input_realtime_ratio);
    CHECK(*result.state.snapshot.input_realtime_ratio < 1.0);

    values = advancing(4); auto damage = wedged(4, 2.0);
    for (auto& value : damage) value.video_fps_estimate = 18.9;
    append(values, damage); result = replay(values);
    CHECK(result.state.snapshot.degraded_reason == PlaybackDegradedReason::DecodeDamage);
    REQUIRE(result.state.snapshot.video_fps_shortfall);
    CHECK(*result.state.snapshot.video_fps_shortfall > kDefaultHealthPolicy.decode_shortfall_ratio);

    auto state = initial_playback_health(BufferPhase::Zap, TimePoint{});
    TimePoint at{};
    for (const auto& value : values) {
        at += kInterval;
        PlaybackHealthFoldOptions options;
        options.first_frame_seen = true;
        options.phase = BufferPhase::Zap;
        state = fold_playback_health(state, value, at, options).state;
    }
    CHECK_FALSE(state.snapshot.video_fps_shortfall);
    CHECK(state.snapshot.degraded_reason == PlaybackDegradedReason::Unclassified);
}

TEST_CASE("decode stall uses its long clock and excludes positive under-delivery") {
    const int count = static_cast<int>(std::ceil(
        kDefaultHealthPolicy.decode_stall_confirmation / kInterval)) + 2;
    auto values = advancing(4); auto damage = wedged(count, 2.0);
    for (auto& value : damage) value.video_fps_estimate = 18.9;
    append(values, damage);
    auto result = replay(values);
    CHECK(std::any_of(result.folds.begin(), result.folds.end(),
                      [](const auto& fold) { return fold.decode_stalled; }));
    for (const auto& fold : result.folds) CHECK_FALSE(fold.stalled);

    values = advancing(4);
    std::vector<PlaybackHealthObservation> slow;
    for (int index = 0; index < count + 8; ++index) {
        auto value = healthy(); value.buffer_seconds = 0.0;
        value.cache_end_seconds = 10.0 + (index + 1) * 0.025;
        value.playback_time_seconds = 2.0; slow.push_back(value);
    }
    append(values, slow); result = replay(values);
    CHECK(result.state.snapshot.input_realtime_ratio == Approx(0.05));
    CHECK(result.state.snapshot.degraded_reason == PlaybackDegradedReason::InputThrottled);
    for (const auto& fold : result.folds) { CHECK_FALSE(fold.decode_stalled); CHECK_FALSE(fold.stalled); }
}

TEST_CASE("full-buffer backpressure can confirm decode stall and resumed progress resets it") {
    const int count = static_cast<int>(std::ceil(
        kDefaultHealthPolicy.decode_stall_confirmation / kInterval)) + 2;
    auto values = advancing(4);
    std::vector<PlaybackHealthObservation> full;
    for (int index = 0; index < count; ++index) {
        auto value = healthy(); value.buffer_seconds = 10.0; value.cache_end_seconds = 10.0;
        value.playback_time_seconds = 2.0; value.video_fps_estimate = 18.9; full.push_back(value);
    }
    append(values, full); auto result = replay(values);
    CHECK(std::any_of(result.folds.begin(), result.folds.end(),
                      [](const auto& fold) { return fold.decode_stalled; }));

    const int half = static_cast<int>(std::ceil(
        kDefaultHealthPolicy.decode_stall_confirmation / kInterval / 2.0));
    values = advancing(4); auto first = wedged(half, 2.0);
    for (auto& value : first) value.video_fps_estimate = 18.9;
    append(values, first); append(values, advancing(4, 2.0));
    auto second = wedged(half, 4.0); for (auto& value : second) value.video_fps_estimate = 18.9;
    append(values, second); result = replay(values);
    for (const auto& fold : result.folds) CHECK_FALSE(fold.decode_stalled);
    CHECK(result.state.decode_since.has_value());
}
