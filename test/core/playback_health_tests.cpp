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
    return {.generation = Generation{1}, .load_attempt = LoadAttempt{1},
            .av_sync_seconds = 0.0,
            .buffer_seconds = 8.0, .cache_end_seconds = 8.0,
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

std::vector<PlaybackHealthObservation> cache_paused(int count, double at) {
    auto result = frozen(count, at);
    for (auto& value : result) {
        value.buffer_seconds.reset();
        value.cache_paused = true;
    }
    return result;
}

struct ReplayResult {
    PlaybackHealthState state;
    std::vector<PlaybackHealthFold> folds;
};

ReplayResult replay(const std::vector<PlaybackHealthObservation>& values,
                    bool first_frame = true, TimePoint load_at = TimePoint{}) {
    auto state = initial_playback_health(Generation{1}, LoadAttempt{1}, BufferPhase::Zap, load_at);
    auto at = load_at;
    std::vector<PlaybackHealthFold> folds;
    for (auto value : values) {
        // Fixtures that exercise unavailable opening telemetry use default
        // observations; the helper attaches them to its current load.
        if (value.generation == Generation{}) value.generation = Generation{1};
        if (value.load_attempt == LoadAttempt{}) value.load_attempt = LoadAttempt{1};
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
    CHECK(kDefaultHealthPolicy.cache_pause_grace == seconds(10.0));
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

TEST_CASE("confirmed unpaused no progress raises the progress-stall signal") {
    auto values = advancing(4);
    append(values, frozen(6, 2.0));
    const auto result = replay(values);
    CHECK(result.state.verdict == PlaybackHealthVerdict::Stalled);
    const auto found = std::find_if(result.folds.begin(), result.folds.end(),
                                    [](const auto& fold) { return fold.stalled; });
    REQUIRE(found != result.folds.end());
    CHECK_FALSE(found->state.snapshot.cache_paused);
    CHECK(found->state.observations >= kDefaultHealthPolicy.min_stall_observations);
    REQUIRE(found->state.snapshot.stalled_for);
    CHECK(*found->state.snapshot.stalled_for >= kDefaultHealthPolicy.stall_confirmation);
}

TEST_CASE("isolated backward timestamp movement does not raise a recovery signal") {
    auto values = advancing(4);
    auto backward = healthy();
    backward.buffer_seconds = 6.0;
    backward.cache_end_seconds = 10.5;
    backward.playback_time_seconds = 1.0;
    values.push_back(backward);
    append(values, advancing(4, 2.0));

    const auto result = replay(values);
    CHECK(result.state.verdict == PlaybackHealthVerdict::Healthy);
    CHECK(std::any_of(result.folds.begin(), result.folds.end(),
                      [](const auto& fold) { return fold.discontinuity; }));
    for (const auto& fold : result.folds) {
        CHECK_FALSE(fold.stalled);
        CHECK_FALSE(fold.decode_stalled);
    }
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

TEST_CASE("brief cache pause does not confirm either recovery clock") {
    auto values = advancing(4);
    // The soak's longest ordinary fill was six seconds, so pin that observed
    // boundary below the independently confirmed ten-second grace.
    append(values, cache_paused(
        static_cast<int>(seconds(6.0) / kInterval), 2.0));
    const auto result = replay(values);
    CHECK(result.state.verdict == PlaybackHealthVerdict::Degraded);
    CHECK(result.state.snapshot.degraded_reason == PlaybackDegradedReason::CacheBuffering);
    for (const auto& fold : result.folds) {
        CHECK_FALSE(fold.stalled);
        CHECK_FALSE(fold.decode_stalled);
    }
}

TEST_CASE("sustained cache pause without progress raises a distinct cache stall") {
    auto values = advancing(4);
    const int count = static_cast<int>(std::ceil(
        kDefaultHealthPolicy.cache_pause_grace / kInterval)) + 2;
    append(values, cache_paused(count, 2.0));

    const auto result = replay(values);
    CHECK(result.state.verdict == PlaybackHealthVerdict::CacheStalled);
    CHECK(result.state.snapshot.degraded_reason == PlaybackDegradedReason::CacheBuffering);
    const auto found = std::find_if(result.folds.begin(), result.folds.end(),
                                    [](const auto& fold) { return fold.cache_stalled; });
    REQUIRE(found != result.folds.end());
    CHECK(found->stalled);
    CHECK(found->state.cache_pause_observations >=
          kDefaultHealthPolicy.min_cache_stall_observations);
    REQUIRE(found->state.snapshot.stalled_for);
    CHECK(*found->state.snapshot.stalled_for >=
          kDefaultHealthPolicy.cache_pause_grace);
    for (const auto& fold : result.folds) CHECK_FALSE(fold.decode_stalled);
}

TEST_CASE("cache stall grace resets on progress and pause exit") {
    const int partial = static_cast<int>(
        kDefaultHealthPolicy.cache_pause_grace / kInterval) - 4;
    auto values = advancing(4);
    append(values, cache_paused(partial, 2.0));

    auto progress_while_paused = healthy();
    progress_while_paused.buffer_seconds.reset();
    progress_while_paused.cache_end_seconds = 10.5;
    progress_while_paused.cache_paused = true;
    progress_while_paused.playback_time_seconds = 2.5;
    values.push_back(progress_while_paused);
    append(values, cache_paused(partial, 2.5));

    // Leaving the pause also clears the dedicated clock, even if this one
    // unpaused sample has not yet moved enough to be healthy.
    auto unpaused = healthy();
    unpaused.buffer_seconds = 0.0;
    unpaused.cache_end_seconds = 2.5;
    unpaused.playback_time_seconds = 2.5;
    values.push_back(unpaused);
    append(values, cache_paused(partial, 2.5));

    const auto result = replay(values);
    for (const auto& fold : result.folds) CHECK_FALSE(fold.cache_stalled);
}

TEST_CASE("health observations are scoped to both generation and load attempt") {
    const auto started = initial_playback_health(
        Generation{9}, LoadAttempt{2}, BufferPhase::Zap, TimePoint{});
    auto observation = healthy();
    observation.generation = Generation{9};
    observation.load_attempt = LoadAttempt{1};

    PlaybackHealthFoldOptions options;
    const auto stale = fold_playback_health(
        started, observation, TimePoint{} + kInterval, options);
    CHECK_FALSE(stale.observation_accepted);
    CHECK(stale.state.load_attempt == LoadAttempt{2});
    CHECK(stale.state.observations == 0);
}

TEST_CASE("first-frame deadline runs from load issue and cannot arm after first frame") {
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

TEST_CASE("opening input and stale playback time cannot outlive the first-frame deadline") {
    std::vector<PlaybackHealthObservation> values;
    for (int index = 0; index < 40; ++index) {
        PlaybackHealthObservation value;
        value.buffer_seconds = 2.0;
        value.cache_end_seconds = (index + 1) * kInterval.count();
        value.input_rate_bytes_per_second = 400'000.0;
        value.ipc_round_trip_ms = 0.9;
        value.playback_time_seconds = 125.0;
        values.push_back(value);
    }
    const auto result = replay(values, false);
    CHECK(result.state.verdict == PlaybackHealthVerdict::OpenStalled);
    CHECK(result.state.snapshot.input_advancing == true);
    CHECK(result.state.snapshot.progressing == false);
    const auto stalled = std::find_if(result.folds.begin(), result.folds.end(),
                                      [](const auto& fold) { return fold.stalled; });
    REQUIRE(stalled != result.folds.end());
    const auto due_index = static_cast<std::size_t>(std::ceil(
        kDefaultHealthPolicy.open_stall_confirmation / kInterval)) - 1;
    CHECK(static_cast<std::size_t>(std::distance(result.folds.begin(), stalled)) ==
          due_index);
    for (auto fold = result.folds.begin(); fold != stalled; ++fold) {
        CHECK_FALSE(fold->stalled);
    }

    // App drains FirstFrame before sampling health. If the frame arrives on
    // the exact due turn, that phase change wins and no open stall is emitted.
    auto state = initial_playback_health(
        Generation{1}, LoadAttempt{1}, BufferPhase::Zap, TimePoint{});
    PlaybackHealthFoldOptions options;
    for (std::size_t index = 0; index < due_index; ++index) {
        auto opening_value = values[index];
        opening_value.generation = Generation{1};
        opening_value.load_attempt = LoadAttempt{1};
        state = fold_playback_health(
            state, opening_value,
            TimePoint{} + kInterval * static_cast<double>(index + 1),
            options).state;
    }
    options.first_frame_seen = true;
    auto first_frame_value = values[due_index];
    first_frame_value.generation = Generation{1};
    first_frame_value.load_attempt = LoadAttempt{1};
    first_frame_value.playback_time_seconds = 125.0 + kInterval.count();
    const auto frame_won = fold_playback_health(
        state, first_frame_value,
        TimePoint{} + kDefaultHealthPolicy.open_stall_confirmation,
        options);
    CHECK_FALSE(frame_won.stalled);
    CHECK(frame_won.state.verdict == PlaybackHealthVerdict::Healthy);
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

TEST_CASE("timeline evidence preserves signed movement and deviation") {
    const auto ordinary = replay(advancing(3));
    const auto& normal = ordinary.state.snapshot.timeline;
    REQUIRE(normal.elapsed_seconds);
    REQUIRE(normal.playback_movement_seconds);
    REQUIRE(normal.playback_deviation_seconds);
    CHECK(*normal.elapsed_seconds == Approx(kInterval.count()));
    CHECK(*normal.playback_movement_seconds == Approx(kInterval.count()));
    CHECK(*normal.playback_deviation_seconds == Approx(0.0).margin(1e-9));

    auto first = healthy();
    first.playback_time_seconds = 10.0;
    first.cache_end_seconds = 20.0;
    auto forward = first;
    forward.playback_time_seconds = 13.0;
    forward.cache_end_seconds = 23.0;
    auto result = replay({first, forward});
    REQUIRE(result.state.snapshot.timeline.playback_deviation_seconds);
    CHECK(*result.state.snapshot.timeline.playback_deviation_seconds > 0.0);

    auto backward = first;
    backward.playback_time_seconds = 8.0;
    backward.cache_end_seconds = 18.0;
    result = replay({first, backward});
    REQUIRE(result.state.snapshot.timeline.playback_movement_seconds);
    REQUIRE(result.state.snapshot.timeline.playback_deviation_seconds);
    REQUIRE(result.state.snapshot.timeline.cache_end_movement_seconds);
    // Falsification: flattening either delta with abs() makes these fail.
    CHECK(*result.state.snapshot.timeline.playback_movement_seconds == Approx(-2.0));
    CHECK(*result.state.snapshot.timeline.playback_deviation_seconds == Approx(-2.5));
    CHECK(*result.state.snapshot.timeline.cache_end_movement_seconds == Approx(-2.0));
    // The nonnegative control ratio is still separate from the raw signed delta.
    CHECK(result.state.snapshot.input_realtime_ratio == Approx(0.0));
}

TEST_CASE("stopped playback remains distinct from backward playback") {
    auto first = healthy();
    first.playback_time_seconds = 10.0;
    auto stopped = first;
    auto backward = first;
    backward.playback_time_seconds = 9.5;

    const auto stopped_result = replay({first, stopped});
    const auto backward_result = replay({first, backward});
    REQUIRE(stopped_result.state.snapshot.timeline.playback_movement_seconds);
    REQUIRE(backward_result.state.snapshot.timeline.playback_movement_seconds);
    CHECK(*stopped_result.state.snapshot.timeline.playback_movement_seconds == Approx(0.0));
    CHECK(*backward_result.state.snapshot.timeline.playback_movement_seconds == Approx(-0.5));
}

TEST_CASE("missing timeline telemetry stays unavailable instead of becoming zero") {
    auto present = healthy();
    present.playback_time_seconds = 10.0;
    present.cache_end_seconds = 20.0;
    auto missing = present;
    missing.playback_time_seconds.reset();
    missing.cache_end_seconds.reset();
    auto result = replay({present, missing});
    CHECK_FALSE(result.state.snapshot.timeline.playback_movement_seconds);
    CHECK_FALSE(result.state.snapshot.timeline.playback_deviation_seconds);
    CHECK_FALSE(result.state.snapshot.timeline.cache_end_movement_seconds);

    auto resumed = present;
    resumed.playback_time_seconds = 12.0;
    result = replay({present, missing, resumed});
    CHECK_FALSE(result.state.snapshot.timeline.playback_movement_seconds);
    REQUIRE(result.state.snapshot.timeline.control_playback_movement_seconds);
    REQUIRE(result.state.snapshot.timeline.control_playback_deviation_seconds);
    CHECK(*result.state.snapshot.timeline.control_playback_movement_seconds == Approx(2.0));
    CHECK(*result.state.snapshot.timeline.control_playback_deviation_seconds == Approx(1.5));
    CHECK(result.state.snapshot.timeline.control_baseline_retained);
    CHECK(result.folds.back().discontinuity);

    result = replay({missing, present});
    // Falsification: converting either absent endpoint to 0.0 makes these
    // available and manufactures very large signed movements.
    CHECK_FALSE(result.state.snapshot.timeline.playback_movement_seconds);
    CHECK_FALSE(result.state.snapshot.timeline.playback_deviation_seconds);
    CHECK_FALSE(result.state.snapshot.timeline.cache_end_movement_seconds);
}

TEST_CASE("pause resume and backward replay retain different evidence") {
    auto baseline = healthy();
    baseline.playback_time_seconds = 10.0;
    baseline.cache_end_seconds = 20.0;
    auto state = initial_playback_health(Generation{1}, LoadAttempt{1}, BufferPhase::Zap, TimePoint{});
    PlaybackHealthFoldOptions options;
    options.first_frame_seen = true;

    state = fold_playback_health(state, baseline, TimePoint{} + seconds(0.5), options).state;
    auto paused = baseline;
    paused.cache_paused = true;
    auto pause_fold = fold_playback_health(
        state, paused, TimePoint{} + seconds(2.0), options);
    REQUIRE(pause_fold.discontinuity);
    CHECK(pause_fold.state.snapshot.timeline.playback_movement_seconds == Approx(0.0));
    CHECK(pause_fold.state.snapshot.timeline.cache_paused);

    auto resumed = baseline;
    resumed.playback_time_seconds = 10.5;
    auto resume_fold = fold_playback_health(
        pause_fold.state, resumed, TimePoint{} + seconds(4.0), options);
    REQUIRE(resume_fold.discontinuity);
    CHECK(resume_fold.state.snapshot.timeline.playback_movement_seconds == Approx(0.5));
    CHECK(resume_fold.state.snapshot.timeline.previous_cache_paused == true);
    CHECK_FALSE(resume_fold.state.snapshot.timeline.cache_paused);

    auto replayed = resumed;
    replayed.playback_time_seconds = 8.0;
    const auto replay_fold = fold_playback_health(
        resume_fold.state, replayed, TimePoint{} + seconds(4.5), options);
    REQUIRE(replay_fold.discontinuity);
    REQUIRE(replay_fold.state.snapshot.timeline.playback_movement_seconds);
    CHECK(*replay_fold.state.snapshot.timeline.playback_movement_seconds < 0.0);
    CHECK(replay_fold.state.snapshot.timeline.playback_movement_seconds !=
          resume_fold.state.snapshot.timeline.playback_movement_seconds);
}

TEST_CASE("stale generations cannot alter current-load timeline evidence") {
    auto current = healthy();
    current.generation = Generation{9};
    auto state = initial_playback_health(Generation{9}, LoadAttempt{1}, BufferPhase::Zap, TimePoint{});
    PlaybackHealthFoldOptions options;
    options.first_frame_seen = true;
    state = fold_playback_health(
        state, current, TimePoint{} + seconds(0.5), options).state;

    auto stale = current;
    stale.generation = Generation{8};
    stale.playback_time_seconds = -500.0;
    stale.cache_end_seconds = -500.0;
    const auto rejected = fold_playback_health(
        state, stale, TimePoint{} + seconds(1.0), options);
    CHECK_FALSE(rejected.observation_accepted);
    CHECK(rejected.state.generation == Generation{9});
    CHECK(rejected.state.snapshot.timeline.generation == Generation{9});
    CHECK(rejected.state.discontinuities == state.discontinuities);
    CHECK(rejected.state.snapshot.timeline.playback_movement_seconds ==
          state.snapshot.timeline.playback_movement_seconds);
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

    auto state = initial_playback_health(Generation{1}, LoadAttempt{1}, BufferPhase::Zap, TimePoint{});
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
