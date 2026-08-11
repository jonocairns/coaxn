#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <vector>

#include "core/playback_health.hpp"
#include "core/policy.hpp"
#include "core/supervisor_host.hpp"
#include "player/live_sync_turn.hpp"
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

// The recovery-sensitive portion of App::run(), with the same ordering:
// drain player edges, publish cache state, fold a health sample, then poll the
// supervisor. Recovery effects synchronously issue the next load just as
// App::execute_supervisor_effect() does, but all time and mpv edges are fake.
class RecoveryAppLoop {
public:
    RecoveryAppLoop()
        : supervisor_(clock_, {
              .on_effect = [this](const core::SupervisorEffect& effect) {
                  effects.push_back(effect);
                  REQUIRE(std::holds_alternative<core::ReopenStream>(effect.payload));
                  issue_load(effect.load_attempt, core::LoadIntent::RecoveryReopen);
                  supervisor_.dispatch(core::StreamLoadIssued{
                      effect.generation, effect.load_attempt,
                      core::LoadIntent::RecoveryReopen,
                      core::RecoveryTransport::MpegTs});
              },
              .on_state_changed = {},
              .on_transition = [this](const core::SupervisorTransition& transition) {
                  transitions.push_back(transition);
              },
          }) {}

    void play() {
        supervisor_.dispatch(core::ChannelRequested{generation_});
        issue_load(core::LoadAttempt{1}, core::LoadIntent::FreshSelection);
        supervisor_.dispatch(core::StreamLoadIssued{
            generation_, core::LoadAttempt{1}, core::LoadIntent::FreshSelection,
            core::RecoveryTransport::MpegTs});
    }

    void tick(double at, core::PlaybackHealthObservation observation,
              bool frame_started = false) {
        clock_.current = core::TimePoint{core::seconds(at)};
        if (frame_started) {
            events_.playback_restart(active_entry_);
        }
        process_player_events();

        if (core::supervisor_health_supervision_enabled(supervisor_.current().name)) {
            dispatch_cache_state(observation.cache_paused);
            sample_health(std::move(observation));
        }
        supervisor_.poll();
    }

    void source_ended(double at, core::EndReason reason) {
        clock_.current = core::TimePoint{core::seconds(at)};
        supervisor_.dispatch(core::StreamEnded{
            generation_, active_attempt_, reason, std::nullopt});
    }

    void poll(double at) {
        clock_.current = core::TimePoint{core::seconds(at)};
        supervisor_.poll();
    }

    [[nodiscard]] const core::SupervisorState& state() const {
        return supervisor_.current();
    }

    [[nodiscard]] const core::PlaybackHealthState& health() const {
        REQUIRE(health_);
        return *health_;
    }

    std::vector<core::SupervisorEffect> effects;
    std::vector<core::SupervisorTransition> transitions;

private:
    void issue_load(core::LoadAttempt load_attempt, core::LoadIntent) {
        active_attempt_ = load_attempt;
        health_ = core::initial_playback_health(
            generation_, load_attempt, core::BufferPhase::Zap, clock_.now(),
            core::buffer_phase_targets(core::BufferPhase::Zap).cache_seconds);
        turn_.begin_load();
        last_cache_state_.reset();
        stall_reported_ = false;
        decode_stall_reported_ = false;

        const auto request_id = ++request_id_;
        active_entry_ = ++entry_id_;
        events_.track_load(request_id, generation_, load_attempt);
        events_.start_file(active_entry_);
    }

    void process_player_events() {
        const auto drained = events_.drain();
        turn_.observe_events(drained, generation_, active_attempt_);
        for (const auto& event : drained) {
            if (event.generation != generation_ ||
                event.load_attempt != active_attempt_) continue;
            if (std::holds_alternative<player::FirstPlaybackStart>(event.payload)) {
                supervisor_.dispatch(core::FirstFrame{
                    event.generation, event.load_attempt});
            }
        }
    }

