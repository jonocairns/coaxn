#include "core/playback_health.hpp"

#include <algorithm>
#include <cmath>

namespace coax::core {
namespace {

std::optional<bool> advanced(std::optional<double> previous,
                             std::optional<double> current, double epsilon) {
    if (!previous || !current) return std::nullopt;
    return *current - *previous > epsilon;
}

bool is_depleted(const PlaybackHealthObservation& observation,
                 const HealthPolicy& policy) {
    return observation.cache_paused ||
           (observation.buffer_seconds &&
            *observation.buffer_seconds <= policy.depleted_buffer_seconds);
}

bool input_is_delivering(std::optional<double> ratio) {
    return ratio && *ratio > 0.0;
}

bool is_discontinuous(std::optional<double> previous_playback,
                      std::optional<double> playback,
                      std::optional<Duration> elapsed,
                      const HealthPolicy& policy) {
    if (!previous_playback || !playback || !elapsed) return false;
    const double expected = std::max(0.0, elapsed->count());
    const double moved = *playback - *previous_playback;
    return std::abs(moved - expected) > policy.discontinuity_jump_seconds;
}

std::optional<double> video_fps_shortfall(std::optional<double> estimate,
                                          std::optional<double> container) {
    if (!estimate || !container || *container <= 0.0) return std::nullopt;
    return std::clamp((*container - *estimate) / *container, 0.0, 1.0);
}

PlaybackDegradedReason classify_degraded(bool discontinuity,
                                         std::optional<double> input_ratio,
                                         std::optional<double> shortfall,
                                         const HealthPolicy& policy) {
    if (input_is_delivering(input_ratio) && *input_ratio < policy.throttled_input_ratio) {
        return PlaybackDegradedReason::InputThrottled;
    }
    if (discontinuity) return PlaybackDegradedReason::Discontinuity;
    if (shortfall && *shortfall > policy.decode_shortfall_ratio) {
        return PlaybackDegradedReason::DecodeDamage;
    }
    return PlaybackDegradedReason::Unclassified;
}

double rounded(double value, double places) {
    return std::round(value * places) / places;
}

}  // namespace

PlaybackHealthState initial_playback_health(BufferPhase phase, TimePoint load_issued_at,
                                            std::optional<double> target_seconds) {
    PlaybackHealthState state;
    state.load_issued_at = load_issued_at;
    state.snapshot.phase = phase;
    state.snapshot.buffer_target_seconds = target_seconds;
    return state;
}

PlaybackHealthFold fold_playback_health(const PlaybackHealthState& previous,
                                        const PlaybackHealthObservation& observation,
                                        TimePoint now,
                                        const PlaybackHealthFoldOptions& options) {
    const auto& policy = options.policy;
    const std::optional<Duration> elapsed = previous.observed_at
        ? std::optional<Duration>{now - *previous.observed_at} : std::nullopt;
    const auto progressing = advanced(previous.playback_time_seconds,
                                      observation.playback_time_seconds,
                                      policy.progress_epsilon_seconds);
    const auto input_advancing = advanced(previous.cache_end_seconds,
                                          observation.cache_end_seconds,
                                          policy.progress_epsilon_seconds);

    std::optional<double> input_ratio;
    if (previous.cache_end_seconds && observation.cache_end_seconds &&
        elapsed && elapsed->count() > 0.0) {
        input_ratio = std::max(0.0, (*observation.cache_end_seconds -
                                     *previous.cache_end_seconds) / elapsed->count());
    }

    const bool discontinuity = is_discontinuous(previous.playback_time_seconds,
                                                observation.playback_time_seconds,
                                                elapsed, policy);
    const auto fps_estimate = observation.video_fps_estimate
        ? observation.video_fps_estimate : previous.video_fps_estimate;
    const auto av_sync = observation.av_sync_seconds
        ? observation.av_sync_seconds : previous.snapshot.av_sync_seconds;
    const auto shortfall = video_fps_shortfall(fps_estimate, options.container_fps);

    PlaybackHealthVerdict verdict;
    if (!options.first_frame_seen) {
        verdict = !observation.playback_time_seconds && !input_is_delivering(input_ratio) &&
                          (!observation.buffer_seconds ||
                           *observation.buffer_seconds <= policy.depleted_buffer_seconds)
                      ? PlaybackHealthVerdict::OpenStalled
                      : PlaybackHealthVerdict::Unknown;
    } else if (!progressing) {
        verdict = PlaybackHealthVerdict::Unknown;
    } else if (*progressing) {
        verdict = PlaybackHealthVerdict::Healthy;
    } else if (is_depleted(observation, policy) && !input_is_delivering(input_ratio)) {
        verdict = PlaybackHealthVerdict::Stalled;
    } else {
        verdict = PlaybackHealthVerdict::Degraded;
    }

    std::optional<PlaybackDegradedReason> degraded_reason;
    if (verdict == PlaybackHealthVerdict::Degraded) {
        degraded_reason = classify_degraded(discontinuity, input_ratio, shortfall, policy);
    }

    const bool continuing = verdict == previous.verdict;
    const int observations = continuing ? previous.observations + 1 : 1;
    std::optional<TimePoint> since;
    if (verdict == PlaybackHealthVerdict::OpenStalled) {
        since = previous.load_issued_at;
    } else if (verdict == PlaybackHealthVerdict::Stalled) {
        since = continuing && previous.since ? previous.since : std::optional<TimePoint>{now};
    }
    const std::optional<Duration> stalled_for = since
        ? std::optional<Duration>{std::max(Duration::zero(), now - *since)} : std::nullopt;

    const bool decode_wedged = verdict == PlaybackHealthVerdict::Degraded &&
        degraded_reason != PlaybackDegradedReason::InputThrottled;
    const std::optional<TimePoint> decode_since = decode_wedged
        ? (previous.decode_since ? previous.decode_since : std::optional<TimePoint>{now})
        : std::nullopt;
    const int decode_observations = decode_wedged ? previous.decode_observations + 1 : 0;
    const bool decode_stalled = decode_since &&
        now - *decode_since >= policy.decode_stall_confirmation &&
        decode_observations >= policy.min_decode_stall_observations;

    const bool open = verdict == PlaybackHealthVerdict::OpenStalled;
    const bool stalled = stalled_for &&
        *stalled_for >= (open ? policy.open_stall_confirmation
                             : policy.stall_confirmation) &&
        observations >= (open ? policy.min_open_stall_observations
                              : policy.min_stall_observations);

    PlaybackHealthState state;
    state.cache_end_seconds = observation.cache_end_seconds
        ? observation.cache_end_seconds : previous.cache_end_seconds;
    state.decode_since = decode_since;
    state.decode_observations = decode_observations;
    state.discontinuities = previous.discontinuities + (discontinuity ? 1 : 0);
    state.load_issued_at = previous.load_issued_at;
    state.observations = observations;
    state.observed_at = now;
    state.playback_time_seconds = observation.playback_time_seconds
        ? observation.playback_time_seconds : previous.playback_time_seconds;
    state.since = since;
    state.verdict = verdict;
    state.video_fps_estimate = fps_estimate;
    state.snapshot.av_sync_seconds = av_sync;
    state.snapshot.buffer_seconds = observation.buffer_seconds;
    state.snapshot.buffer_target_seconds = options.target_seconds;
    state.snapshot.cache_paused = observation.cache_paused;
    state.snapshot.degraded_reason = degraded_reason;
    state.snapshot.discontinuities = state.discontinuities;
    state.snapshot.input_advancing = input_advancing;
    state.snapshot.input_rate_bytes_per_second = observation.input_rate_bytes_per_second;
    state.snapshot.input_realtime_ratio = input_ratio
        ? std::optional<double>{rounded(*input_ratio, 100.0)} : std::nullopt;
    state.snapshot.ipc_round_trip_ms = observation.ipc_round_trip_ms;
    state.snapshot.main_process_cpu_percent = options.main_process_cpu_percent;
    state.snapshot.observed_at = now;
    state.snapshot.phase = options.phase;
    state.snapshot.progressing = progressing;
    state.snapshot.stalled_for = stalled_for;
    state.snapshot.verdict = verdict;
    state.snapshot.video_fps_shortfall = shortfall
        ? std::optional<double>{rounded(*shortfall, 1000.0)} : std::nullopt;

    return PlaybackHealthFold{
        .decode_stalled = decode_stalled,
        .discontinuity = discontinuity,
        .state = state,
        .stalled = stalled,
    };
}

const char* to_string(PlaybackHealthVerdict value) {
    switch (value) {
        case PlaybackHealthVerdict::Unknown: return "unknown";
        case PlaybackHealthVerdict::Healthy: return "healthy";
        case PlaybackHealthVerdict::Degraded: return "degraded";
        case PlaybackHealthVerdict::OpenStalled: return "open-stalled";
        case PlaybackHealthVerdict::Stalled: return "stalled";
    }
    return "unknown";
}

const char* to_string(PlaybackDegradedReason value) {
    switch (value) {
        case PlaybackDegradedReason::DecodeDamage: return "decode-damage";
        case PlaybackDegradedReason::Discontinuity: return "discontinuity";
        case PlaybackDegradedReason::InputThrottled: return "input-throttled";
        case PlaybackDegradedReason::Unclassified: return "unclassified";
    }
    return "unclassified";
}

}  // namespace coax::core
