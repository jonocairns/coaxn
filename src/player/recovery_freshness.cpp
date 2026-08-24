#include "player/recovery_freshness.hpp"

#include <cmath>
#include <utility>

namespace coax::player {
namespace {

bool same_direction(double left, double right) {
    return (left < 0.0 && right < 0.0) || (left > 0.0 && right > 0.0);
}

}  // namespace

void RecoveryFreshnessObserver::begin_recovery(RecoveryFreshnessAnchor anchor) {
    anchor_ = std::move(anchor);
    first_comparable_at_.reset();
    last_convergence_at_.reset();
    first_cache_deficit_seconds_.reset();
    previous_cache_deficit_seconds_.reset();
    last_classification_.reset();
    comparable_samples_ = 0;
}

void RecoveryFreshnessObserver::reset() {
    anchor_.reset();
    first_comparable_at_.reset();
    last_convergence_at_.reset();
    first_cache_deficit_seconds_.reset();
    previous_cache_deficit_seconds_.reset();
    last_classification_.reset();
    comparable_samples_ = 0;
}

std::optional<RecoveryFreshnessReport> RecoveryFreshnessObserver::observe(
    const RecoveryFreshnessObservation& observation) {
    if (!anchor_) return std::nullopt;

    RecoveryFreshnessReport report;
    report.generation = anchor_->generation;
    report.outgoing_load_attempt = anchor_->outgoing_load_attempt;
    report.recovered_load_attempt = anchor_->recovered_load_attempt;
    report.point = observation.point;
    report.phase = observation.phase;
    report.recovery_reason = anchor_->recovery_reason;
    report.anchor_cache_paused = anchor_->cache_paused;
    report.cache_paused = observation.cache_paused;
    report.policy_version = policy_.version;

    const bool current_identity =
        observation.generation == anchor_->generation &&
        observation.load_attempt == anchor_->recovered_load_attempt;
    if (!current_identity) {
        report.unverifiable_reason =
            RecoveryFreshnessUnverifiableReason::StaleIdentity;
        return report;
    }
    if (observation.observed_at < anchor_->observed_at) {
        report.unverifiable_reason =
            RecoveryFreshnessUnverifiableReason::ClockDomainUnclear;
        return report;
    }

    report.elapsed_since_anchor = observation.observed_at - anchor_->observed_at;
    if (!anchor_->playback_time_seconds || !anchor_->cache_end_seconds ||
        !observation.playback_time_seconds || !observation.cache_end_seconds) {
        report.unverifiable_reason =
            RecoveryFreshnessUnverifiableReason::MissingTelemetry;
        return report;
    }

    const double elapsed = report.elapsed_since_anchor->count();
    const double projected_playback = *anchor_->playback_time_seconds + elapsed;
    const double projected_cache_end = *anchor_->cache_end_seconds + elapsed;
    const double playback_deficit =
        projected_playback - *observation.playback_time_seconds;
    const double cache_deficit =
        projected_cache_end - *observation.cache_end_seconds;
    const double anchor_gap =
        *anchor_->cache_end_seconds - *anchor_->playback_time_seconds;
    const double recovered_gap =
        *observation.cache_end_seconds - *observation.playback_time_seconds;

    report.playback_deficit_seconds = playback_deficit;
    report.cache_end_deficit_seconds = cache_deficit;
    report.local_live_gap_change_seconds = recovered_gap - anchor_gap;

    if (!first_comparable_at_) {
        first_comparable_at_ = observation.observed_at;
        first_cache_deficit_seconds_ = cache_deficit;
        report.first_readable = true;
    }
    ++comparable_samples_;
    report.comparable_samples = comparable_samples_;
    report.comparable_for = observation.observed_at - *first_comparable_at_;
    report.deficit_improvement_seconds =
        *first_cache_deficit_seconds_ - cache_deficit;
    const std::optional<double> latest_improvement = previous_cache_deficit_seconds_
        ? std::optional<double>{*previous_cache_deficit_seconds_ - cache_deficit}
        : std::nullopt;
    previous_cache_deficit_seconds_ = cache_deficit;

    const bool coherent_clocks =
        std::abs(playback_deficit - cache_deficit) <=
        policy_.clock_coherence_tolerance_seconds;
    const bool large_coherent_shift = coherent_clocks &&
        same_direction(playback_deficit, cache_deficit) &&
        std::abs(playback_deficit) >= policy_.clock_rebase_threshold_seconds &&
        std::abs(cache_deficit) >= policy_.clock_rebase_threshold_seconds;

    if (large_coherent_shift) {
        report.classification = RecoveryFreshnessClassification::ClockRebased;
    } else if (!coherent_clocks) {
        report.classification = RecoveryFreshnessClassification::Unverifiable;
        report.unverifiable_reason =
            RecoveryFreshnessUnverifiableReason::ClockDomainUnclear;
    } else if (std::abs(cache_deficit) <= policy_.fresh_tolerance_seconds) {
        report.classification = RecoveryFreshnessClassification::Fresh;
    } else if (cache_deficit < 0.0) {
        report.classification = RecoveryFreshnessClassification::Unverifiable;
        report.unverifiable_reason =
            RecoveryFreshnessUnverifiableReason::ClockDomainUnclear;
    } else if (comparable_samples_ < 2) {
        report.classification = RecoveryFreshnessClassification::Unverifiable;
        report.unverifiable_reason =
            RecoveryFreshnessUnverifiableReason::InsufficientHistory;
    } else if (latest_improvement && *latest_improvement >=
               policy_.convergence_epsilon_seconds) {
        report.classification = RecoveryFreshnessClassification::Converging;
        last_convergence_at_ = observation.observed_at;
    } else if (*report.comparable_for >= policy_.stale_confirmation &&
               (!last_convergence_at_ ||
                observation.observed_at - *last_convergence_at_ >=
                    policy_.stale_confirmation)) {
        report.classification = RecoveryFreshnessClassification::Stale;
    } else {
        report.classification = RecoveryFreshnessClassification::Unverifiable;
        report.unverifiable_reason =
            RecoveryFreshnessUnverifiableReason::InsufficientHistory;
    }

    report.classification_changed =
        !last_classification_ || *last_classification_ != report.classification;
    last_classification_ = report.classification;
    return report;
}

const char* to_string(RecoveryFreshnessClassification value) {
    switch (value) {
        case RecoveryFreshnessClassification::Unverifiable: return "unverifiable";
        case RecoveryFreshnessClassification::Fresh: return "fresh";
        case RecoveryFreshnessClassification::Converging: return "converging";
        case RecoveryFreshnessClassification::Stale: return "stale";
        case RecoveryFreshnessClassification::ClockRebased: return "clock-rebased";
    }
    return "unverifiable";
}

const char* to_string(RecoveryFreshnessObservationPoint value) {
    switch (value) {
        case RecoveryFreshnessObservationPoint::FirstFrame: return "first-frame";
        case RecoveryFreshnessObservationPoint::HealthSample: return "health-sample";
    }
    return "health-sample";
}

const char* to_string(RecoveryFreshnessPhase value) {
    switch (value) {
        case RecoveryFreshnessPhase::Opening: return "opening";
        case RecoveryFreshnessPhase::Probation: return "probation";
        case RecoveryFreshnessPhase::PostProbation: return "post-probation";
    }
    return "opening";
}

const char* to_string(RecoveryFreshnessUnverifiableReason value) {
    switch (value) {
        case RecoveryFreshnessUnverifiableReason::None: return "none";
        case RecoveryFreshnessUnverifiableReason::MissingTelemetry:
            return "missing-telemetry";
        case RecoveryFreshnessUnverifiableReason::StaleIdentity:
            return "stale-identity";
        case RecoveryFreshnessUnverifiableReason::InsufficientHistory:
            return "insufficient-history";
        case RecoveryFreshnessUnverifiableReason::ClockDomainUnclear:
            return "clock-domain-unclear";
    }
    return "none";
}

}  // namespace coax::player
