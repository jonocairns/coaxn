#pragma once

#include <array>
#include <cstddef>
#include <string_view>

#include "core/playback_types.hpp"

namespace coax::core {

inline constexpr std::string_view kTransportPolicyVersion = "coax-transport-recovery-v7";
inline constexpr std::string_view kRecoveryPolicyVersion = "coax-recovery-v5";
inline constexpr int kHlsLiveStartIndex = -1;
inline constexpr std::string_view kHlsRuntimeRetryOptions =
    "http_persistent=0,http_multiple=0,seg_max_retry=0";
inline constexpr std::size_t kDemuxerMaxBytes = 64U * 1024U * 1024U;

struct RecoveryPolicy {
    std::array<Duration, 5> attempt_delays{
        milliseconds(500), milliseconds(1'000), milliseconds(2'000),
        milliseconds(4'000), milliseconds(5'000)};
    Duration steady_healthy_window = milliseconds(5'000);
    // Commands must be admitted within this episode window. A command issued
    // inside it may still reach first frame and complete probation afterward.
    Duration command_admission_budget = milliseconds(30'000);
    std::size_t short_reopens_before_recreation = 2;
    std::string_view version = kRecoveryPolicyVersion;
};

inline constexpr RecoveryPolicy kDefaultRecoveryPolicy{};

inline constexpr BufferPhaseTargets buffer_phase_targets(BufferPhase phase) {
    // A low opening target trims the initial read burst. Once five healthy
    // seconds establish the load, ten seconds absorbs provider jitter.
    return phase == BufferPhase::Zap ? BufferPhaseTargets{1.0, 1.0}
                                     : BufferPhaseTargets{10.0, 10.0};
}

struct HealthPolicy {
    // The provider soak separated ordinary cache fills (at most six seconds)
    // from hard freezes (at least 74 seconds). Ten seconds preserves a clear
    // grace margin while bounding how long paused-for-cache can suppress
    // recovery when playback is not moving.
    Duration cache_pause_grace = milliseconds(10'000);
    double decode_shortfall_ratio = 0.05;
    Duration decode_stall_confirmation = milliseconds(6'000);
    double depleted_buffer_seconds = 0.5;
    double discontinuity_jump_seconds = 1.0;
    int min_cache_stall_observations = 3;
    int min_decode_stall_observations = 8;
    int min_open_stall_observations = 8;
    int min_stall_observations = 3;
    Duration open_stall_confirmation = milliseconds(8'000);
    double progress_epsilon_seconds = 0.05;
    Duration sample_interval = milliseconds(500);
    Duration stall_confirmation = milliseconds(1'000);
    double throttled_input_ratio = 0.9;
};

inline constexpr HealthPolicy kDefaultHealthPolicy{};

}  // namespace coax::core
