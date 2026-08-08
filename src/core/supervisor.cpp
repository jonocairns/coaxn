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
        .policy_version = policy.version,
        .reason = std::move(reason),
        .to = next.name,
        .transport = next.transport,
        .transport_policy_version = kTransportPolicyVersion,
    };
    return {.effects = std::move(effects), .state = std::move(next),
            .transition = std::move(transition)};
}

SupervisorReduction terminal(const SupervisorState& state,
                             std::optional<DetectionReason> detection,
                             FailureReason failure,
                             std::optional<TimePoint> recovery_started_at,
                             TimePoint now, const RecoveryPolicy& policy) {
    auto next = state;
    next.deadlines = {};
    next.detection = detection;
    next.failure = failure;
    next.first_frame_at.reset();
    next.name = SupervisorStateName::Failed;
    next.recovery.reset();
    next.recovery_started_at = recovery_started_at;
    return settle(state, next, to_string(failure), now, policy);
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

SupervisorReduction recover(const SupervisorState& state, DetectionReason detection,
                            RecoveryAction action, TimePoint now,
                            const RecoveryPolicy& policy) {
    if (state.name == SupervisorStateName::Idle || state.name == SupervisorStateName::Failed) {
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
    ++next.attempt;
    next.deadlines = {.retry_at = retry_at, .steady_at = std::nullopt};
    next.detection = detection;
    next.failure.reset();
    next.first_frame_at.reset();
    next.name = SupervisorStateName::Recovering;
    next.recovery = action;
    next.recovery_started_at = started;
    return settle(state, next, to_string(detection), now, policy);
}

SupervisorReduction reduce_deadline(const SupervisorState& state, TimePoint now,
                                    const RecoveryPolicy& policy) {
    if (state.name == SupervisorStateName::Zap && state.deadlines.steady_at &&
        now >= *state.deadlines.steady_at) {
        // The window is evidence of continuous healthy playback, so it cannot
        // expire while the cache is holding playback back or the health fold's
        // current verdict is not Healthy. An interruption edge alone cannot
        // establish that: a sustained unhealthy run produces no later edge to
        // stop the deadline.
        if (state.cache_paused) {
            auto next = state;
            next.deadlines.steady_at = now + policy.steady_healthy_window;
            return settle(state, next, "steady-window-held-by-cache-pause", now, policy);
        }
        if (!state.playback_healthy) {
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
        return settle(state, next, "steady-confirmed", now, policy);
    }
    if (state.name == SupervisorStateName::Recovering && state.deadlines.retry_at &&
        now >= *state.deadlines.retry_at && state.recovery) {
        auto next = state;
        next.deadlines = {};
        return settle(state, next, "recovery-attempt-started", now, policy,
                      {{state.generation, effect_payload(*state.recovery)}});
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
            if (state.playback_healthy == value.healthy) return ignore(state);
            auto next = state;
            next.playback_healthy = value.healthy;
            // Both edges restart the evidence window. Becoming unhealthy
            // invalidates what was counted; becoming healthy is the instant a
            // new continuous healthy interval can begin.
            if (state.name == SupervisorStateName::Zap && state.deadlines.steady_at) {
                next.deadlines.steady_at = now + policy.steady_healthy_window;
                return settle(state, next,
                              value.healthy ? "playback-health-restarted-steady-window"
                                            : "playback-unhealthy-restarted-steady-window",
                              now, policy);
            }
            return settle(state, next, "playback-health-observed", now, policy);
        } else if constexpr (std::is_same_v<T, AuthRejected>) {
            if (state.name == SupervisorStateName::Idle) return ignore(state);
            return terminal(state, state.detection, FailureReason::AuthRejected,
                            state.recovery_started_at, now, policy);
        } else if constexpr (std::is_same_v<T, FirstFrame>) {
            if (state.name != SupervisorStateName::Zap) return ignore(state);
            auto next = state;
            next.deadlines = {.retry_at = std::nullopt,
                              .steady_at = now + policy.steady_healthy_window};
            next.first_frame_at = now;
            return settle(state, next, "first-frame", now, policy);
        } else if constexpr (std::is_same_v<T, DecodeStalled>) {
            return recover(state, DetectionReason::DecodeStall, reopen_action(state), now, policy);
        } else if constexpr (std::is_same_v<T, PlaybackStalled>) {
            return recover(state,
                           value.stall == StallKind::Open ? DetectionReason::OpenStall
                                                          : DetectionReason::ProgressStall,
                           reopen_action(state), now, policy);
        } else if constexpr (std::is_same_v<T, IpcUnresponsive>) {
            return recover(state, DetectionReason::IpcUnresponsive,
                           RecoveryAction::RecreatePlayer, now, policy);
        } else if constexpr (std::is_same_v<T, PresentationLost>) {
            // The surface is rebuilt by the platform adapter before this
            // arrives; what remains is a libmpv instance holding a dead D3D11
            // device, which only recreation clears.
            return recover(state, DetectionReason::PresentationDeviceLost,
                           RecoveryAction::RecreatePlayer, now, policy);
        } else if constexpr (std::is_same_v<T, ProcessExited>) {
            return recover(state, DetectionReason::ProcessExited,
                           RecoveryAction::RecreatePlayer, now, policy);
        } else if constexpr (std::is_same_v<T, StreamEnded>) {
            const DetectionReason detection =
                stream_end_detection(value.end_reason, value.failure_reason);
            const RecoveryAction action =
                value.failure_reason == TransportFailureReason::FormatProbeRequired
                    ? RecoveryAction::ReopenProbed : reopen_action(state);
            return recover(state, detection, action, now, policy);
        } else if constexpr (std::is_same_v<T, SourceFailed>) {
            if (state.name == SupervisorStateName::Idle) return ignore(state);
            return terminal(state, state.detection, FailureReason::SourceUnavailable,
                            state.recovery_started_at, now, policy);
        } else if constexpr (std::is_same_v<T, StreamLoadIssued>) {
            if (state.name != SupervisorStateName::Loading &&
                state.name != SupervisorStateName::Recovering) return ignore(state);
            auto next = state;
            next.deadlines = {};
            next.first_frame_at.reset();
            next.cache_paused = false;
            next.playback_healthy = false;
            next.name = SupervisorStateName::Zap;
            next.transport = value.transport;
            return settle(state, next, "stream-load-issued", now, policy);
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
const char* to_string(DetectionReason value) {
    switch (value) {
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
