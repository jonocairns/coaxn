#pragma once

#include <cstddef>
#include <deque>
#include <optional>

#include "core/playback_health.hpp"

namespace coax::player {

struct TimelineRecoveryPolicy {
    double material_rewind_seconds = 5.0;
    double material_live_gap_seconds = 5.0;
    double common_clock_tolerance_seconds = 1.0;
    core::Duration confirmation_window = core::milliseconds(1500);
    core::Duration baseline_max_age = core::seconds(15);
    core::Duration cooldown = core::seconds(30);
    core::Duration rate_window = core::seconds(300);
    std::size_t max_recoveries_per_window = 2;
};

enum class TimelineRecoveryOutcome {
    None,
    CandidateArmed,
    CandidateAwaitingTelemetry,
    CandidatePersistencePending,
    CandidateCleared,
    CommonClockReset,
    Recover,
    SuppressedCooldown,
    SuppressedRateLimit,
};

struct TimelineRecoveryObservation {
    core::Generation generation;
    core::LoadAttempt load_attempt;
    core::TimePoint observed_at{};
    core::TimelineEvidence timeline;
    std::optional<double> cache_end_seconds;
    std::optional<double> playback_time_seconds;
    std::optional<double> rebuffer_age_seconds;
    bool healthy = false;
};

struct TimelineRecoveryStep {
    std::optional<double> baseline_live_gap_seconds;
    std::optional<double> current_live_gap_seconds;
    std::optional<double> cache_relative_loss_seconds;
    std::optional<double> live_gap_increase_seconds;
    std::optional<double> rebuffer_age_seconds;
    std::optional<bool> supervisor_accepted;
    bool cache_resume_related = false;
    TimelineRecoveryOutcome outcome = TimelineRecoveryOutcome::None;
    bool recover = false;
};

// Distinguishes a provider clock rebase from playback actually falling behind
// the incoming MPEG-TS head. Load changes clear evidence but retain the
// generation-scoped circuit breaker; a new channel clears both.
class TimelineRecovery {
public:
    explicit TimelineRecovery(TimelineRecoveryPolicy policy = {});

    void reset();
    void begin_load(core::Generation generation, core::LoadAttempt load_attempt);
    void note_recovered_first_frame(core::TimePoint now);
    [[nodiscard]] TimelineRecoveryStep observe(
        const TimelineRecoveryObservation& observation, bool eligible);

private:
    struct Candidate {
        core::Generation generation;
        core::LoadAttempt load_attempt;
        core::TimePoint armed_at;
        std::optional<double> baseline_live_gap_seconds;
        std::optional<double> cache_relative_loss_seconds;
        bool requires_gap_persistence = false;
        bool gap_persistence_observed = false;
    };

    [[nodiscard]] TimelineRecoveryStep decide_recovery(
        core::TimePoint now, TimelineRecoveryStep step);
    void prune(core::TimePoint now);
    void remember_trusted_live_gap(const TimelineRecoveryObservation& observation);

    TimelineRecoveryPolicy policy_;
    core::Generation generation_;
    core::LoadAttempt load_attempt_;
    std::optional<Candidate> candidate_;
    std::optional<double> trusted_live_gap_seconds_;
    std::optional<core::TimePoint> trusted_live_gap_at_;
    std::deque<core::TimePoint> recovery_decisions_;
    std::optional<core::TimePoint> cooldown_until_;
};

const char* to_string(TimelineRecoveryOutcome outcome);

}  // namespace coax::player
