#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <exception>
#include <optional>
#include <stdexcept>
#include <vector>

#include "core/playback_health.hpp"
#include "core/policy.hpp"
#include "player/playback_session.hpp"
#include "player/player_event_adapter.hpp"

using namespace coax;

namespace {

class RecoveryClock final : public core::SupervisorClock {
public:
    core::TimePoint current{};
    [[nodiscard]] core::TimePoint now() const override { return current; }
};

core::PlaybackHealthObservation opening_delivery(double cache_end_seconds) {
    core::PlaybackHealthObservation observation;
    observation.buffer_seconds = 2.0;
    observation.cache_end_seconds = cache_end_seconds;
    observation.input_rate_bytes_per_second = 400'000.0;
    observation.ipc_round_trip_ms = 0.9;
    observation.playback_time_seconds = 125.0;
    return observation;
}

core::PlaybackHealthObservation no_telemetry() { return {}; }

core::PlaybackHealthObservation playing(double playback_time_seconds) {
    core::PlaybackHealthObservation observation;
    observation.buffer_seconds = 4.0;
    observation.cache_end_seconds = playback_time_seconds + 4.0;
    observation.input_rate_bytes_per_second = 400'000.0;
    observation.ipc_round_trip_ms = 0.9;
    observation.playback_time_seconds = playback_time_seconds;
    return observation;
}

// A fake engine boundary around the production PlaybackSession. The test no
// longer reproduces App's event/cache/health/poll ordering: service_turn() is
// the exact coordinator the Windows application calls.
class RecoveryAppLoop {
public:
    enum class ExecutionMode { Success, Missing, Reject, Throw, ThrowRestore };

    explicit RecoveryAppLoop(
        ExecutionMode mode = ExecutionMode::Success,
        core::RecoveryPolicy policy = core::kDefaultRecoveryPolicy)
        : execution_mode_(mode), session_(clock_, make_callbacks(), policy) {}

    void play() {
        generation_ = session_.begin_channel();
        issue_load(core::LoadAttempt{1}, core::LoadIntent::FreshSelection);
        session_.load_started(core::LoadAttempt{1}, core::LoadIntent::FreshSelection,
                              core::RecoveryTransport::MpegTs);
    }

    void tick(double at, core::PlaybackHealthObservation observation,
              bool frame_started = false) {
        clock_.current = core::TimePoint{core::seconds(at)};
        observation_ = std::move(observation);
        observation_.generation = generation_;
        observation_.load_attempt = active_attempt_;
        diagnostics_.paused_for_cache = observation_.cache_paused;
        diagnostics_.cache_duration_seconds = observation_.buffer_seconds;
        diagnostics_.cache_end_seconds = observation_.cache_end_seconds;
        diagnostics_.input_rate_bytes_per_second =
            observation_.input_rate_bytes_per_second;
        diagnostics_.playback_time_seconds = observation_.playback_time_seconds;
        diagnostics_.video_fps_estimate = observation_.video_fps_estimate;
        if (frame_started) {
            events_.playback_restart(active_entry_);
        }
        const auto drained = events_.drain();
        session_.service_turn(drained);
    }

    void source_ended(double at, core::EndReason reason) {
        clock_.current = core::TimePoint{core::seconds(at)};
        const auto player_reason = reason == core::EndReason::Eof
            ? player::PlayerEndReason::Eof : player::PlayerEndReason::Error;
        const player::PlayerEvent event{
            generation_, active_attempt_, player::EndFileEvent{player_reason, 0}};
        session_.service_turn(std::span{&event, std::size_t{1}});
        // Generic end-file is intentionally held for exact transport evidence.
        clock_.current += core::milliseconds(50);
        session_.service_turn({});
    }

    void poll(double at) {
        clock_.current = core::TimePoint{core::seconds(at)};
        session_.service_turn({});
    }

