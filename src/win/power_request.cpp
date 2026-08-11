#include "win/power_request.hpp"

#include "util/log.hpp"

namespace coax::win {
namespace {

constexpr auto kRetryInterval = std::chrono::seconds(1);

const char* mode_name(core::PlaybackPowerMode mode) {
    switch (mode) {
        case core::PlaybackPowerMode::AllowSleep: return "allow-sleep";
        case core::PlaybackPowerMode::SystemRequired: return "system-required";
        case core::PlaybackPowerMode::DisplayAndSystemRequired:
            return "display-and-system-required";
    }
    return "unknown";
}

}  // namespace

PlaybackPowerRequest::~PlaybackPowerRequest() { release(); }

bool PlaybackPowerRequest::ensure_handle() {
    if (handle_ != INVALID_HANDLE_VALUE) return true;

    REASON_CONTEXT reason{};
    reason.Version = POWER_REQUEST_CONTEXT_VERSION;
    reason.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
    reason.Reason.SimpleReasonString =
        const_cast<PWSTR>(L"Coax is maintaining an active live playback session");
    handle_ = PowerCreateRequest(&reason);
    if (handle_ == INVALID_HANDLE_VALUE) {
        warn_failure("creation", GetLastError());
        return false;
    }
    return true;
}

bool PlaybackPowerRequest::set_request(POWER_REQUEST_TYPE type, bool enabled,
                                       bool& active) {
    if (enabled == active) return true;
    const BOOL changed = enabled ? PowerSetRequest(handle_, type)
                                 : PowerClearRequest(handle_, type);
    if (!changed) {
        warn_failure("update", GetLastError());
        return false;
    }
    active = enabled;
    return true;
}

void PlaybackPowerRequest::warn_failure(const char* operation, DWORD error) {
    if (warned_) return;
    warned_ = true;
    log::warn("Playback power request {} failed ({}); retrying", operation, error);
}

void PlaybackPowerRequest::set_mode(core::PlaybackPowerMode mode) {
    const auto now = std::chrono::steady_clock::now();
    const bool mode_changed = mode != requested_mode_;
    if (!mode_changed && settled_) return;
    if (!mode_changed && now < retry_after_) return;

    const bool retrying = !settled_;
    requested_mode_ = mode;

    const bool want_system = mode != core::PlaybackPowerMode::AllowSleep;
    const bool want_display = mode == core::PlaybackPowerMode::DisplayAndSystemRequired;
    bool ok = true;
    if ((want_system || want_display) && !ensure_handle()) {
        ok = false;
    } else {
        // Clear the narrower display requirement before the system requirement,
        // and establish the system requirement before adding the display one.
        if (!want_display) ok = set_request(PowerRequestDisplayRequired, false,
                                            display_active_) && ok;
        if (!want_system) ok = set_request(PowerRequestSystemRequired, false,
                                           system_active_) && ok;
        if (want_system) ok = set_request(PowerRequestSystemRequired, true,
                                          system_active_) && ok;
        if (want_display) ok = set_request(PowerRequestDisplayRequired, true,
                                           display_active_) && ok;
    }

    settled_ = ok;
    if (ok) {
        warned_ = false;
        retry_after_ = {};
        if (mode_changed || retrying) {
            log::info("Playback power mode {}", mode_name(mode));
        }
    } else {
        retry_after_ = now + kRetryInterval;
    }
}

void PlaybackPowerRequest::release() {
    if (handle_ != INVALID_HANDLE_VALUE) {
        set_request(PowerRequestDisplayRequired, false, display_active_);
        set_request(PowerRequestSystemRequired, false, system_active_);
        CloseHandle(handle_);
    }
    handle_ = INVALID_HANDLE_VALUE;
    requested_mode_ = core::PlaybackPowerMode::AllowSleep;
    system_active_ = false;
    display_active_ = false;
    settled_ = true;
    warned_ = false;
    retry_after_ = {};
}

}  // namespace coax::win
