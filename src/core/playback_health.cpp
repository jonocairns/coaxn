#include "core/playback_health.hpp"

#include <algorithm>
#include <cmath>

namespace coax::core {
namespace {

bool is_depleted(const PlaybackHealthObservation& observation,
                 const HealthPolicy& policy) {
    return observation.cache_paused ||
           (observation.buffer_seconds &&
            *observation.buffer_seconds <= policy.depleted_buffer_seconds);
}

bool input_is_delivering(std::optional<double> ratio) {
    return ratio && *ratio > 0.0;
}

std::optional<double> difference(std::optional<double> previous,
                                 std::optional<double> current) {
    if (!previous || !current) return std::nullopt;
    return *current - *previous;
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

PlaybackHealthState initial_playback_health(Generation generation, BufferPhase phase,
                                            TimePoint load_issued_at,
                                            std::optional<double> target_seconds) {
    PlaybackHealthState state;
    state.generation = generation;
    state.load_issued_at = load_issued_at;
    state.snapshot.phase = phase;
    state.snapshot.buffer_target_seconds = target_seconds;
    state.snapshot.timeline.generation = generation;
    return state;
}

PlaybackHealthFold fold_playback_health(const PlaybackHealthState& previous,
                                        const PlaybackHealthObservation& observation,
                                        TimePoint now,
                                        const PlaybackHealthFoldOptions& options) {
    if (observation.generation != previous.generation) {
        return PlaybackHealthFold{
            .observation_accepted = false,
            .state = previous,
        };
    }
    const auto& policy = options.policy;
    const std::optional<Duration> elapsed = previous.observed_at
        ? std::optional<Duration>{now - *previous.observed_at} : std::nullopt;
    TimelineEvidence timeline;
    timeline.generation = previous.generation;
    timeline.elapsed_seconds = elapsed
        ? std::optional<double>{std::max(0.0, elapsed->count())} : std::nullopt;
    timeline.playback_movement_seconds = difference(
        previous.previous_sample_playback_time_seconds,
        observation.playback_time_seconds);
    if (timeline.playback_movement_seconds && timeline.elapsed_seconds) {
        timeline.playback_deviation_seconds =
            *timeline.playback_movement_seconds - *timeline.elapsed_seconds;
    }
    timeline.cache_end_movement_seconds = difference(
        previous.previous_sample_cache_end_seconds,
        observation.cache_end_seconds);
    timeline.previous_cache_paused = previous.previous_cache_paused;
    timeline.cache_paused = observation.cache_paused;

    const auto control_playback_movement = difference(
        previous.playback_time_seconds, observation.playback_time_seconds);
    const auto control_cache_end_movement = difference(
        previous.cache_end_seconds, observation.cache_end_seconds);
    const auto progressing = control_playback_movement
        ? std::optional<bool>{*control_playback_movement >
                              policy.progress_epsilon_seconds}
        : std::nullopt;
    const auto input_advancing = control_cache_end_movement
        ? std::optional<bool>{*control_cache_end_movement >
                              policy.progress_epsilon_seconds}
        : std::nullopt;

    std::optional<double> input_ratio;
    if (control_cache_end_movement && timeline.elapsed_seconds &&
        *timeline.elapsed_seconds > 0.0) {
        // Delivery rate remains a deliberately nonnegative derived control
        // input. The signed cache movement above is retained separately.
        input_ratio = std::max(0.0, *control_cache_end_movement /
                                     *timeline.elapsed_seconds);
    }

    const std::optional<double> control_playback_deviation =
        control_playback_movement && timeline.elapsed_seconds
            ? std::optional<double>{*control_playback_movement -
                                    *timeline.elapsed_seconds}
            : std::nullopt;
    timeline.control_playback_movement_seconds = control_playback_movement;
    timeline.control_playback_deviation_seconds = control_playback_deviation;
    if (control_playback_movement) {
        timeline.control_baseline_retained =
            !previous.previous_sample_playback_time_seconds;
    }
    const bool discontinuity = control_playback_deviation &&
        std::abs(*control_playback_deviation) >
            policy.discontinuity_jump_seconds;
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
    state.generation = previous.generation;
    state.load_issued_at = previous.load_issued_at;
    state.observations = observations;
    state.observed_at = now;
    state.previous_sample_cache_end_seconds = observation.cache_end_seconds;
    state.previous_sample_playback_time_seconds = observation.playback_time_seconds;
    state.playback_time_seconds = observation.playback_time_seconds
        ? observation.playback_time_seconds : previous.playback_time_seconds;
    state.previous_cache_paused = observation.cache_paused;
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
    state.snapshot.timeline = timeline;
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