    void failure_turn(double at, bool with_exact_failure) {
        clock_.current = core::TimePoint{core::seconds(at)};
        std::vector<player::PlayerEvent> events{
            {generation_, active_attempt_,
             player::EndFileEvent{player::PlayerEndReason::Error, -1}},
        };
        if (with_exact_failure) {
            events.push_back({generation_, active_attempt_,
                              player::TransportFailureDetected{
                                  core::TransportFailureReason::HttpRequestTimeout}});
        }
        session_.service_turn(events);
    }

    [[nodiscard]] const core::SupervisorState& state() const {
        return session_.state();
    }

    [[nodiscard]] const core::BufferHealthSnapshot& health() const {
        return session_.health_snapshot();
    }

    [[nodiscard]] double live_target() const { return session_.live_target_seconds(); }

    void backend_failed_turn(double at) {
        clock_.current = core::TimePoint{core::seconds(at)};
        const player::PlayerEvent event{
            generation_, active_attempt_, player::BackendFailed{-1}};
        session_.service_turn(std::span{&event, std::size_t{1}});
    }

    std::vector<core::SupervisorEffect> effects;
    std::vector<core::SupervisorTransition> transitions;
    std::vector<std::exception_ptr> recovery_exceptions;
    std::vector<double> speed_writes;

private:
    player::PlaybackSessionCallbacks make_callbacks() {
        player::PlaybackSessionCallbacks callbacks;
        callbacks.active_load = [this]() { return active_load_; };
        callbacks.diagnostics = [this]() -> const player::Diagnostics& {
            return diagnostics_;
        };
        callbacks.health_observation = [this] { return observation_; };
        if (execution_mode_ != ExecutionMode::Missing) {
            callbacks.execute_recovery = [this](const core::SupervisorEffect& effect)
                -> std::optional<core::RecoveryTransport> {
                effects.push_back(effect);
                if (execution_mode_ == ExecutionMode::Throw) {
                    throw std::runtime_error("fake recovery failure");
                }
                if (execution_mode_ == ExecutionMode::Reject) return std::nullopt;
                issue_load(effect.load_attempt,
                           std::holds_alternative<core::RecreatePlayer>(effect.payload)
                               ? core::LoadIntent::PlayerRecreation
                               : core::LoadIntent::RecoveryReopen);
                return core::RecoveryTransport::MpegTs;
            };
        }
        callbacks.on_recovery_exception =
            [this](const core::SupervisorEffect&, std::exception_ptr failure) {
                recovery_exceptions.push_back(std::move(failure));
            };
        callbacks.restore_backend_settings = [this] {
            if (execution_mode_ == ExecutionMode::ThrowRestore) {
                throw std::runtime_error("fake backend restore failure");
            }
        };
        callbacks.set_health_discontinuities = [this](int count) {
            diagnostics_.health_discontinuities = count;
        };
        callbacks.set_speed = [this](double speed) { speed_writes.push_back(speed); };
        callbacks.on_transition = [this](const core::SupervisorTransition& transition) {
            transitions.push_back(transition);
        };
        return callbacks;
    }

    void issue_load(core::LoadAttempt load_attempt, core::LoadIntent) {
        active_attempt_ = load_attempt;
        active_load_ = player::ActiveLoad{generation_, load_attempt};
        diagnostics_.buffer_phase = core::BufferPhase::Zap;
        observation_ = {};
        observation_.generation = generation_;
        observation_.load_attempt = load_attempt;

        const auto request_id = ++request_id_;
        active_entry_ = ++entry_id_;
        events_.track_load(request_id, generation_, load_attempt);
        events_.start_file(active_entry_);
    }

    RecoveryClock clock_;
    ExecutionMode execution_mode_ = ExecutionMode::Success;
    player::Diagnostics diagnostics_;
    core::PlaybackHealthObservation observation_;
    std::optional<player::ActiveLoad> active_load_;
    player::PlayerEventAdapter events_;
    core::Generation generation_;
    core::LoadAttempt active_attempt_{};
    std::uint64_t request_id_ = 0;
    std::int64_t entry_id_ = 0;
    std::int64_t active_entry_ = 0;
    player::PlaybackSession session_;
};

}  // namespace

