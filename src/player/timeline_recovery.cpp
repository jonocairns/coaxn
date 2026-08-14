#include "player/timeline_recovery.hpp"

#include <algorithm>
#include <cmath>

namespace coax::player {
namespace {

std::optional<double> live_gap(const TimelineRecoveryObservation& observation) {
    if (!observation.cache_end_seconds || !observation.playback_time_seconds) {
        return std::nullopt;
    }
    return *observation.cache_end_seconds - *observation.playback_time_seconds;
}

bool cache_resume_related(const TimelineRecoveryObservation& observation) {
    return observation.timeline.cache_paused ||
           observation.timeline.previous_cache_paused.value_or(false);
}

}  // namespace

TimelineRecovery::TimelineRecovery(TimelineRecoveryPolicy policy)
    : policy_(policy) {}

void TimelineRecovery::reset() {
    generation_ = {};
    load_attempt_ = {};
    candidate_.reset();
    trusted_live_gap_seconds_.reset();
    trusted_live_gap_at_.reset();
    recovery_decisions_.clear();
    cooldown_until_.reset();
}

void TimelineRecovery::begin_load(core::Generation generation,
                                  core::LoadAttempt load_attempt) {
    generation_ = generation;
    load_attempt_ = load_attempt;
    candidate_.reset();
    trusted_live_gap_seconds_.reset();
    trusted_live_gap_at_.reset();
}

void TimelineRecovery::note_recovered_first_frame(core::TimePoint now) {
    cooldown_until_ = now + policy_.cooldown;
}

void TimelineRecovery::prune(core::TimePoint now) {
    while (!recovery_decisions_.empty() &&
           now - recovery_decisions_.front() > policy_.rate_window) {
        recovery_decisions_.pop_front();
    }
}

void TimelineRecovery::remember_trusted_live_gap(
    const TimelineRecoveryObservation& observation) {
    if (observation.healthy) {
        if (const auto gap = live_gap(observation)) {
            trusted_live_gap_seconds_ = gap;
            trusted_live_gap_at_ = observation.observed_at;
        }
    }
}

TimelineRecoveryStep TimelineRecovery::decide_recovery(
    core::TimePoint now, TimelineRecoveryStep step) {
    candidate_.reset();
    prune(now);
    if (cooldown_until_ && now < *cooldown_until_) {
        step.outcome = TimelineRecoveryOutcome::SuppressedCooldown;
        return step;
    }
    if (recovery_decisions_.size() >= policy_.max_recoveries_per_window) {
        step.outcome = TimelineRecoveryOutcome::SuppressedRateLimit;
        return step;
    }
    recovery_decisions_.push_back(now);
    step.outcome = TimelineRecoveryOutcome::Recover;
    step.recover = true;
    return step;
}

TimelineRecoveryStep TimelineRecovery::observe(
    const TimelineRecoveryObservation& observation, bool eligible) {
    TimelineRecoveryStep step;
    prune(observation.observed_at);
    if (observation.generation != generation_ ||
        observation.load_attempt != load_attempt_) {
        return step;
    }
    if (!eligible) {
        candidate_.reset();
        trusted_live_gap_seconds_.reset();
        trusted_live_gap_at_.reset();
        return step;
    }

    const auto movement = observation.timeline.playback_movement_seconds;
    const auto deviation = observation.timeline.playback_deviation_seconds;
    const auto cache_movement = observation.timeline.cache_end_movement_seconds;
    const bool material_rewind = deviation &&
        *deviation <= -policy_.material_rewind_seconds;
    const bool common_clock_reset = material_rewind && movement && cache_movement &&
        std::abs(*cache_movement - *movement) <=
            policy_.common_clock_tolerance_seconds;
    std::optional<double> cache_relative_loss;
    if (movement && cache_movement) {
        cache_relative_loss = *cache_movement - *movement;
    }
    const auto current_gap = live_gap(observation);
    const bool refill_related = cache_resume_related(observation);

    if (candidate_) {
        auto candidate = *candidate_;
        candidate_.reset();
        if (observation.observed_at - candidate.armed_at <=
            policy_.confirmation_window) {
            candidate.requires_gap_persistence =
                candidate.requires_gap_persistence || refill_related;
            step.baseline_live_gap_seconds = candidate.baseline_live_gap_seconds;
            step.current_live_gap_seconds = current_gap;
            step.cache_relative_loss_seconds = cache_relative_loss
                ? cache_relative_loss : candidate.cache_relative_loss_seconds;
            step.rebuffer_age_seconds = observation.rebuffer_age_seconds;
            step.cache_resume_related = candidate.requires_gap_persistence;
            if (common_clock_reset) {
                step.outcome = TimelineRecoveryOutcome::CommonClockReset;
                remember_trusted_live_gap(observation);
                return step;
            }

            std::optional<double> gap_increase;
            if (current_gap && candidate.baseline_live_gap_seconds) {
                gap_increase = *current_gap - *candidate.baseline_live_gap_seconds;
            }
            const bool confirmed_gap = gap_increase &&
                *gap_increase >= policy_.material_live_gap_seconds;
            step.live_gap_increase_seconds = gap_increase;
            if (material_rewind) {
                return decide_recovery(observation.observed_at, step);
            }
            if (confirmed_gap) {
                if (candidate.requires_gap_persistence &&
                    !candidate.gap_persistence_observed) {
                    candidate.gap_persistence_observed = true;
                    candidate_ = candidate;
                    step.outcome =
                        TimelineRecoveryOutcome::CandidatePersistencePending;
                    return step;
                }
                return decide_recovery(observation.observed_at, step);
            }

            if (!current_gap || !candidate.baseline_live_gap_seconds) {
                candidate_ = candidate;
                step.outcome =
                    TimelineRecoveryOutcome::CandidateAwaitingTelemetry;
                return step;
            }

            step.outcome = TimelineRecoveryOutcome::CandidateCleared;
            remember_trusted_live_gap(observation);
            return step;
        }

        step.outcome = TimelineRecoveryOutcome::CandidateCleared;
    }

    if (material_rewind) {
        std::optional<double> baseline;
        if (trusted_live_gap_seconds_ && trusted_live_gap_at_ &&
            observation.observed_at - *trusted_live_gap_at_ <=
                policy_.baseline_max_age) {
            baseline = trusted_live_gap_seconds_;
        }
        candidate_ = Candidate{
            observation.generation,
            observation.load_attempt,
            observation.observed_at,
            baseline,
            cache_relative_loss,
            refill_related,
            false,
        };
        step.baseline_live_gap_seconds = baseline;
        step.current_live_gap_seconds = current_gap;
        step.cache_relative_loss_seconds = cache_relative_loss;
        step.rebuffer_age_seconds = observation.rebuffer_age_seconds;
        step.cache_resume_related = refill_related;
        step.outcome = common_clock_reset
            ? TimelineRecoveryOutcome::CommonClockReset
            : TimelineRecoveryOutcome::CandidateArmed;
        return step;
    }

    if (step.outcome == TimelineRecoveryOutcome::CandidateCleared) {
        remember_trusted_live_gap(observation);
        return step;
    }

    remember_trusted_live_gap(observation);
    return step;
}

const char* to_string(TimelineRecoveryOutcome outcome) {
    switch (outcome) {
        case TimelineRecoveryOutcome::None: return "none";
        case TimelineRecoveryOutcome::CandidateArmed: return "candidate-armed";
        case TimelineRecoveryOutcome::CandidateAwaitingTelemetry:
            return "candidate-awaiting-telemetry";
        case TimelineRecoveryOutcome::CandidatePersistencePending:
            return "candidate-persistence-pending";
        case TimelineRecoveryOutcome::CandidateCleared: return "candidate-cleared";
        case TimelineRecoveryOutcome::CommonClockReset: return "common-clock-reset";
        case TimelineRecoveryOutcome::Recover: return "recover";
        case TimelineRecoveryOutcome::SuppressedCooldown: return "suppressed-cooldown";
        case TimelineRecoveryOutcome::SuppressedRateLimit: return "suppressed-rate-limit";
    }
    return "none";
}

}  // namespace coax::player
