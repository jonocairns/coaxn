#pragma once

#include <cstddef>
#include <optional>
#include <string_view>

#include "core/playback_types.hpp"
#include "core/supervisor.hpp"

namespace coax::player {

enum class RecoveryEdgeObservationPoint { FirstFrame, HealthSample };
enum class RecoveryEdgePhase { Opening, Probation, PostProbation };

// This status describes whether a report contains a trustworthy calculation.
// It deliberately says nothing about whether the recovered media is fresh.
enum class RecoveryEdgeDataStatus {
    Complete,
    MissingTelemetry,
    StaleIdentity,
    NonMonotonicTime,
};

// Raw MPEG-TS exposes no authoritative provider live edge. This basis records
// the proxy used by the telemetry without claiming that the provider's media
// clock necessarily advanced with wall time.
enum class RecoveryEdgeProjectionBasis { AnchorPlusWallClock };

inline constexpr core::Duration kRecoveryEdgeCaptureWindow = core::seconds(30.0);
inline constexpr std::string_view kRecoveryEdgeTelemetrySchema =
    "recovery-edge-observability-v1";

struct RecoveryEdgeAnchor {
    core::Generation generation;
    core::LoadAttempt outgoing_load_attempt;
    core::LoadAttempt recovered_load_attempt;
    core::TimePoint observed_at{};
    std::optional<double> playback_time_seconds;
    std::optional<double> cache_end_seconds;
    bool cache_paused = false;
    std::optional<core::DetectionReason> recovery_reason;
};

struct RecoveryEdgeObservation {
    core::Generation generation;
    core::LoadAttempt load_attempt;
    core::TimePoint observed_at{};
    std::optional<double> playback_time_seconds;
    std::optional<double> cache_end_seconds;
    bool cache_paused = false;
    RecoveryEdgeObservationPoint point = RecoveryEdgeObservationPoint::HealthSample;
    RecoveryEdgePhase phase = RecoveryEdgePhase::Opening;
};

// Only identities, flags, closed enums and numeric deltas leave the observer.
// Absolute media timestamps remain inside RecoveryEdgeObserver so telemetry
// cannot retain provider coordinates or accidentally grow into recovery policy.
struct RecoveryEdgeReport {
    core::Generation generation;
    core::LoadAttempt outgoing_load_attempt;
    core::LoadAttempt recovered_load_attempt;
    RecoveryEdgeObservationPoint point = RecoveryEdgeObservationPoint::HealthSample;
    RecoveryEdgePhase phase = RecoveryEdgePhase::Opening;
    RecoveryEdgeDataStatus data_status = RecoveryEdgeDataStatus::Complete;
    RecoveryEdgeProjectionBasis projection_basis =
        RecoveryEdgeProjectionBasis::AnchorPlusWallClock;
    std::optional<core::DetectionReason> recovery_reason;
    std::optional<core::Duration> elapsed_since_anchor;
    std::optional<double> playback_wall_residual_seconds;
    std::optional<double> cache_end_wall_residual_seconds;
    std::optional<double> local_live_gap_change_seconds;
    bool anchor_cache_paused = false;
    bool cache_paused = false;
    bool pause_seen_since_anchor = false;
    bool first_readable = false;
    std::size_t readable_sample_index = 0;
    core::Duration capture_window = kRecoveryEdgeCaptureWindow;
    std::string_view schema_version = kRecoveryEdgeTelemetrySchema;
};

class RecoveryEdgeObserver {
public:
    void begin_recovery(RecoveryEdgeAnchor anchor);
    void reset();

    [[nodiscard]] bool active() const { return anchor_.has_value(); }
    [[nodiscard]] std::optional<RecoveryEdgeReport> observe(
        const RecoveryEdgeObservation& observation);

private:
    std::optional<RecoveryEdgeAnchor> anchor_;
    bool pause_seen_since_anchor_ = false;
    std::size_t readable_samples_ = 0;
};

const char* to_string(RecoveryEdgeObservationPoint value);
const char* to_string(RecoveryEdgePhase value);
const char* to_string(RecoveryEdgeDataStatus value);
const char* to_string(RecoveryEdgeProjectionBasis value);

}  // namespace coax::player
