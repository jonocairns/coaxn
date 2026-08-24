#include "player/recovery_edge_telemetry.hpp"

#include <utility>

namespace coax::player {

void RecoveryEdgeObserver::begin_recovery(RecoveryEdgeAnchor anchor) {
    pause_seen_since_anchor_ = anchor.cache_paused;
    readable_samples_ = 0;
    anchor_ = std::move(anchor);
}

void RecoveryEdgeObserver::reset() {
    anchor_.reset();
    pause_seen_since_anchor_ = false;
    readable_samples_ = 0;
}

std::optional<RecoveryEdgeReport> RecoveryEdgeObserver::observe(
    const RecoveryEdgeObservation& observation) {
    if (!anchor_) return std::nullopt;

    RecoveryEdgeReport report;
    report.generation = anchor_->generation;
    report.outgoing_load_attempt = anchor_->outgoing_load_attempt;
    report.recovered_load_attempt = anchor_->recovered_load_attempt;
    report.point = observation.point;
    report.phase = observation.phase;
    report.recovery_reason = anchor_->recovery_reason;
    report.anchor_cache_paused = anchor_->cache_paused;
    report.cache_paused = observation.cache_paused;

    if (observation.observed_at < anchor_->observed_at) {
        report.data_status = RecoveryEdgeDataStatus::NonMonotonicTime;
        report.pause_seen_since_anchor = pause_seen_since_anchor_;
        return report;
    }

    report.elapsed_since_anchor = observation.observed_at - anchor_->observed_at;
    if (*report.elapsed_since_anchor > kRecoveryEdgeCaptureWindow) {
        reset();
        return std::nullopt;
    }

    const bool current_identity =
        observation.generation == anchor_->generation &&
        observation.load_attempt == anchor_->recovered_load_attempt;
    if (!current_identity) {
        report.data_status = RecoveryEdgeDataStatus::StaleIdentity;
        report.pause_seen_since_anchor = pause_seen_since_anchor_;
        return report;
    }

    pause_seen_since_anchor_ =
        pause_seen_since_anchor_ || observation.cache_paused;
    report.pause_seen_since_anchor = pause_seen_since_anchor_;

    if (!anchor_->playback_time_seconds || !anchor_->cache_end_seconds ||
        !observation.playback_time_seconds || !observation.cache_end_seconds) {
        report.data_status = RecoveryEdgeDataStatus::MissingTelemetry;
        return report;
    }

    const double elapsed = report.elapsed_since_anchor->count();
    report.playback_wall_residual_seconds =
        *observation.playback_time_seconds - *anchor_->playback_time_seconds - elapsed;
    report.cache_end_wall_residual_seconds =
        *observation.cache_end_seconds - *anchor_->cache_end_seconds - elapsed;
    const double anchor_gap =
        *anchor_->cache_end_seconds - *anchor_->playback_time_seconds;
    const double observed_gap =
        *observation.cache_end_seconds - *observation.playback_time_seconds;
    report.local_live_gap_change_seconds = observed_gap - anchor_gap;

    report.first_readable = readable_samples_ == 0;
    report.readable_sample_index = ++readable_samples_;
    return report;
}

const char* to_string(RecoveryEdgeObservationPoint value) {
    switch (value) {
        case RecoveryEdgeObservationPoint::FirstFrame: return "first-frame";
        case RecoveryEdgeObservationPoint::HealthSample: return "health-sample";
    }
    return "health-sample";
}

const char* to_string(RecoveryEdgePhase value) {
    switch (value) {
        case RecoveryEdgePhase::Opening: return "opening";
        case RecoveryEdgePhase::Probation: return "probation";
        case RecoveryEdgePhase::PostProbation: return "post-probation";
    }
    return "opening";
}

const char* to_string(RecoveryEdgeDataStatus value) {
    switch (value) {
        case RecoveryEdgeDataStatus::Complete: return "complete";
        case RecoveryEdgeDataStatus::MissingTelemetry: return "missing-telemetry";
        case RecoveryEdgeDataStatus::StaleIdentity: return "stale-identity";
        case RecoveryEdgeDataStatus::NonMonotonicTime: return "non-monotonic-time";
    }
    return "missing-telemetry";
}

const char* to_string(RecoveryEdgeProjectionBasis value) {
    switch (value) {
        case RecoveryEdgeProjectionBasis::AnchorPlusWallClock:
            return "anchor-plus-wall-clock";
    }
    return "anchor-plus-wall-clock";
}

}  // namespace coax::player
