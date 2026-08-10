#pragma once

#include <windows.h>

#include <chrono>

#include "core/playback_power.hpp"

namespace coax::win {

// Owns the two handle-based Windows power requests independently. The class is
// idempotent because the application evaluates its portable policy every turn
// while a terminal-failure grace may be expiring.
class PlaybackPowerRequest {
public:
    PlaybackPowerRequest() = default;
    ~PlaybackPowerRequest();

    PlaybackPowerRequest(const PlaybackPowerRequest&) = delete;
    PlaybackPowerRequest& operator=(const PlaybackPowerRequest&) = delete;

    void set_mode(core::PlaybackPowerMode mode);

private:
    bool ensure_handle();
    bool set_request(POWER_REQUEST_TYPE type, bool enabled, bool& active);
    void warn_failure(const char* operation, DWORD error);
    void release();

    HANDLE handle_ = INVALID_HANDLE_VALUE;
    core::PlaybackPowerMode requested_mode_ = core::PlaybackPowerMode::AllowSleep;
    bool system_active_ = false;
    bool display_active_ = false;
    bool settled_ = true;
    bool warned_ = false;
    std::chrono::steady_clock::time_point retry_after_{};
};

}  // namespace coax::win
