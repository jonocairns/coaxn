#pragma once

#include <optional>

#include "core/playback_types.hpp"
#include "core/policy.hpp"

namespace coax::core {

enum class PlaybackHealthVerdict { Unknown, Healthy, Degraded, OpenStalled, Stalled };
enum class PlaybackDegradedReason { DecodeDamage, Discontinuity, InputThrottled, Unclassified };

struct PlaybackHealthObservation {
    std::optional<double> av_sync_seconds;
    std::optional<double> buffer_seconds;
    std::optional<double> cache_end_seconds;
    bool cache_paused = false;
    std::optional<double> input_rate_bytes_per_second;
    std::optional<double> ipc_round_trip_ms;
    std::optional<double> playback_time_seconds;
    std::optional<double> video_fps_estimate;
};

struct BufferHealthSnapshot {
    std::optional<double> av_sync_seconds;
    std::optional<double> buffer_seconds;
    std::optional<double> buffer_target_seconds;
    bool cache_paused = false;
    std::optional<PlaybackDegradedReason> degraded_reason;
    int discontinuities = 0;
    std::optional<bool> input_advancing;
    std::optional<double> input_rate_bytes_per_second;
    std::optional<double> input_realtime_ratio;
    std::optional<double> ipc_round_trip_ms;
    std::optional<double> main_process_cpu_percent;
    std::optional<TimePoint> observed_at;
    std::optional<BufferPhase> phase;
    std::optional<bool> progressing;
    std::optional<Duration> stalled_for;
    PlaybackHealthVerdict verdict = PlaybackHealthVerdict::Unknown;
    std::optional<double> video_fps_shortfall;
};

struct PlaybackHealthState {
    std::optional<double> cache_end_seconds;
    std::optional<TimePoint> decode_since;
    int decode_observations = 0;
    int discontinuities = 0;
    TimePoint load_issued_at{};
    int observations = 0;
    std::optional<TimePoint> observed_at;
    std::optional<double> playback_time_seconds;
    std::optional<TimePoint> since;
    BufferHealthSnapshot snapshot;
    PlaybackHealthVerdict verdict = PlaybackHealthVerdict::Unknown;
    std::optional<double> video_fps_estimate;
};

struct PlaybackHealthFoldOptions {
    std::optional<double> container_fps;
    bool first_frame_seen = false;
    std::optional<double> main_process_cpu_percent;
    BufferPhase phase = BufferPhase::Zap;
    std::optional<double> target_seconds;
    HealthPolicy policy = kDefaultHealthPolicy;
};

struct PlaybackHealthFold {
    bool decode_stalled = false;
    bool discontinuity = false;
    bool interrupted = false;
    PlaybackHealthState state;
    bool stalled = false;
};

PlaybackHealthState initial_playback_health(
    BufferPhase phase, TimePoint load_issued_at,
    std::optional<double> target_seconds = std::nullopt);

PlaybackHealthFold fold_playback_health(
    const PlaybackHealthState& previous,
    const PlaybackHealthObservation& observation,
    TimePoint now,
    const PlaybackHealthFoldOptions& options);

const char* to_string(PlaybackHealthVerdict value);
const char* to_string(PlaybackDegradedReason value);

}  // namespace coax::core
