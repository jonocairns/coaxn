#include "player/live_sync.hpp"

#include <algorithm>
#include <cmath>

namespace coax::player {

LiveSync::LiveSync(LiveSyncConfig config)
    : config_(config), target_offset_seconds_(config.initial_target_offset_seconds) {}

void LiveSync::reset() {
    target_offset_seconds_ = config_.initial_target_offset_seconds;
    speed_                 = 1.0;
    has_updated_           = false;
    last_update_at_        = 0.0;
}

void LiveSync::notify_rebuffer() {
    target_offset_seconds_ = std::min(target_offset_seconds_ + config_.rebuffer_increment_seconds,
                                      config_.max_target_offset_seconds);

    // Force the next update() to recompute immediately rather than waiting out
    // the interval: the target just moved, so the current speed is stale.
    has_updated_ = false;
}

std::optional<double> LiveSync::hold_unity_speed() {
    // A hold marks a break in valid normal-playback telemetry. Do this even if
    // unity is already installed: when telemetry returns, its first sample is
    // more relevant than the rate-limit deadline from before the break.
    has_updated_ = false;
    if (speed_ == 1.0) return std::nullopt;
    speed_ = 1.0;
    return speed_;
}

std::optional<double> LiveSync::update(double buffered_seconds, double now_seconds) {
    if (has_updated_ && now_seconds - last_update_at_ < config_.min_update_interval_seconds) {
        return std::nullopt;
    }
    last_update_at_ = now_seconds;
    has_updated_    = true;

    const double target = std::clamp(target_offset_seconds_,
                                     config_.min_target_offset_seconds,
                                     config_.max_target_offset_seconds);

    // Buffered duration stands in for how far behind the live edge we are:
    // more buffered means further behind, so a positive error means speed up.
    const double error = buffered_seconds - target;

    double speed = 1.0;
    if (std::abs(error) > config_.deadband_seconds) {
        speed = std::clamp(1.0 + config_.proportional_control_factor * error,
                           config_.min_speed, config_.max_speed);
    }

    // Avoid churning mpv's speed property with changes it cannot act on.
    if (std::abs(speed - speed_) < 0.001) {
        return std::nullopt;
    }

    speed_ = speed;
    return speed_;
}

}  // namespace coax::player
