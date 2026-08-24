#include "player/playback_session.hpp"

#include <algorithm>
#include <chrono>
#include <utility>
#include <variant>

#include "core/policy.hpp"
#include "player/playback_observability.hpp"

namespace coax::player {
namespace {

template<class... Ts> struct Overloaded : Ts... { using Ts::operator()...; };
template<class... Ts> Overloaded(Ts...) -> Overloaded<Ts...>;

bool is_current(const PlayerEvent& event, core::Generation generation,
                const std::optional<ActiveLoad>& active) {
    return event.generation == generation && active &&
           event.generation == active->generation &&
           event.load_attempt == active->load_attempt;
}

bool accepts_generic_stream_end(const PlayerEvent& event, core::Generation generation,
                                const std::optional<ActiveLoad>& active,
                                bool exact_failure_reported) {
    return is_current(event, generation, active) && !exact_failure_reported;
}

}  // namespace

const Diagnostics& PlaybackSession::diagnostics() const {
    static const Diagnostics empty;
    return callbacks_.diagnostics ? callbacks_.diagnostics() : empty;
}

PlaybackSession::PlaybackSession(const core::SupervisorClock& clock,
                                 PlaybackSessionCallbacks callbacks,
                                 core::RecoveryPolicy policy)
    : clock_(clock), callbacks_(std::move(callbacks)),
      supervisor_(clock_, {
          .on_effect = [this](const core::SupervisorEffect& effect) {
              execute_recovery(effect);
          },
          .on_state_changed = [this](const core::SupervisorState& state) {
              on_supervisor_state_changed(state);
          },
          .on_transition = [this](const core::SupervisorTransition& transition) {
              if (callbacks_.on_transition) callbacks_.on_transition(transition);
          },
      }, policy) {}

core::Generation PlaybackSession::begin_channel() {
    reset_live_state();
    recovery_freshness_.reset();
    timeline_recovery_.reset();
    timeline_recovery_pending_ = false;
    timeline_recovery_capability_ = TimelineRecoveryCapability::Disabled;

    generation_ = core::Generation{generation_.value() + 1};
    supervisor_.dispatch(core::ChannelRequested{generation_});
    return generation_;
}

void PlaybackSession::load_started(core::LoadAttempt load_attempt,
                                   core::LoadIntent intent,
                                   core::RecoveryTransport transport,
                                   TimelineRecoveryCapability
                                       timeline_recovery_capability) {
    timeline_recovery_capability_ = timeline_recovery_capability;
    restart_health_supervision(load_attempt);
    live_sync_turn_.begin_load();
    supervisor_.dispatch(core::StreamLoadIssued{
        generation_, load_attempt, intent, transport});
}

void PlaybackSession::load_failed(core::LoadAttempt load_attempt) {
    supervisor_.dispatch(core::SourceFailed{generation_, load_attempt});
}

bool PlaybackSession::stop(core::Generation generation) {
    if (generation != generation_ ||
        (supervisor_.current().name == core::SupervisorStateName::Idle &&
         supervisor_.current().generation == generation)) return false;

    supervisor_.dispatch(core::PlaybackStopped{generation});
    playback_health_.reset();
    health_snapshot_ = {};
    timeline_classification_ = TimelineClassification::Unavailable;
    next_health_sample_ = {};
    stall_reported_ = false;
    decode_stall_reported_ = false;
    exact_failure_reported_ = false;
    last_cache_state_dispatched_.reset();
    pending_stream_ends_.clear();
    recovery_freshness_.reset();
    timeline_recovery_.reset();
    timeline_recovery_pending_ = false;
    timeline_recovery_capability_ = TimelineRecoveryCapability::Disabled;
    reset_live_state();
    return true;
}

void PlaybackSession::backend_recreated() {
    recovery_freshness_.reset();
    reset_live_state();
}

void PlaybackSession::reset_live_state() {
    live_sync_.reset();
    live_sync_turn_.reset();
    rebuffer_count_ = 0;
    last_rebuffer_at_.reset();
    if (callbacks_.set_speed) callbacks_.set_speed(1.0);
}

void PlaybackSession::complete_recovery(
    const core::SupervisorEffect& effect,
    std::optional<core::RecoveryTransport> transport) {
    if (!transport) {
        supervisor_.dispatch(core::SourceFailed{effect.generation, effect.load_attempt});
        return;
    }
    restart_health_supervision(effect.load_attempt);
    live_sync_turn_.begin_load();
    core::LoadIntent intent = core::LoadIntent::RecoveryReopen;
    if (std::holds_alternative<core::RecreatePlayer>(effect.payload)) {
        intent = core::LoadIntent::PlayerRecreation;
    }
    supervisor_.dispatch(core::StreamLoadIssued{
        effect.generation, effect.load_attempt, intent, *transport});
}

void PlaybackSession::execute_recovery(const core::SupervisorEffect& effect) {
    const auto report_exception = [&](std::exception_ptr failure) noexcept {
        if (!callbacks_.on_recovery_exception) return;
        try {
            callbacks_.on_recovery_exception(effect, std::move(failure));
        } catch (...) {
            // Diagnostics must never prevent the supervisor from settling the
            // effect that raised them.
        }
    };

    begin_recovery_freshness(effect);

    std::optional<core::RecoveryTransport> transport;
    if (callbacks_.execute_recovery) {
        try {
            transport = callbacks_.execute_recovery(effect);
        } catch (...) {
            report_exception(std::current_exception());
            transport.reset();
        }
    }
    if (transport && std::holds_alternative<core::RecreatePlayer>(effect.payload)) {
        try {
            backend_recreated();
            if (callbacks_.restore_backend_settings) callbacks_.restore_backend_settings();
        } catch (...) {
            report_exception(std::current_exception());
            transport.reset();
        }
    }
    complete_recovery(effect, transport);
}

void PlaybackSession::begin_recovery_freshness(
    const core::SupervisorEffect& effect) {
    const auto& state = supervisor_.current();
    const bool source_reopen =
        !std::holds_alternative<core::RecreatePlayer>(effect.payload) &&
        state.transport == core::RecoveryTransport::MpegTs &&
        timeline_recovery_capability_ ==
            TimelineRecoveryCapability::ContinuousRawMpegTs;
    if (!source_reopen) return;

    core::PlaybackHealthObservation observation;
    const auto active = callbacks_.active_load ? callbacks_.active_load() : std::nullopt;
    if (active && active->generation == state.generation &&
        active->load_attempt == state.load_attempt && callbacks_.health_observation) {
        observation = callbacks_.health_observation();
        if (observation.generation != active->generation ||
            observation.load_attempt != active->load_attempt) {
            observation.playback_time_seconds.reset();
            observation.cache_end_seconds.reset();
        }
    }

    recovery_freshness_.begin_recovery({
        .generation = effect.generation,
        .outgoing_load_attempt = state.load_attempt,
        .recovered_load_attempt = effect.load_attempt,
        .observed_at = clock_.now(),
        .playback_time_seconds = observation.playback_time_seconds,
        .cache_end_seconds = observation.cache_end_seconds,
        .cache_paused = observation.cache_paused,
        .recovery_reason = state.detection,
    });
}

void PlaybackSession::observe_recovery_freshness(
    const core::PlaybackHealthObservation& observation,
    RecoveryFreshnessObservationPoint point,
    RecoveryFreshnessPhase phase) {
    const auto report = recovery_freshness_.observe({
        .generation = observation.generation,
        .load_attempt = observation.load_attempt,
        .observed_at = clock_.now(),
        .playback_time_seconds = observation.playback_time_seconds,
        .cache_end_seconds = observation.cache_end_seconds,
        .cache_paused = observation.cache_paused,
        .point = point,
        .phase = phase,
    });
    if (report && callbacks_.on_recovery_freshness) {
        callbacks_.on_recovery_freshness(*report);
    }
}

void PlaybackSession::restart_health_supervision(core::LoadAttempt load_attempt) {
    const auto now = clock_.now();
    const auto target = core::buffer_phase_targets(core::BufferPhase::Zap);
    playback_health_ = core::initial_playback_health(
        generation_, load_attempt, core::BufferPhase::Zap, now, target.cache_seconds);
    timeline_recovery_.begin_load(generation_, load_attempt);
    health_snapshot_ = playback_health_->snapshot;
    timeline_classification_ = TimelineClassification::Unavailable;
    next_health_sample_ = now + core::kDefaultHealthPolicy.sample_interval;
    stall_reported_ = false;
    decode_stall_reported_ = false;
    exact_failure_reported_ = false;
    const auto& current_diagnostics = diagnostics();
    last_health_engine_message_count_ = current_diagnostics.engine_message_count;
    last_health_unattributed_engine_message_count_ =
        current_diagnostics.unattributed_engine_message_count;
    last_cache_state_dispatched_.reset();
    if (callbacks_.set_health_discontinuities) callbacks_.set_health_discontinuities(0);
}

void PlaybackSession::service_turn(std::span<const PlayerEvent> events,
                                   const std::function<void()>& after_events) {
    process_events(events);
    if (after_events) after_events();
    dispatch_cache_state();
    sample_health();
    supervisor_.poll();
    update_live_sync();
}

void PlaybackSession::process_events(std::span<const PlayerEvent> events) {
    auto active = callbacks_.active_load ? callbacks_.active_load() : std::nullopt;
    if (active) {
        live_sync_turn_.observe_events(events, generation_, active->load_attempt);
    }

    for (const auto& event : events) {
        if (callbacks_.on_player_event) callbacks_.on_player_event(event);
        std::visit(Overloaded{
            [&](const LoadCommandResult& result) {
                if (!result.accepted) {
                    supervisor_.dispatch(core::SourceFailed{
                        event.generation, event.load_attempt});
                }
            },
            [&](const FirstPlaybackStart&) {
                active = callbacks_.active_load ? callbacks_.active_load() : std::nullopt;
                if (!is_current(event, generation_, active)) return;
                if (callbacks_.health_observation) {
                    auto observation = callbacks_.health_observation();
                    observe_recovery_freshness(
                        observation, RecoveryFreshnessObservationPoint::FirstFrame,
                        RecoveryFreshnessPhase::Probation);
                }
                supervisor_.dispatch(core::FirstFrame{
                    event.generation, event.load_attempt});
            },
            [&](const EndFileEvent& ended) {
                if (ended.reason == PlayerEndReason::Stop ||
                    ended.reason == PlayerEndReason::Quit ||
                    ended.reason == PlayerEndReason::Redirect) return;
                active = callbacks_.active_load ? callbacks_.active_load() : std::nullopt;
                if (!accepts_generic_stream_end(
                        event, generation_, active, exact_failure_reported_)) return;
                auto reason = core::EndReason::Unknown;
                if (ended.reason == PlayerEndReason::Eof) reason = core::EndReason::Eof;
                else if (ended.reason == PlayerEndReason::Error) reason = core::EndReason::Error;
                pending_stream_ends_.push_back({event.generation, event.load_attempt, reason,
                                                clock_.now() + core::milliseconds(50)});
            },
            [&](const PlaybackStopped& stopped) {
                if (stopped.kind == IntentionalStopKind::Requested) {
                    supervisor_.dispatch(core::PlaybackStopped{event.generation});
                }
            },
            [&](const BackendFailed&) {
                supervisor_.dispatch(core::ProcessExited{
                    event.generation, event.load_attempt});
            },
            [&](const PropertyCommandResult&) {},
            [&](const PlayerAuthenticationRejected&) {
                active = callbacks_.active_load ? callbacks_.active_load() : std::nullopt;
                if (is_current(event, generation_, active)) exact_failure_reported_ = true;
                std::erase_if(pending_stream_ends_, [&](const auto& pending) {
                    return pending.generation == event.generation &&
                           pending.load_attempt == event.load_attempt;
                });
                supervisor_.dispatch(core::AuthRejected{
                    event.generation, event.load_attempt});
            },
            [&](const TransportFailureDetected& failure) {
                active = callbacks_.active_load ? callbacks_.active_load() : std::nullopt;
                if (is_current(event, generation_, active)) exact_failure_reported_ = true;
                std::erase_if(pending_stream_ends_, [&](const auto& pending) {
                    return pending.generation == event.generation &&
                           pending.load_attempt == event.load_attempt;
                });
                supervisor_.dispatch(core::StreamEnded{
                    event.generation, event.load_attempt,
                    core::EndReason::Error, failure.reason});
            }}, event.payload);
    }
    flush_pending_stream_ends();
}

void PlaybackSession::flush_pending_stream_ends() {
    const auto now = clock_.now();
    for (auto pending = pending_stream_ends_.begin(); pending != pending_stream_ends_.end();) {
        if (pending->dispatch_at > now) {
            ++pending;
            continue;
        }
        supervisor_.dispatch(core::StreamEnded{
            pending->generation, pending->load_attempt, pending->reason, std::nullopt});
        pending = pending_stream_ends_.erase(pending);
    }
}

void PlaybackSession::dispatch_cache_state() {
    const auto active = callbacks_.active_load ? callbacks_.active_load() : std::nullopt;
    if (!active || !core::supervisor_health_supervision_enabled(supervisor_.current().name)) {
        return;
    }
    const bool paused = diagnostics().paused_for_cache;
    if (last_cache_state_dispatched_ && *last_cache_state_dispatched_ == paused) return;
    last_cache_state_dispatched_ = paused;
    supervisor_.dispatch(core::CacheState{
        active->generation, active->load_attempt, paused});
}

void PlaybackSession::sample_health() {
    const auto active = callbacks_.active_load ? callbacks_.active_load() : std::nullopt;
    if (!playback_health_ || !active ||
        !core::supervisor_health_supervision_enabled(supervisor_.current().name)) return;
    const auto now = clock_.now();
    if (now < next_health_sample_) return;
    next_health_sample_ = now + core::kDefaultHealthPolicy.sample_interval;

    const auto& current_diagnostics = diagnostics();
    const auto target = core::buffer_phase_targets(current_diagnostics.buffer_phase);
    const auto observation = callbacks_.health_observation
        ? callbacks_.health_observation() : core::PlaybackHealthObservation{};
    const core::PlaybackHealthFoldOptions options{
        .container_fps = current_diagnostics.container_fps,
        .first_frame_seen = live_sync_turn_.first_frame_seen(),
        .main_process_cpu_percent = std::nullopt,
        .phase = current_diagnostics.buffer_phase,
        .target_seconds = target.cache_seconds,
    };
    const auto fold = core::fold_playback_health(*playback_health_, observation, now, options);
    if (!fold.observation_accepted) {
        if (callbacks_.on_health_sample) {
            HealthSampleReport report;
            report.fold = fold;
            report.observed_generation = observation.generation;
            callbacks_.on_health_sample(report);
        }
        return;
    }
    playback_health_ = fold.state;
    health_snapshot_ = fold.state.snapshot;
    if (callbacks_.set_health_discontinuities) {
        callbacks_.set_health_discontinuities(fold.state.discontinuities);
    }

    const auto engine_count = current_diagnostics.engine_message_count;
    const auto engine_delta = engine_count >= last_health_engine_message_count_
        ? engine_count - last_health_engine_message_count_ : engine_count;
    last_health_engine_message_count_ = engine_count;
    const auto unattributed_count = current_diagnostics.unattributed_engine_message_count;
    const auto unattributed_delta =
        unattributed_count >= last_health_unattributed_engine_message_count_
            ? unattributed_count - last_health_unattributed_engine_message_count_
            : unattributed_count;
    last_health_unattributed_engine_message_count_ = unattributed_count;
    timeline_classification_ = classify_timeline(health_snapshot_.timeline, options.policy);
    observe_recovery_freshness(
        observation, RecoveryFreshnessObservationPoint::HealthSample,
        !live_sync_turn_.first_frame_seen()
            ? RecoveryFreshnessPhase::Opening
            : (supervisor_.current().name == core::SupervisorStateName::Steady
                   ? RecoveryFreshnessPhase::PostProbation
                   : RecoveryFreshnessPhase::Probation));
    const bool timeline_recovery_eligible =
        supervisor_.current().name == core::SupervisorStateName::Steady &&
        supervisor_.current().transport == core::RecoveryTransport::MpegTs &&
        timeline_recovery_capability_ ==
            TimelineRecoveryCapability::ContinuousRawMpegTs &&
        live_sync_turn_.first_frame_seen();
    std::optional<double> rebuffer_age_seconds;
    if (last_rebuffer_at_ && now >= *last_rebuffer_at_) {
        rebuffer_age_seconds =
            std::chrono::duration<double>(now - *last_rebuffer_at_).count();
    }
    auto timeline_recovery_step = timeline_recovery_.observe({
        .generation = fold.state.generation,
        .load_attempt = fold.state.load_attempt,
        .observed_at = now,
        .timeline = health_snapshot_.timeline,
        .cache_end_seconds = observation.cache_end_seconds,
        .playback_time_seconds = observation.playback_time_seconds,
        .rebuffer_age_seconds = rebuffer_age_seconds,
        .healthy = fold.state.verdict == core::PlaybackHealthVerdict::Healthy,
    }, timeline_recovery_eligible);

    if (fold.state.snapshot.progressing && *fold.state.snapshot.progressing) {
        supervisor_.dispatch(core::ForwardProgressObserved{
            fold.state.generation, fold.state.load_attempt});
    }
    if (fold.state.verdict != core::PlaybackHealthVerdict::Unknown) {
        supervisor_.dispatch(core::PlaybackHealthObserved{
            fold.state.generation, fold.state.load_attempt,
            fold.state.verdict != core::PlaybackHealthVerdict::Healthy});
    }
    if (fold.stalled && !stall_reported_) {
        stall_reported_ = true;
        supervisor_.dispatch(core::PlaybackStalled{
            fold.state.generation, fold.state.load_attempt,
            fold.cache_stalled ? core::StallKind::Cache
                : (fold.state.verdict == core::PlaybackHealthVerdict::OpenStalled
                       ? core::StallKind::Open : core::StallKind::Progress)});
    } else if (fold.decode_stalled && !decode_stall_reported_) {
        decode_stall_reported_ = true;
        supervisor_.dispatch(core::DecodeStalled{
            fold.state.generation, fold.state.load_attempt});
    } else if (timeline_recovery_step.recover) {
        supervisor_.dispatch(core::TimelineRegressed{
            fold.state.generation, fold.state.load_attempt});
        const auto& state = supervisor_.current();
        const bool accepted =
            state.name == core::SupervisorStateName::Recovering &&
            state.detection == core::DetectionReason::TimelineRegression &&
            state.generation == fold.state.generation &&
            state.load_attempt == fold.state.load_attempt;
        timeline_recovery_step.supervisor_accepted = accepted;
        timeline_recovery_pending_ = accepted;
    }
    if (timeline_recovery_step.recover &&
        !timeline_recovery_step.supervisor_accepted) {
        timeline_recovery_step.supervisor_accepted = false;
    }

    if (callbacks_.on_health_sample) {
        callbacks_.on_health_sample({
            .fold = fold,
            .observed_generation = observation.generation,
            .classification = timeline_classification_,
            .engine_messages_since_sample = engine_delta,
            .unattributed_engine_messages_since_sample = unattributed_delta,
            .engine_warning = current_diagnostics.last_engine_message,
            .timeline_recovery = timeline_recovery_step,
        });
    }
}

void PlaybackSession::update_live_sync() {
    if (supervisor_.current().name == core::SupervisorStateName::Idle) return;
    const auto step = live_sync_turn_.observe(diagnostics());
    if (step.rebuffered) {
        last_rebuffer_at_ = clock_.now();
        ++rebuffer_count_;
        live_sync_.notify_rebuffer();
        if (callbacks_.on_rebuffer) {
            callbacks_.on_rebuffer(rebuffer_count_, live_sync_.target_offset_seconds());
        }
    }
    if (step.hold_unity_speed) {
        if (const auto speed = live_sync_.hold_unity_speed()) {
            if (callbacks_.on_unity_speed) callbacks_.on_unity_speed(*speed);
            if (callbacks_.set_speed) callbacks_.set_speed(*speed);
        }
    } else if (step.control_input) {
        const double now_seconds =
            std::chrono::duration<double>(clock_.now().time_since_epoch()).count();
        if (const auto speed = live_sync_.update(*step.control_input, now_seconds)) {
            if (callbacks_.set_speed) callbacks_.set_speed(*speed);
        }
    }
    if (callbacks_.set_live_sync_state) {
        callbacks_.set_live_sync_state(live_sync_.target_offset_seconds(), rebuffer_count_);
    }
}

void PlaybackSession::on_supervisor_state_changed(const core::SupervisorState& state) {
    const auto previous = supervisor_state_name_;
    supervisor_state_name_ = state.name;

    if (timeline_recovery_pending_ && state.name == core::SupervisorStateName::Zap &&
        state.load_intent != core::LoadIntent::FreshSelection && state.first_frame_at) {
        timeline_recovery_.note_recovered_first_frame(*state.first_frame_at);
        timeline_recovery_pending_ = false;
    }

    if (previous == core::SupervisorStateName::Failed &&
        state.name == core::SupervisorStateName::Zap &&
        state.generation == generation_) {
        // The exhausted command's already-issued load won late admission.
        // Restart its fold without resetting the first-frame edge for that
        // same physical load.
        restart_health_supervision(state.load_attempt);
    }
    if (state.name == core::SupervisorStateName::Steady) {
        if (callbacks_.apply_buffer_phase) {
            callbacks_.apply_buffer_phase(state.generation, core::BufferPhase::Steady);
        }
        if (state.generation == generation_) live_sync_turn_.note_playback_established();
    }
    if (callbacks_.on_state_changed) callbacks_.on_state_changed(state, previous);
}

void PlaybackSession::presentation_lost() {
    supervisor_.dispatch(core::PresentationLost{generation_});
}

void PlaybackSession::dispose() { supervisor_.dispose(); }

}  // namespace coax::player
