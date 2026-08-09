#include "core/supervisor.hpp"

#include <algorithm>
#include <cassert>
#include <type_traits>

namespace coax::core {
namespace {

template<class>
inline constexpr bool kAlwaysFalse = false;

SupervisorReduction ignore(const SupervisorState& state) {
    assert(supervisor_deadlines_valid(state));
    return {.effects = {}, .state = state, .transition = std::nullopt};
}

SupervisorReduction settle(const SupervisorState& previous, SupervisorState next,
                           std::string reason, TimePoint now,
                           const RecoveryPolicy& policy,
                           std::vector<SupervisorEffect> effects = {}) {
    assert(supervisor_deadlines_valid(previous));
    assert(supervisor_deadlines_valid(next));
    const Duration elapsed = next.recovery_started_at
        ? std::max(Duration::zero(), now - *next.recovery_started_at) : Duration::zero();
    SupervisorTransition transition{
        .attempt = next.attempt,
        .elapsed_budget = elapsed,
        .from = previous.name,
        .generation = next.generation,
        .load_attempt = next.load_attempt,
        .load_intent = next.load_intent,
        .escalation = RecoveryEscalation::None,
        .outcome = RecoveryOutcome::None,
        .last_progress_to_decision = std::nullopt,
        .decision_to_command = std::nullopt,
        .command_to_first_frame = std::nullopt,
        .first_frame_to_outcome = std::nullopt,
        .recovered_load_lifetime = std::nullopt,
        .policy_version = policy.version,
        .reason = std::move(reason),
        .to = next.name,
        .transport = next.transport,
        .transport_policy_version = kTransportPolicyVersion,
    };
    return {.effects = std::move(effects), .state = std::move(next),
            .transition = std::move(transition)};
}

std::optional<Duration> elapsed_between(std::optional<TimePoint> earlier,
                                        TimePoint later);

SupervisorReduction terminal(const SupervisorState& state,
                             std::optional<DetectionReason> detection,
                             FailureReason failure,
                             std::optional<TimePoint> recovery_started_at,
                             TimePoint now, const RecoveryPolicy& policy) {
    const bool command_budget_exhausted =
        failure == FailureReason::AttemptsExhausted ||
        failure == FailureReason::BudgetExpired;
    const bool late_completion_available = command_budget_exhausted && detection &&
        *detection == DetectionReason::OpenStall && state.load_command_at &&
        !state.first_frame_at && !state.late_completion_probation;
    auto next = state;
    next.deadlines = {};
    next.detection = detection;
    next.failure = failure;
    next.first_frame_at.reset();
    next.name = SupervisorStateName::Failed;
    next.pending_load_attempt.reset();
    next.pending_load_intent.reset();
    next.recovery.reset();
    next.recovery_started_at = recovery_started_at;
    next.late_completion_available = late_completion_available;
    next.late_completion_probation = false;
    auto result = settle(state, next, to_string(failure), now, policy);
    result.transition->outcome = RecoveryOutcome::TerminalFailure;
    result.transition->first_frame_to_outcome =
        elapsed_between(state.first_frame_at, now);
    if (state.load_intent != LoadIntent::FreshSelection) {
        result.transition->recovered_load_lifetime =
            elapsed_between(state.load_command_at, now);
    }
    if (state.short_load_recreation_used ||
        state.load_intent == LoadIntent::PlayerRecreation) {
        result.transition->escalation = RecoveryEscalation::PlayerRecreation;
    } else if (recovery_started_at ||
               state.load_intent == LoadIntent::RecoveryReopen) {
        result.transition->escalation = RecoveryEscalation::SourceReopen;
    }
    return result;
}

SupervisorReduction retire_failed_late_completion(
    const SupervisorState& state, std::string reason, TimePoint now,
    const RecoveryPolicy& policy) {
    if (state.name != SupervisorStateName::Failed ||
        !state.late_completion_available) return ignore(state);
    auto next = state;
    next.late_completion_available = false;
    return settle(state, next, std::move(reason), now, policy);
}

DetectionReason stream_end_detection(EndReason reason,
                                     std::optional<TransportFailureReason> failure) {
    if (failure) {
        switch (*failure) {
            case TransportFailureReason::FormatProbeRequired:
                return DetectionReason::FormatProbeRequired;
            case TransportFailureReason::HlsPlaylistFailed:
                return DetectionReason::HlsPlaylistFailed;
            case TransportFailureReason::HlsSegmentUnavailable:
                return DetectionReason::HlsSegmentUnavailable;
            case TransportFailureReason::HttpRequestTimeout:
                return DetectionReason::HttpRequestTimeout;
        }
    }
    switch (reason) {
        case EndReason::Eof: return DetectionReason::StreamEndedEof;
        case EndReason::Error: return DetectionReason::StreamEndedError;
        case EndReason::Unknown: return DetectionReason::StreamEndedUnknown;
    }
    return DetectionReason::StreamEndedUnknown;
}

SupervisorEffectPayload effect_payload(RecoveryAction action) {
    switch (action) {
        case RecoveryAction::ReopenStream: return ReopenStream{};
        case RecoveryAction::ReloadHlsLive: return ReloadHlsLive{};
        case RecoveryAction::ReopenProbed: return ReopenProbed{};
        case RecoveryAction::RecreatePlayer: return RecreatePlayer{};
    }
    return ReopenStream{};
}

bool short_recovery_fault(DetectionReason detection) {
    switch (detection) {
        case DetectionReason::CacheStall:
        case DetectionReason::DecodeStall:
        case DetectionReason::OpenStall:
        case DetectionReason::ProgressStall:
        case DetectionReason::StreamEndedEof:
        case DetectionReason::StreamEndedError:
        case DetectionReason::StreamEndedUnknown:
            return true;
        default:
            return false;
    }
}

bool source_reopen_action(RecoveryAction action) {
    return action != RecoveryAction::RecreatePlayer;
}

RecoveryEscalation escalation_for(RecoveryAction action) {
    return action == RecoveryAction::RecreatePlayer
        ? RecoveryEscalation::PlayerRecreation : RecoveryEscalation::SourceReopen;
}

std::optional<Duration> elapsed_between(std::optional<TimePoint> earlier, TimePoint later) {
    return earlier
        ? std::optional<Duration>{std::max(Duration::zero(), later - *earlier)}
        : std::nullopt;
}

SupervisorReduction recover(const SupervisorState& state, DetectionReason detection,
                            RecoveryAction action, TimePoint now,
                            const RecoveryPolicy& policy) {
    // Repeated load faults cannot spend another attempt while their retry is
    // already waiting. A backend/presentation fault is different: reopening a
    // source cannot repair a dead mpv instance or presentation device, so it
    // may upgrade a weaker pending action to recreation. Once recreation is
    // pending, later duplicates collapse as usual.
    const bool upgrades_pending_recovery = state.name == SupervisorStateName::Recovering &&
        action == RecoveryAction::RecreatePlayer &&
        state.recovery != RecoveryAction::RecreatePlayer;
    if (state.name == SupervisorStateName::Idle || state.name == SupervisorStateName::Failed ||
        (state.name == SupervisorStateName::Recovering && !upgrades_pending_recovery)) {
        return ignore(state);
    }
    const TimePoint started = state.recovery_started_at.value_or(now);
    if (state.attempt >= policy.attempt_delays.size()) {
        return terminal(state, detection, FailureReason::AttemptsExhausted, started, now, policy);
    }
    const TimePoint retry_at = now + policy.attempt_delays[state.attempt];
    if (retry_at > started + policy.wall_clock_budget) {
        return terminal(state, detection, FailureReason::BudgetExpired, started, now, policy);
    }
    auto next = state;
    if (upgrades_pending_recovery && state.pending_load_attempt) {
        // The weaker effect has already been emitted and may have reached mpv.
        // Stop accepting its settlement, but do not reuse its load identity:
        // the recreation command must remain strictly newer at the adapter
        // boundary as well as in the reducer.
        next.pending_load_attempt =
            LoadAttempt{state.pending_load_attempt->value() + 1};
        next.pending_load_intent = LoadIntent::PlayerRecreation;
    }
    const bool episode_active = state.recovery_started_at.has_value();
    const bool short_reopen = state.transport == RecoveryTransport::MpegTs &&
        state.name == SupervisorStateName::Zap &&
        state.load_intent == LoadIntent::RecoveryReopen &&
        episode_active && short_recovery_fault(detection) &&
        state.last_short_recovery_failure_attempt != state.load_attempt;
    if (short_reopen) {
        ++next.short_recovery_load_failures;
        next.last_short_recovery_failure_attempt = state.load_attempt;
    }
    if (short_reopen && !state.short_load_recreation_used &&
        next.short_recovery_load_failures >= policy.short_reopens_before_recreation) {
        action = RecoveryAction::RecreatePlayer;
        next.short_load_recreation_used = true;
    }
    ++next.attempt;
    next.deadlines = {.retry_at = retry_at, .steady_at = std::nullopt};
    next.detection = detection;
    next.failure.reset();
    next.first_frame_at.reset();
    next.fault_decided_at = now;
    next.name = SupervisorStateName::Recovering;
    next.recovery = action;
    next.recovery_started_at = started;
    next.late_completion_available = false;
    next.late_completion_probation = false;
    auto result = settle(state, next, to_string(detection), now, policy);
    result.transition->escalation = escalation_for(action);
    result.transition->last_progress_to_decision =
        elapsed_between(state.last_forward_progress_at, now);
    result.transition->first_frame_to_outcome = elapsed_between(state.first_frame_at, now);
    if (state.load_intent != LoadIntent::FreshSelection) {
        result.transition->recovered_load_lifetime =
            elapsed_between(state.load_command_at, now);
    }
    result.transition->outcome = episode_active
        ? (detection == DetectionReason::StreamEndedEof
               ? RecoveryOutcome::RenewedEof : RecoveryOutcome::RenewedStall)
        : RecoveryOutcome::FaultDecided;
    return result;
}

SupervisorReduction reduce_deadline(const SupervisorState& state, TimePoint now,
                                    const RecoveryPolicy& policy) {
    if (state.name == SupervisorStateName::Zap && state.deadlines.steady_at &&
        now >= *state.deadlines.steady_at) {
        // The window cannot expire while the cache is holding playback back or
        // the last determinate health level is unhealthy. An interruption edge
        // alone cannot establish that: a sustained unhealthy run produces no
        // later edge to stop the deadline. Unknown telemetry is neutral and
        // leaves this retained level unchanged.
        if (state.cache_paused) {
            auto next = state;
            next.deadlines.steady_at = now + policy.steady_healthy_window;
            return settle(state, next, "steady-window-held-by-cache-pause", now, policy);
        }
        if (state.playback_unhealthy) {
            auto next = state;
            next.deadlines.steady_at = now + policy.steady_healthy_window;
            return settle(state, next, "steady-window-held-by-unhealthy-playback", now, policy);
        }
        auto next = state;
        next.attempt = 0;
        next.deadlines = {};
        next.detection.reset();
        next.name = SupervisorStateName::Steady;
        next.recovery.reset();
        next.recovery_started_at.reset();
        next.fault_decided_at.reset();
        next.first_frame_at.reset();
        next.short_recovery_load_failures = 0;
        next.last_short_recovery_failure_attempt.reset();
        next.short_load_recreation_used = false;
        next.late_completion_available = false;
        next.late_completion_probation = false;
        auto result = settle(state, next, "steady-confirmed", now, policy);
        result.transition->outcome = RecoveryOutcome::CleanProbation;
        result.transition->first_frame_to_outcome = elapsed_between(state.first_frame_at, now);
        if (state.load_intent != LoadIntent::FreshSelection) {
            result.transition->recovered_load_lifetime =
                elapsed_between(state.load_command_at, now);
        }
        result.transition->escalation = state.load_intent == LoadIntent::PlayerRecreation
            ? RecoveryEscalation::PlayerRecreation
            : (state.load_intent == LoadIntent::RecoveryReopen
                   ? RecoveryEscalation::SourceReopen : RecoveryEscalation::None);
        return result;
    }
    if (state.name == SupervisorStateName::Recovering && state.deadlines.retry_at &&
        now >= *state.deadlines.retry_at && state.recovery) {
        if (state.recovery_started_at &&
            now > *state.recovery_started_at + policy.wall_clock_budget) {
            return terminal(state, state.detection, FailureReason::BudgetExpired,
                            state.recovery_started_at, now, policy);
        }
        auto next = state;
        next.deadlines = {};
        next.pending_load_attempt = state.pending_load_attempt.value_or(
            LoadAttempt{state.load_attempt.value() + 1});
        next.pending_load_intent = *state.recovery == RecoveryAction::RecreatePlayer
            ? LoadIntent::PlayerRecreation : LoadIntent::RecoveryReopen;
        auto result = settle(state, next, "recovery-attempt-started", now, policy,
                             {{state.generation, *next.pending_load_attempt,
                               effect_payload(*state.recovery)}});
        result.transition->load_attempt = *next.pending_load_attempt;
        result.transition->load_intent = *next.pending_load_intent;
        result.transition->escalation = escalation_for(*state.recovery);
        return result;
    }
    return ignore(state);
}

RecoveryAction reopen_action(const SupervisorState& state) {
    return state.transport == RecoveryTransport::Hls
        ? RecoveryAction::ReloadHlsLive : RecoveryAction::ReopenStream;
}

}  // namespace

SupervisorState initial_supervisor_state() { return {}; }

bool supervisor_deadlines_valid(const SupervisorState& state) {
    if (state.deadlines.retry_at && state.deadlines.steady_at) return false;
    if (state.deadlines.retry_at && state.name != SupervisorStateName::Recovering) return false;
    if (state.deadlines.steady_at && state.name != SupervisorStateName::Zap) return false;
    return true;
}

bool supervisor_health_supervision_enabled(SupervisorStateName state) {
    return state != SupervisorStateName::Idle &&
           state != SupervisorStateName::Failed;
}

std::optional<TimePoint> next_deadline_at(const SupervisorState& state) {
    assert(supervisor_deadlines_valid(state));
    if (!state.deadlines.retry_at) return state.deadlines.steady_at;
    if (!state.deadlines.steady_at) return state.deadlines.retry_at;
    return std::min(*state.deadlines.retry_at, *state.deadlines.steady_at);
}

SupervisorReduction reduce_supervisor_state(const SupervisorState& state,
                                            const SupervisorEvent& event,
                                            TimePoint now,
                                            const RecoveryPolicy& policy) {
    assert(supervisor_deadlines_valid(state));
    if (std::holds_alternative<DeadlineReached>(event)) {
        return reduce_deadline(state, now, policy);
    }

    if (const auto* requested = std::get_if<ChannelRequested>(&event)) {
        if (requested->generation <= state.generation) return ignore(state);
        auto next = initial_supervisor_state();
        next.generation = requested->generation;
        next.name = SupervisorStateName::Loading;
        return settle(state, next, "channel-requested", now, policy);
    }
    if (const auto* stopped = std::get_if<PlaybackStopped>(&event)) {
        if (stopped->generation < state.generation) return ignore(state);
        auto next = initial_supervisor_state();
        next.generation = stopped->generation;
        return settle(state, next, "playback-stopped", now, policy);
    }

    const Generation generation = std::visit([](const auto& value) -> Generation {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, DeadlineReached>) return Generation{};
        else return value.generation;
    }, event);
    if (generation != state.generation) return ignore(state);

