#include "core/supervisor_host.hpp"

namespace coax::core {

TimePoint SteadySupervisorClock::now() const {
    return std::chrono::time_point_cast<Duration>(std::chrono::steady_clock::now());
}

PlaybackSupervisor::PlaybackSupervisor(const SupervisorClock& clock,
                                       SupervisorHostCallbacks callbacks,
                                       RecoveryPolicy policy)
    : clock_(clock), callbacks_(std::move(callbacks)), policy_(std::move(policy)) {}

void PlaybackSupervisor::dispatch(const SupervisorEvent& event) {
    if (disposed_) return;
    pending_events_.push_back(event);
    if (dispatching_) return;

    dispatching_ = true;
    try {
        while (!disposed_ && (!pending_events_.empty() || !pending_effects_.empty())) {
            if (!pending_events_.empty()) {
                auto next = std::move(pending_events_.front());
                pending_events_.pop_front();
                apply(next);
                continue;
            }
            auto effect = std::move(pending_effects_.front());
            pending_effects_.pop_front();
            if (callbacks_.on_effect) callbacks_.on_effect(effect);
        }
    } catch (...) {
        dispatching_ = false;
        throw;
    }
    dispatching_ = false;
}

void PlaybackSupervisor::poll() {
    if (disposed_ || !armed_deadline_ || clock_.now() < *armed_deadline_) return;
    dispatch(DeadlineReached{});
}

void PlaybackSupervisor::dispose() {
    disposed_ = true;
    armed_deadline_.reset();
    pending_events_.clear();
    pending_effects_.clear();
}

void PlaybackSupervisor::apply(const SupervisorEvent& event) {
    auto reduction = reduce_supervisor_state(state_, event, clock_.now(), policy_);
    state_ = std::move(reduction.state);
    // Re-arm before callbacks. Effects are queued and drained only by the
    // outermost dispatch frame, so a synchronous settlement cannot re-enter.
    rearm();
    for (auto& effect : reduction.effects) pending_effects_.push_back(std::move(effect));
    if (reduction.transition && callbacks_.on_transition) {
        callbacks_.on_transition(*reduction.transition);
    }
    if (reduction.transition && callbacks_.on_state_changed) {
        callbacks_.on_state_changed(state_);
    }
}

void PlaybackSupervisor::rearm() { armed_deadline_ = next_deadline_at(state_); }

}  // namespace coax::core
