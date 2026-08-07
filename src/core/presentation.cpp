#include "core/presentation.hpp"

namespace coax::core {

const char* to_string(DeviceLossKind value) {
    switch (value) {
        case DeviceLossKind::Removed: return "device-removed";
        case DeviceLossKind::Reset: return "device-reset";
    }
    return "device-removed";
}

bool DeviceLossLatch::raise(DeviceLossKind kind) {
    if (kind_) return false;
    kind_ = kind;
    return true;
}

void PresentationRebuildBudget::request(TimePoint now) {
    // Once the budget has been given up on, a further loss does not restart it.
    // Failure is surfaced honestly rather than retried under a new name.
    if (exhausted_ || outstanding_) return;
    outstanding_ = true;
    due_at_ = now;
}

RebuildDecision PresentationRebuildBudget::poll(TimePoint now) {
    if (!outstanding_) return RebuildDecision::Hold;
    if (attempts_ >= policy_.max_attempts) {
        outstanding_ = false;
        exhausted_ = true;
        return RebuildDecision::Exhausted;
    }
    if (now < due_at_) return RebuildDecision::Hold;
    ++attempts_;
    // Spent here rather than on the caller settling it. A rebuild that neither
    // succeeds nor reports failure — an exception on the way out, a path that
    // forgets — must not be able to hand out the next attempt on the next
    // frame; the pacing has to hold whatever the caller does.
    due_at_ = now + policy_.retry_delay;
    return RebuildDecision::Attempt;
}

void PresentationRebuildBudget::succeeded() {
    outstanding_ = false;
    attempts_ = 0;
}

std::optional<TimePoint> PresentationRebuildBudget::next_decision_at(TimePoint now) const {
    // Nothing outstanding is nothing to wake for, and that covers exhaustion:
    // poll() clears the flag on its way to reporting it, and request() will not
    // set it again.
    if (!outstanding_) return std::nullopt;
    // The ceiling is already reached, so the next poll reports exhaustion
    // whatever the clock says. Reporting due_at_ here would hold the loop
    // asleep for a retry delay it is no longer entitled to spend.
    if (attempts_ >= policy_.max_attempts) return now;
    return due_at_;
}

void PresentationRebuildBudget::failed(TimePoint now) {
    // Still outstanding, re-timed from when the attempt actually finished
    // rather than from when it started: creating a device against a resetting
    // adapter can itself take a while to fail.
    due_at_ = now + policy_.retry_delay;
}

const char* to_string(PresentationPhase value) {
    switch (value) {
        case PresentationPhase::Ready: return "ready";
        case PresentationPhase::Rebuilding: return "rebuilding";
        case PresentationPhase::Failed: return "failed";
    }
    return "ready";
}

std::optional<Duration> decide_frame_wait(PresentationPhase phase, TimePoint now,
                                          std::optional<TimePoint> presentation_deadline,
                                          std::optional<TimePoint> supervisor_deadline,
                                          Duration ceiling) {
    // The loop is about to present, and the vsync wait inside that present is
    // its throttle. Sleeping as well would drop frames.
    if (phase == PresentationPhase::Ready) return std::nullopt;

    if (ceiling < Duration::zero()) ceiling = Duration::zero();
    TimePoint wake_at = now + ceiling;
    for (const std::optional<TimePoint>& deadline : {presentation_deadline, supervisor_deadline}) {
        if (deadline && *deadline < wake_at) wake_at = *deadline;
    }
    // A deadline already in the past is not a negative sleep; it is no sleep.
    return wake_at > now ? wake_at - now : Duration::zero();
}

}  // namespace coax::core