    return std::visit([&](const auto& value) -> SupervisorReduction {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, CacheState>) {
            if (value.load_attempt != state.load_attempt) return ignore(state);
            auto next = state;
            next.cache_paused = value.paused;
            // The window counts continuous clean playback, so both edges of a
            // fill restart it. Entering one interrupts the evidence, whether or
            // not the fold classified it as a new degradation edge; leaving one
            // is where clean playback actually begins, so the count starts
            // there rather than carrying credit for time spent filling. Without
            // the second rule a fill that clears just before the deadline is
            // confirmed almost immediately, and the oscillation at the end of
            // that same fill is charged as a rebuffer.
            //
            // A repeated observation of playing is not an edge and must not
            // restart anything, or a steady load could never confirm.
            if (state.name == SupervisorStateName::Zap && state.deadlines.steady_at &&
                (value.paused || state.cache_paused)) {
                next.deadlines.steady_at = now + policy.steady_healthy_window;
                return settle(state, next,
                              value.paused ? "cache-pause-restarted-steady-window"
                                           : "cache-resume-restarted-steady-window",
                              now, policy);
            }
            return settle(state, next, "cache-state-observed", now, policy);
        } else if constexpr (std::is_same_v<T, PlaybackHealthObserved>) {
            if (value.load_attempt != state.load_attempt) return ignore(state);
            if (state.playback_unhealthy == value.unhealthy) return ignore(state);
            auto next = state;
            next.playback_unhealthy = value.unhealthy;
            // Both edges restart the evidence window. Becoming unhealthy
            // invalidates what was counted; becoming healthy is the instant a
            // new continuous healthy interval can begin.
            if (state.name == SupervisorStateName::Zap && state.deadlines.steady_at) {
                next.deadlines.steady_at = now + policy.steady_healthy_window;
                return settle(state, next,
                              value.unhealthy ? "playback-unhealthy-restarted-steady-window"
                                              : "playback-health-restarted-steady-window",
                              now, policy);
            }
            return settle(state, next, "playback-health-observed", now, policy);
        } else if constexpr (std::is_same_v<T, ForwardProgressObserved>) {
            if (value.load_attempt != state.load_attempt) return ignore(state);
            auto next = state;
            next.last_forward_progress_at = now;
            return {.effects = {}, .state = std::move(next), .transition = std::nullopt};
        } else if constexpr (std::is_same_v<T, AuthRejected>) {
            if (value.load_attempt != state.load_attempt) return ignore(state);
            if (state.name == SupervisorStateName::Failed) {
                return retire_failed_late_completion(
                    state, "failed-load-auth-rejected", now, policy);
            }
            if (state.name == SupervisorStateName::Idle) return ignore(state);
            return terminal(state, state.detection, FailureReason::AuthRejected,
                            state.recovery_started_at, now, policy);
        } else if constexpr (std::is_same_v<T, FirstFrame>) {
            if (value.load_attempt != state.load_attempt) return ignore(state);
            const bool normal_first_frame = state.name == SupervisorStateName::Zap &&
                !state.first_frame_at;
            const bool cancels_opening_retry =
                state.name == SupervisorStateName::Recovering &&
                state.detection == DetectionReason::OpenStall && state.recovery &&
                source_reopen_action(*state.recovery) && state.deadlines.retry_at &&
                !state.pending_load_attempt;
            const bool revives_failed_current_load =
                state.name == SupervisorStateName::Failed &&
                state.late_completion_available;
            if (!normal_first_frame && !cancels_opening_retry &&
                !revives_failed_current_load) return ignore(state);
            auto next = state;
            next.deadlines = {.retry_at = std::nullopt,
                              .steady_at = now + policy.steady_healthy_window};
            next.first_frame_at = now;
            next.name = SupervisorStateName::Zap;
            // Any retained unhealthy level before this event described the
            // opening phase: no presentable frame existed yet. The accepted
            // exact-load frame resolves that condition. A determinate bad
            // post-frame sample can set the level again, while Unknown remains
            // neutral during probation as it did before the hard opening bound.
            next.playback_unhealthy = false;
            next.failure.reset();
            next.pending_load_attempt.reset();
            next.pending_load_intent.reset();
            next.recovery.reset();
            next.late_completion_available = false;
            next.late_completion_probation = revives_failed_current_load;
            const bool late = cancels_opening_retry || revives_failed_current_load;
            auto result = settle(
                state, next,
                revives_failed_current_load
                    ? "late-first-frame-after-command-exhaustion"
                    : (cancels_opening_retry
                           ? "first-frame-cancelled-opening-retry"
                           : "first-frame"),
                now, policy);
            result.transition->outcome = late ? RecoveryOutcome::LateFirstFrame
                                              : RecoveryOutcome::FirstFrame;
            result.transition->command_to_first_frame =
                elapsed_between(state.load_command_at, now);
            result.transition->escalation = state.load_intent == LoadIntent::PlayerRecreation
                ? RecoveryEscalation::PlayerRecreation
                : (state.load_intent == LoadIntent::RecoveryReopen
                       ? RecoveryEscalation::SourceReopen : RecoveryEscalation::None);
            return result;
        } else if constexpr (std::is_same_v<T, DecodeStalled>) {
            if (value.load_attempt != state.load_attempt) return ignore(state);
            return recover(state, DetectionReason::DecodeStall, reopen_action(state), now, policy);
        } else if constexpr (std::is_same_v<T, PlaybackStalled>) {
            if (value.load_attempt != state.load_attempt) return ignore(state);
            const DetectionReason detection = value.stall == StallKind::Cache
                ? DetectionReason::CacheStall
                : (value.stall == StallKind::Open ? DetectionReason::OpenStall
                                                  : DetectionReason::ProgressStall);
            return recover(state, detection, reopen_action(state), now, policy);
        } else if constexpr (std::is_same_v<T, IpcUnresponsive>) {
            if (value.load_attempt != state.load_attempt) return ignore(state);
            if (state.name == SupervisorStateName::Failed) {
                return retire_failed_late_completion(
                    state, "failed-load-ipc-unresponsive", now, policy);
            }
            return recover(state, DetectionReason::IpcUnresponsive,
                           RecoveryAction::RecreatePlayer, now, policy);
        } else if constexpr (std::is_same_v<T, PresentationLost>) {
            // The surface is rebuilt by the platform adapter before this
            // arrives; what remains is a libmpv instance holding a dead D3D11
            // device, which only recreation clears.
            if (state.name == SupervisorStateName::Failed) {
                return retire_failed_late_completion(
                    state, "failed-load-presentation-lost", now, policy);
            }
            return recover(state, DetectionReason::PresentationDeviceLost,
                           RecoveryAction::RecreatePlayer, now, policy);
        } else if constexpr (std::is_same_v<T, ProcessExited>) {
            if (value.load_attempt != state.load_attempt) return ignore(state);
            if (state.name == SupervisorStateName::Failed) {
                return retire_failed_late_completion(
                    state, "failed-load-process-exited", now, policy);
            }
            return recover(state, DetectionReason::ProcessExited,
                           RecoveryAction::RecreatePlayer, now, policy);
        } else if constexpr (std::is_same_v<T, StreamEnded>) {
            if (value.load_attempt != state.load_attempt) return ignore(state);
            if (state.name == SupervisorStateName::Failed) {
                return retire_failed_late_completion(
                    state, "failed-load-ended", now, policy);
            }
            const DetectionReason detection =
                stream_end_detection(value.end_reason, value.failure_reason);
            const RecoveryAction action =
                value.failure_reason == TransportFailureReason::FormatProbeRequired
                    ? RecoveryAction::ReopenProbed : reopen_action(state);
            return recover(state, detection, action, now, policy);
        } else if constexpr (std::is_same_v<T, SourceFailed>) {
            const bool current = value.load_attempt == state.load_attempt;
            const bool pending = state.pending_load_attempt &&
                value.load_attempt == *state.pending_load_attempt;
            const bool fresh_failed = state.name == SupervisorStateName::Loading &&
                value.load_attempt == LoadAttempt{1};
            if (!current && !pending && !fresh_failed) return ignore(state);
            if (state.name == SupervisorStateName::Failed) {
                return retire_failed_late_completion(
                    state, "failed-load-source-unavailable", now, policy);
            }
            if (state.name == SupervisorStateName::Idle) return ignore(state);
            return terminal(state, state.detection, FailureReason::SourceUnavailable,
                            state.recovery_started_at, now, policy);
        } else if constexpr (std::is_same_v<T, StreamLoadIssued>) {
            const bool fresh = state.name == SupervisorStateName::Loading &&
                value.load_attempt == LoadAttempt{1} &&
                value.intent == LoadIntent::FreshSelection;
            const bool recovery = state.name == SupervisorStateName::Recovering &&
                state.pending_load_attempt == value.load_attempt &&
                state.pending_load_intent == value.intent;
            if (!fresh && !recovery) return ignore(state);
            auto next = state;
            next.deadlines = {};
            next.first_frame_at.reset();
            next.cache_paused = false;
            next.playback_unhealthy = false;
            next.last_forward_progress_at.reset();
            next.load_attempt = value.load_attempt;
            next.load_intent = value.intent;
            next.load_command_at = now;
            next.name = SupervisorStateName::Zap;
            next.pending_load_attempt.reset();
            next.pending_load_intent.reset();
            next.late_completion_available = false;
            next.late_completion_probation = false;
            next.transport = value.transport;
            auto result = settle(state, next, "stream-load-issued", now, policy);
            result.transition->outcome = RecoveryOutcome::CommandIssued;
            result.transition->decision_to_command =
                elapsed_between(state.fault_decided_at, now);
            result.transition->escalation = value.intent == LoadIntent::PlayerRecreation
                ? RecoveryEscalation::PlayerRecreation :
                  (value.intent == LoadIntent::RecoveryReopen
                       ? RecoveryEscalation::SourceReopen : RecoveryEscalation::None);
            return result;
        } else if constexpr (std::is_same_v<T, DeadlineReached> ||
                             std::is_same_v<T, ChannelRequested> ||
                             std::is_same_v<T, PlaybackStopped>) {
            // Handled before visitation.
            return ignore(state);
        } else {
            static_assert(kAlwaysFalse<T>, "SupervisorEvent handling must be exhaustive");
        }
    }, event);
}

