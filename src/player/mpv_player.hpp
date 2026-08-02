#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

struct mpv_handle;
struct mpv_event_property;

namespace coax::player {

struct PlayerConfig {
    int  composition_width  = 1920;
    int  composition_height = 1080;
    bool hardware_decode    = true;

    // FFmpeg-level reconnect. Off by default: on a non-seekable live stream it
    // resumes at the wrong position and replays already-watched content.
    bool transport_reconnect = false;

    // Stream probing is deliberately left at the runtime default.
    //
    // Shortening it is the obvious way to cut tune-in time, and it is wrong:
    // provider MPEG-TS needs a full PMT before a track list exists, so a
    // truncated analysis makes slow-signalling channels unplayable rather than
    // merely slower. This was established against real provider streams in the
    // Electron implementation; see its supervisor/policy.ts.
    //
    // Zero means "leave the runtime default alone".
    double analyze_duration_seconds = 0.0;
    int    probe_size_bytes         = 0;
};

// Deliberately separates what was asked for, what mpv reports as configured,
// and what can actually be confirmed. Nothing here infers a vendor
// enhancement ran from the GPU model or from the filter being attached.
struct Diagnostics {
    std::string video_codec;
    std::string hwdec_requested;
    std::string hwdec_active;       // mpv's hwdec-current
    int         video_width  = 0;
    int         video_height = 0;

    bool vsr_requested       = false;
    bool vsr_filter_attached = false;
    // vsr_confirmed is intentionally absent: no reliable activation signal is
    // available, so the POC does not pretend to have one.

    bool        core_idle        = false;
    bool        paused_for_cache = false;
    double      cache_seconds    = 0.0;
    std::string swapchain_state  = "none";

    // Live-offset controller state.
    double      live_target_seconds = 0.0;
    double      playback_speed      = 1.0;
    int         rebuffer_count      = 0;

    // How long the last channel change took, and how many times mpv has had
    // to resynchronise mid-stream. Discontinuities are a property of the
    // provider's stream, not of playback settings, so they are counted rather
    // than treated as errors.
    double      last_load_seconds = 0.0;
    int         discontinuities   = 0;
};

// RAII wrapper over an in-process libmpv instance configured for D3D11
// composition output. Not thread-safe: drive it from the UI thread.
class MpvPlayer {
public:
    using SwapchainCallback = std::function<void(void* swapchain)>;

    MpvPlayer() = default;
    ~MpvPlayer();

    MpvPlayer(const MpvPlayer&)            = delete;
    MpvPlayer& operator=(const MpvPlayer&) = delete;

    bool initialize(const PlayerConfig& config, std::string& error);

    void play(const std::string& url);
    void stop();

    void set_composition_size(int width, int height);
    void set_vsr(bool enabled, double scale);
    void set_volume(int percent);
    void set_paused(bool paused);

    // Playback rate, used by the live-offset controller to converge on its
    // target without seeking. Audio pitch correction keeps small changes
    // inaudible.
    void set_speed(double speed);

    // Mirrors the live controller's state into the diagnostics block so the
    // overlay can show it without reaching into the controller itself.
    void set_live_sync_state(double target_seconds, int rebuffer_count);

    // Drains queued mpv events. Call once per frame from the UI thread.
    void pump();

    // Invoked when the composition swap chain first becomes available and
    // again whenever mpv replaces it.
    void on_swapchain(SwapchainCallback callback) { swapchain_callback_ = std::move(callback); }

    [[nodiscard]] const Diagnostics& diagnostics() const { return diagnostics_; }
    [[nodiscard]] bool               initialized()  const { return mpv_ != nullptr; }

private:
    void handle_property(uint64_t observe_id, const mpv_event_property& property);
    void publish_swapchain(void* swapchain);

    mpv_handle*       mpv_ = nullptr;
    void*             swapchain_ = nullptr;
    SwapchainCallback swapchain_callback_;
    Diagnostics       diagnostics_;
    double            vsr_scale_ = 1.0;
    bool              vsr_enabled_ = false;

    // Set when a load is issued, cleared when the file reports loaded, so the
    // channel-change cost is measured rather than estimated.
    std::chrono::steady_clock::time_point load_started_at_{};
    bool                                  load_in_flight_ = false;
    bool                                  first_restart_seen_ = false;

    // The filter string currently applied to mpv, so an unchanged graph is
    // never reassigned. Cleared on file load, where mpv resets the chain.
    std::string       applied_filter_;
};

}  // namespace coax::player
