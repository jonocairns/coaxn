#pragma once

#include <optional>

#include "core/playback_types.hpp"
#include "core/policy.hpp"

namespace coax::core {

enum class PlaybackHealthVerdict { Unknown, Healthy, Degraded, OpenStalled, Stalled };
enum class PlaybackDegradedReason { DecodeDamage, Discontinuity, InputThrottled, Unclassified };

struct PlaybackHealthObservation {
    Generation generation;
    std::optional<double> av_sync_seconds;
    std::optional<double> buffer_seconds;
    std::optional<double> cache_end_seconds;
    bool cache_paused = false;
    std::optional<double> input_rate_bytes_per_second;
    std::optional<double> ipc_round_trip_ms;
    std::optional<double> playback_time_seconds;
    std::optional<double> video_fps_estimate;
};

// Raw, policy-free evidence from two adjacent health samples. Movement is
// current minus previous. Playback deviation is movement minus elapsed
// monotonic time, so a jump ahead is positive and a lag or jump backward is
// negative. Every value remains absent unless the source samples needed to
// compute it were present; absence is never rewritten as zero.
struct TimelineEvidence {
    Generation generation;
    std::optional<double> elapsed_seconds;
    std::optional<double> playback_movement_seconds;
    std::optional<double> playback_deviation_seconds;
    std::optional<double> cache_end_movement_seconds;
    std::optional<bool> previous_cache_paused;
    bool cache_paused = false;
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
    TimelineEvidence timeline;
    PlaybackHealthVerdict verdict = PlaybackHealthVerdict::Unknown;
    std::optional<double> video_fps_shortfall;
};

struct PlaybackHealthState {
    std::optional<double> cache_end_seconds;
    std::optional<TimePoint> decode_since;
    int decode_observations = 0;
    int discontinuities = 0;
    Generation generation;
    TimePoint load_issued_at{};
    int observations = 0;
    std::optional<TimePoint> observed_at;
    // Exact preceding sample values for raw adjacent-sample evidence. The
    // control baselines above/below deliberately retain their last usable
    // values across an unreadable sample, preserving the health policy that
    // predates TimelineEvidence.
    std::optional<double> previous_sample_cache_end_seconds;
    std::optional<double> previous_sample_playback_time_seconds;
    std::optional<double> playback_time_seconds;
    std::optional<bool> previous_cache_paused;
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
    bool observation_accepted = true;
    PlaybackHealthState state;
    bool stalled = false;
};

PlaybackHealthState initial_playback_health(
    Generation generation, BufferPhase phase, TimePoint load_issued_at,
    std::optional<double> target_seconds = std::nullopt);

PlaybackHealthFold fold_playback_health(
    const PlaybackHealthState& previous,
    const PlaybackHealthObservation& observation,
    TimePoint now,
    const PlaybackHealthFoldOptions& options);

const char* to_string(PlaybackHealthVerdict value);
const char* to_string(PlaybackDegradedReason value);

}  // namespace coax::core