SupervisorStatsSnapshot project_supervisor_stats(const SupervisorState& state,
                                                  TimePoint now,
                                                  const RecoveryPolicy& policy) {
    SupervisorStatsSnapshot result;
    result.attempt = state.attempt;
    result.attempt_ceiling = policy.attempt_delays.size();
    result.elapsed_budget = state.recovery_started_at
        ? std::optional<Duration>{std::max(Duration::zero(), now - *state.recovery_started_at)}
        : std::nullopt;
    result.policy_version = policy.version;
    if (state.failure) result.reason = to_string(*state.failure);
    else if (state.detection) result.reason = to_string(*state.detection);
    result.state = state.name;
    result.transport = state.transport;
    result.load_attempt = state.load_attempt;
    result.load_intent = state.load_intent;
    result.short_load_recreation_used = state.short_load_recreation_used;
    return result;
}

const char* to_string(SupervisorStateName value) {
    switch (value) {
        case SupervisorStateName::Idle: return "idle";
        case SupervisorStateName::Loading: return "loading";
        case SupervisorStateName::Zap: return "zap";
        case SupervisorStateName::Steady: return "steady";
        case SupervisorStateName::Recovering: return "recovering";
        case SupervisorStateName::Failed: return "failed";
    }
    return "idle";
}
const char* to_string(RecoveryTransport value) {
    switch (value) {
        case RecoveryTransport::MpegTs: return "mpeg-ts";
        case RecoveryTransport::Hls: return "hls";
    }
    return "mpeg-ts";
}
const char* to_string(RecoveryAction value) {
    switch (value) {
        case RecoveryAction::ReopenStream: return "reopen-stream";
        case RecoveryAction::ReloadHlsLive: return "reload-hls-live";
        case RecoveryAction::ReopenProbed: return "reopen-probed";
        case RecoveryAction::RecreatePlayer: return "recreate-player";
    }
    return "reopen-stream";
}
const char* to_string(RecoveryEscalation value) {
    switch (value) {
        case RecoveryEscalation::None: return "none";
        case RecoveryEscalation::SourceReopen: return "source-reopen";
        case RecoveryEscalation::PlayerRecreation: return "player-recreation";
    }
    return "none";
}
const char* to_string(RecoveryOutcome value) {
    switch (value) {
        case RecoveryOutcome::None: return "none";
        case RecoveryOutcome::FaultDecided: return "fault-decided";
        case RecoveryOutcome::CommandIssued: return "command-issued";
        case RecoveryOutcome::FirstFrame: return "first-frame";
        case RecoveryOutcome::LateFirstFrame: return "late-first-frame";
        case RecoveryOutcome::CleanProbation: return "clean-probation";
        case RecoveryOutcome::RenewedStall: return "renewed-stall";
        case RecoveryOutcome::RenewedEof: return "renewed-eof";
        case RecoveryOutcome::TerminalFailure: return "terminal-failure";
    }
    return "none";
}
const char* to_string(LoadIntent value) {
    switch (value) {
        case LoadIntent::FreshSelection: return "fresh-selection";
        case LoadIntent::RecoveryReopen: return "recovery-reopen";
        case LoadIntent::PlayerRecreation: return "player-recreation";
    }
    return "fresh-selection";
}
const char* to_string(DetectionReason value) {
    switch (value) {
        case DetectionReason::CacheStall: return "cache-stall";
        case DetectionReason::DecodeStall: return "decode-stall";
        case DetectionReason::FormatProbeRequired: return "format-probe-required";
        case DetectionReason::HlsPlaylistFailed: return "hls-playlist-failed";
        case DetectionReason::HlsSegmentUnavailable: return "hls-segment-unavailable";
        case DetectionReason::HttpRequestTimeout: return "http-request-timeout";
        case DetectionReason::IpcUnresponsive: return "ipc-unresponsive";
        case DetectionReason::OpenStall: return "open-stall";
        case DetectionReason::PresentationDeviceLost: return "presentation-device-lost";
        case DetectionReason::ProcessExited: return "process-exited";
        case DetectionReason::ProgressStall: return "progress-stall";
        case DetectionReason::StreamEndedEof: return "stream-ended-eof";
        case DetectionReason::StreamEndedError: return "stream-ended-error";
        case DetectionReason::StreamEndedUnknown: return "stream-ended-unknown";
    }
    return "stream-ended-unknown";
}
const char* to_string(FailureReason value) {
    switch (value) {
        case FailureReason::AttemptsExhausted: return "attempts-exhausted";
        case FailureReason::AuthRejected: return "auth-rejected";
        case FailureReason::BudgetExpired: return "budget-expired";
        case FailureReason::SourceUnavailable: return "source-unavailable";
    }
    return "source-unavailable";
}
const char* effect_name(const SupervisorEffectPayload& value) {
    return std::visit([](const auto& effect) -> const char* {
        using T = std::decay_t<decltype(effect)>;
        if constexpr (std::is_same_v<T, ReopenStream>) return "reopen-stream";
        else if constexpr (std::is_same_v<T, ReloadHlsLive>) return "reload-hls-live";
        else if constexpr (std::is_same_v<T, ReopenProbed>) return "reopen-probed";
        else if constexpr (std::is_same_v<T, RecreatePlayer>) return "recreate-player";
        else static_assert(kAlwaysFalse<T>, "SupervisorEffect handling must be exhaustive");
    }, value);
}

}  // namespace coax::core
