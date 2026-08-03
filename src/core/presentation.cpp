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

void PresentationRebuildBudget::failed(TimePoint now) {
    // Still outstanding, re-timed from when the attempt actually finished
    // rather than from when it started: creating a device against a resetting
    // adapter can itself take a while to fail.
    due_at_ = now + policy_.retry_delay;
}

}  // namespace coax::core
