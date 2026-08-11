#pragma once

#include "core/playback_types.hpp"

namespace coax::core {

// The native adapter translates this policy result into Windows power
// requests. Keeping the decision portable makes playback intent, rather than
// mpv's transient cache-pause state, the thing that owns sleep inhibition.
enum class PlaybackPowerMode { AllowSleep, SystemRequired, DisplayAndSystemRequired };

struct PlaybackPowerContext {
    bool session_active = false;
    bool user_paused = false;
    bool window_minimized = false;
    bool terminal_failure = false;
    bool terminal_failure_grace_active = false;
};

inline constexpr Duration kTerminalFailurePowerGrace = milliseconds(30'000);

[[nodiscard]] constexpr PlaybackPowerMode decide_playback_power_mode(
    const PlaybackPowerContext& context) {
    if (!context.session_active || context.user_paused ||
        (context.terminal_failure && !context.terminal_failure_grace_active)) {
        return PlaybackPowerMode::AllowSleep;
    }
    return context.window_minimized ? PlaybackPowerMode::SystemRequired
                                    : PlaybackPowerMode::DisplayAndSystemRequired;
}

}  // namespace coax::core