TEST_CASE("application ordering cancels an opening retry when its frame wins backoff") {
    RecoveryAppLoop app;
    app.play();

    for (int sample = 1; sample <= 16; ++sample) {
        app.tick(sample * 0.5, no_telemetry());
    }
    REQUIRE(app.state().name == core::SupervisorStateName::Recovering);
    REQUIRE(app.state().detection == core::DetectionReason::OpenStall);
    REQUIRE(app.state().deadlines.retry_at == core::TimePoint{core::seconds(8.5)});
    CHECK(app.effects.empty());

    // mpv publishes its first playback edge for the exact current entry after
    // the eight-second decision but before the 500ms source retry is due.
    app.tick(8.25, playing(1.0), /*frame_started=*/true);
    CHECK(app.state().name == core::SupervisorStateName::Zap);
    CHECK(app.state().attempt == 1);
    CHECK(app.effects.empty());
    REQUIRE_FALSE(app.transitions.empty());
    CHECK(app.transitions.back().reason == "first-frame-cancelled-opening-retry");
    CHECK(app.transitions.back().outcome == core::RecoveryOutcome::LateFirstFrame);

    app.tick(8.5, playing(1.25));
    CHECK(app.effects.empty());
    CHECK(app.state().name == core::SupervisorStateName::Zap);

    app.tick(13.25, playing(6.0));
    CHECK(app.effects.empty());
    CHECK(app.state().name == core::SupervisorStateName::Steady);
}

TEST_CASE("a recovered load cannot hide behind advancing input without a frame") {
    RecoveryAppLoop app;
    app.play();

    app.tick(0.1, playing(0.0), /*frame_started=*/true);
    app.tick(5.1, playing(5.0));
    REQUIRE(app.state().name == core::SupervisorStateName::Steady);

    app.source_ended(6.0, core::EndReason::Eof);
    REQUIRE(app.state().name == core::SupervisorStateName::Recovering);
    app.poll(6.55);
    REQUIRE(app.effects.size() == 1);
    CHECK(app.effects[0].load_attempt == core::LoadAttempt{2});
    REQUIRE(std::holds_alternative<core::ReopenStream>(app.effects[0].payload));
    REQUIRE(app.state().name == core::SupervisorStateName::Zap);
    REQUIRE(app.state().load_attempt == core::LoadAttempt{2});

    // The new request remains busy and advances its demuxer cache, but the
    // playback clock is stale and mpv never publishes PLAYBACK_RESTART.
    for (int sample = 1; sample <= 16; ++sample) {
        app.tick(6.55 + sample * 0.5, opening_delivery(sample * 0.5));
    }
    CHECK(app.health().input_advancing == true);
    CHECK(app.health().progressing == false);
    CHECK(app.health().verdict == core::PlaybackHealthVerdict::OpenStalled);
    CHECK(app.state().name == core::SupervisorStateName::Recovering);
    REQUIRE(app.state().detection == core::DetectionReason::OpenStall);
    CHECK(app.effects.size() == 1);

    // Attempt one was the EOF reopen. The recovered load's open stall keeps
    // that episode and uses the second backoff before issuing attempt three.
    app.poll(15.55);
    REQUIRE(app.effects.size() == 2);
    CHECK(app.effects[1].load_attempt == core::LoadAttempt{3});
    CHECK(std::holds_alternative<core::ReopenStream>(app.effects[1].payload));
    CHECK(app.state().load_attempt == core::LoadAttempt{3});
}

TEST_CASE("the production session coalesces generic and exact stream failures") {
    RecoveryAppLoop app;
    app.play();

    app.failure_turn(1.0, /*with_exact_failure=*/true);
    REQUIRE(app.state().name == core::SupervisorStateName::Recovering);
    CHECK(app.state().detection == core::DetectionReason::HttpRequestTimeout);

    // The generic end's 50ms hold expires, but the exact event removed it.
    app.poll(1.05);
    CHECK(app.effects.empty());
    app.poll(1.5);
    REQUIRE(app.effects.size() == 1);
    CHECK(app.effects.front().load_attempt == core::LoadAttempt{2});
}

