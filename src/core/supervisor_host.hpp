#pragma once

#include <deque>
#include <functional>
#include <optional>

#include "core/supervisor.hpp"

namespace coax::core {

class SupervisorClock {
public:
    virtual ~SupervisorClock() = default;
    [[nodiscard]] virtual TimePoint now() const = 0;
};

class SteadySupervisorClock final : public SupervisorClock {
public:
    [[nodiscard]] TimePoint now() const override;
};

struct SupervisorHostCallbacks {
    std::function<void(const SupervisorEffect&)> on_effect;
    std::function<void(const SupervisorState&)> on_state_changed;
    std::function<void(const SupervisorTransition&)> on_transition;
};

// Owns one logical deadline, always re-derived from the reducer state. The UI
// host calls poll() from its event loop; tests supply a fake monotonic clock.
class PlaybackSupervisor {
public:
    PlaybackSupervisor(const SupervisorClock& clock, SupervisorHostCallbacks callbacks,
                       RecoveryPolicy policy = kDefaultRecoveryPolicy);

    void dispatch(const SupervisorEvent& event);
    void poll();
    void dispose();

    [[nodiscard]] const SupervisorState& current() const { return state_; }
    [[nodiscard]] std::optional<TimePoint> armed_deadline() const { return armed_deadline_; }

private:
    void apply(const SupervisorEvent& event);
    void rearm();

    const SupervisorClock& clock_;
    SupervisorHostCallbacks callbacks_;
    RecoveryPolicy policy_;
    bool disposed_ = false;
    bool dispatching_ = false;
    SupervisorState state_ = initial_supervisor_state();
    std::optional<TimePoint> armed_deadline_;
    std::deque<SupervisorEvent> pending_events_;
    std::deque<SupervisorEffect> pending_effects_;
};

}  // namespace coax::core