    void dispatch_cache_state(bool paused) {
        if (last_cache_state_ && *last_cache_state_ == paused) return;
        last_cache_state_ = paused;
        supervisor_.dispatch(core::CacheState{
            generation_, active_attempt_, paused});
    }

    void sample_health(core::PlaybackHealthObservation observation) {
        REQUIRE(health_);
        observation.generation = generation_;
        observation.load_attempt = active_attempt_;
        const auto phase = health_->snapshot.phase.value_or(core::BufferPhase::Zap);
        core::PlaybackHealthFoldOptions options;
        options.first_frame_seen = turn_.first_frame_seen();
        options.phase = phase;
        options.target_seconds = core::buffer_phase_targets(phase).cache_seconds;
        const auto fold = core::fold_playback_health(
            *health_, observation, clock_.now(), options);
        REQUIRE(fold.observation_accepted);
        health_ = fold.state;

        if (fold.state.snapshot.progressing && *fold.state.snapshot.progressing) {
            supervisor_.dispatch(core::ForwardProgressObserved{
                generation_, active_attempt_});
        }
        if (fold.state.verdict != core::PlaybackHealthVerdict::Unknown) {
            supervisor_.dispatch(core::PlaybackHealthObserved{
                generation_, active_attempt_,
                fold.state.verdict != core::PlaybackHealthVerdict::Healthy});
        }
        if (fold.stalled && !stall_reported_) {
            stall_reported_ = true;
            supervisor_.dispatch(core::PlaybackStalled{
                generation_, active_attempt_,
                fold.cache_stalled ? core::StallKind::Cache
                    : (fold.state.verdict == core::PlaybackHealthVerdict::OpenStalled
                           ? core::StallKind::Open : core::StallKind::Progress)});
        } else if (fold.decode_stalled && !decode_stall_reported_) {
            decode_stall_reported_ = true;
            supervisor_.dispatch(core::DecodeStalled{
                generation_, active_attempt_});
        }
    }

    RecoveryClock clock_;
    player::LiveSyncTurn turn_;
    player::PlayerEventAdapter events_;
    core::Generation generation_{1};
    core::LoadAttempt active_attempt_{};
    std::uint64_t request_id_ = 0;
    std::int64_t entry_id_ = 0;
    std::int64_t active_entry_ = 0;
    std::optional<core::PlaybackHealthState> health_;
    std::optional<bool> last_cache_state_;
    bool stall_reported_ = false;
    bool decode_stall_reported_ = false;
    core::PlaybackSupervisor supervisor_;
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
    app.poll(6.5);
    REQUIRE(app.effects.size() == 1);
    CHECK(app.effects[0].load_attempt == core::LoadAttempt{2});
    REQUIRE(std::holds_alternative<core::ReopenStream>(app.effects[0].payload));
    REQUIRE(app.state().name == core::SupervisorStateName::Zap);
    REQUIRE(app.state().load_attempt == core::LoadAttempt{2});

    // The new request remains busy and advances its demuxer cache, but the
    // playback clock is stale and mpv never publishes PLAYBACK_RESTART.
    for (int sample = 1; sample <= 16; ++sample) {
        app.tick(6.5 + sample * 0.5, opening_delivery(sample * 0.5));
    }
    CHECK(app.health().snapshot.input_advancing == true);
    CHECK(app.health().snapshot.progressing == false);
    CHECK(app.health().verdict == core::PlaybackHealthVerdict::OpenStalled);
    CHECK(app.state().name == core::SupervisorStateName::Recovering);
    REQUIRE(app.state().detection == core::DetectionReason::OpenStall);
    CHECK(app.effects.size() == 1);

    // Attempt one was the EOF reopen. The recovered load's open stall keeps
    // that episode and uses the second backoff before issuing attempt three.
    app.poll(15.5);
    REQUIRE(app.effects.size() == 2);
    CHECK(app.effects[1].load_attempt == core::LoadAttempt{3});
    CHECK(std::holds_alternative<core::ReopenStream>(app.effects[1].payload));
    CHECK(app.state().load_attempt == core::LoadAttempt{3});
}