TEST_CASE("the production session holds unity through a stall and controls immediately on exit") {
    RecoveryAppLoop app;
    app.play();
    app.tick(0.1, playing(0.0), /*frame_started=*/true);
    app.tick(5.1, playing(5.0));
    REQUIRE(app.state().name == core::SupervisorStateName::Steady);

    auto correction = playing(6.1);
    correction.buffer_seconds = 2.0;
    app.tick(6.2, correction);
    REQUIRE(app.speed_writes.back() == 0.97);
    app.speed_writes.clear();
    auto stalled = correction;
    stalled.cache_paused = true;
    app.tick(6.3, stalled);
    REQUIRE(app.speed_writes == std::vector<double>{1.0});

    app.tick(6.4, stalled);
    CHECK(app.speed_writes == std::vector<double>{1.0});

    // Less than the controller's one-second interval has elapsed. The hold
    // invalidates it, so valid telemetry reinstalls the correction now.
    correction.playback_time_seconds = 6.4;
    app.tick(6.5, correction);
    REQUIRE(app.speed_writes.size() == 2);
    CHECK(app.speed_writes.back() == 0.97);
}

TEST_CASE("ordinary recovery preserves learned live target while recreation resets it") {
    RecoveryAppLoop app;
    app.play();
    app.tick(0.1, playing(0.0), /*frame_started=*/true);
    app.tick(5.1, playing(5.0));
    REQUIRE(app.state().name == core::SupervisorStateName::Steady);

    auto stalled = playing(5.1);
    stalled.cache_paused = true;
    app.tick(5.2, stalled);
    REQUIRE(app.live_target() == 4.5);

    app.source_ended(6.0, core::EndReason::Eof);
    app.poll(6.55);
    REQUIRE(app.effects.size() == 1);
    CHECK(app.live_target() == 4.5);
    CHECK(app.state().load_intent == core::LoadIntent::RecoveryReopen);

    app.backend_failed_turn(7.0);
    app.poll(8.0);
    REQUIRE(app.effects.size() == 2);
    CHECK(std::holds_alternative<core::RecreatePlayer>(app.effects.back().payload));
    CHECK(app.live_target() == 4.0);
    CHECK(app.speed_writes.back() == 1.0);
    CHECK(app.state().load_intent == core::LoadIntent::PlayerRecreation);
}

TEST_CASE("missing rejected and throwing session recovery executors cannot park recovery") {
    auto policy = core::kDefaultRecoveryPolicy;
    policy.attempt_delays.fill(core::Duration{});

    for (const auto mode : {RecoveryAppLoop::ExecutionMode::Missing,
                            RecoveryAppLoop::ExecutionMode::Reject,
                            RecoveryAppLoop::ExecutionMode::Throw}) {
        CAPTURE(mode);
        RecoveryAppLoop app(mode, policy);
        app.play();
        app.failure_turn(1.0, /*with_exact_failure=*/true);
        for (int attempt = 0; attempt < 6; ++attempt) app.poll(1.0);
        CHECK(app.state().name == core::SupervisorStateName::Failed);
        CHECK(app.state().failure == core::FailureReason::SourceUnavailable);
        if (mode == RecoveryAppLoop::ExecutionMode::Throw) {
            CHECK_FALSE(app.recovery_exceptions.empty());
            CHECK(app.recovery_exceptions.front());
        } else {
            CHECK(app.recovery_exceptions.empty());
        }
    }
}

TEST_CASE("a throwing backend restore is reported and cannot park recovery") {
    auto policy = core::kDefaultRecoveryPolicy;
    policy.attempt_delays.fill(core::Duration{});

    RecoveryAppLoop app(RecoveryAppLoop::ExecutionMode::ThrowRestore, policy);
    app.play();
    app.backend_failed_turn(1.0);
    for (int attempt = 0; attempt < 6; ++attempt) app.poll(1.0);

    REQUIRE_FALSE(app.recovery_exceptions.empty());
    CHECK(app.recovery_exceptions.front());
    CHECK(app.state().name == core::SupervisorStateName::Failed);
    CHECK(app.state().failure == core::FailureReason::SourceUnavailable);
}
