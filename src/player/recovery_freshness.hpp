#pragma once

#include <cstddef>
#include <optional>
#include <string_view>

#include "core/playback_types.hpp"
#include "core/supervisor.hpp"

namespace coax::player {

enum class RecoveryFreshnessClassification {
    Unverifiable,
    Fresh,
    Converging,
    Stale,
    ClockRebased,
};

enum class RecoveryFreshnessObservationPoint { FirstFrame, HealthSample };
enum class RecoveryFreshnessPhase { Opening, Probation, PostProbation };

enum class RecoveryFreshnessUnverifiableReason {
    None,
    MissingTelemetry,
    StaleIdentity,
    InsufficientHistory,
    ClockDomainUnclear,
};

// These are observability thresholds, not recovery policy. The first provider
// soak is expected to replace them with evidence-backed values before a
// freshness result can influence probation or command admission.
struct RecoveryFreshnessPolicy {
    double fresh_tolerance_seconds = 2.0;
    double convergence_epsilon_seconds = 0.25;
    double clock_rebase_threshold_seconds = 60.0;
    double clock_coherence_tolerance_seconds = 5.0;
    core::Duration stale_confirmation = core::seconds(5.0);
    std::string_view version = "recovery-freshness-observability-v1";
};

inline constexpr RecoveryFreshnessPolicy kDefaultRecoveryFreshnessPolicy{};

struct RecoveryFreshnessAnchor {
    core::Generation generation;
    core::LoadAttempt outgoing_load_attempt;
    core::LoadAttempt recovered_load_attempt;
    core::TimePoint observed_at{};
    std::optional<double> playback_time_seconds;
    std::optional<double> cache_end_seconds;
    bool cache_paused = false;
    std::optional<core::DetectionReason> recovery_reason;
};

struct RecoveryFreshnessObservation {
    core::Generation generation;
    core::LoadAttempt load_attempt;
    core::TimePoint observed_at{};
    std::optional<double> playback_time_seconds;
    std::optional<double> cache_end_seconds;
    bool cache_paused = false;
    RecoveryFreshnessObservationPoint point =
        RecoveryFreshnessObservationPoint::HealthSample;
    RecoveryFreshnessPhase phase = RecoveryFreshnessPhase::Opening;
};

// Only numeric deltas and closed enums leave the classifier. Absolute media
// timestamps are intentionally retained inside RecoveryFreshnessObserver so a
// telemetry consumer cannot accidentally persist absolute media coordinates
// or raw player state beyond the active comparison.
struct RecoveryFreshnessReport {
    core::Generation generation;
    core::LoadAttempt outgoing_load_attempt;
    core::LoadAttempt recovered_load_attempt;
    RecoveryFreshnessObservationPoint point =
        RecoveryFreshnessObservationPoint::HealthSample;
    RecoveryFreshnessPhase phase = RecoveryFreshnessPhase::Opening;
    RecoveryFreshnessClassification classification =
        RecoveryFreshnessClassification::Unverifiable;
    RecoveryFreshnessUnverifiableReason unverifiable_reason =
        RecoveryFreshnessUnverifiableReason::None;
    std::optional<core::DetectionReason> recovery_reason;
    std::optional<core::Duration> elapsed_since_anchor;
    std::optional<core::Duration> comparable_for;
    std::optional<double> playback_deficit_seconds;
    std::optional<double> cache_end_deficit_seconds;
    std::optional<double> local_live_gap_change_seconds;
    std::optional<double> deficit_improvement_seconds;
    bool anchor_cache_paused = false;
    bool cache_paused = false;
    bool first_readable = false;
    bool classification_changed = false;
    std::size_t comparable_samples = 0;
    std::string_view policy_version;
};

class RecoveryFreshnessObserver {
public:
    explicit RecoveryFreshnessObserver(
        RecoveryFreshnessPolicy policy = kDefaultRecoveryFreshnessPolicy)
        : policy_(policy) {}

    void begin_recovery(RecoveryFreshnessAnchor anchor);
    void reset();

    [[nodiscard]] bool active() const { return anchor_.has_value(); }
    [[nodiscard]] std::optional<RecoveryFreshnessReport> observe(
        const RecoveryFreshnessObservation& observation);

private:
    RecoveryFreshnessPolicy policy_;
    std::optional<RecoveryFreshnessAnchor> anchor_;
    std::optional<core::TimePoint> first_comparable_at_;
    std::optional<core::TimePoint> last_convergence_at_;
    std::optional<double> first_cache_deficit_seconds_;
    std::optional<double> previous_cache_deficit_seconds_;
    std::optional<RecoveryFreshnessClassification> last_classification_;
    std::size_t comparable_samples_ = 0;
};

const char* to_string(RecoveryFreshnessClassification value);
const char* to_string(RecoveryFreshnessObservationPoint value);
const char* to_string(RecoveryFreshnessPhase value);
const char* to_string(RecoveryFreshnessUnverifiableReason value);

}  // namespace coax::player
