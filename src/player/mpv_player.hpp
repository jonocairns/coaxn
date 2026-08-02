#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/playback_health.hpp"
#include "core/playback_types.hpp"
#include "player/player_event_adapter.hpp"
#include "player/buffer_phase_gate.hpp"

struct mpv_handle;
struct mpv_event_property;

namespace coax::player {

struct PlayerConfig {
    int composition_width = 1920;
    int composition_height = 1080;
    bool hardware_decode = true;

    // Off: reconnecting a non-seekable live stream can replay content. Fresh
    // reopen is owned by the supervisor.
    bool transport_reconnect = false;

    // Zero leaves provider-sensitive probing at the pinned runtime defaults.
    double analyze_duration_seconds = 0.0;
    int probe_size_bytes = 0;
};

struct Diagnostics {
    std::string video_codec;
    std::string hwdec_requested;
    std::string hwdec_active;
    int video_width = 0;
    int video_height = 0;
    bool vsr_requested = false;
    bool vsr_filter_attached = false;

    bool core_idle = false;
    bool paused_for_cache = false;
    double cache_seconds = 0.0;
    std::string swapchain_state = "none";

    double live_target_seconds = 0.0;
    double playback_speed = 1.0;
    int rebuffer_count = 0;
    double last_load_seconds = 0.0;

    // Explicitly separate mpv restart edges from the health fold's classified
    // timeline deviations. They are not interchangeable counters.
    int mpv_playback_restart_events = 0;
    int health_discontinuities = 0;

    core::BufferPhase buffer_phase = core::BufferPhase::Zap;
    BufferPhaseCommandState buffer_phase_command_state =
        BufferPhaseCommandState::Unissued;
    int buffer_commands_accepted = 0;
    int buffer_commands_rejected = 0;

    std::optional<double> av_sync_seconds;
    std::optional<double> cache_duration_seconds;
    std::optional<double> cache_end_seconds;
    std::optional<double> input_rate_bytes_per_second;
    std::optional<double> playback_time_seconds;
    std::optional<double> video_fps_estimate;
    std::optional<double> container_fps;
};

// Clears observations that belong to one load while retaining process-lifetime
// counters and user configuration. Every new load starts in the Zap policy
// phase; command state separately records whether mpv has confirmed it.
void reset_load_observations(Diagnostics& diagnostics);

struct PlaybackTarget {
    std::string url;
    core::Generation generation;
    core::RecoveryTransport transport = core::RecoveryTransport::MpegTs;
    bool probed_format_forced = false;
};

// RAII libmpv owner. The UI thread is the sole caller.
class MpvPlayer {
public:
    using SwapchainCallback = std::function<void(void* swapchain)>;

    MpvPlayer() = default;
    ~MpvPlayer();
    MpvPlayer(const MpvPlayer&) = delete;
    MpvPlayer& operator=(const MpvPlayer&) = delete;

    bool initialize(const PlayerConfig& config, std::string& error);
    bool play(const std::string& url, core::Generation generation,
              core::RecoveryTransport transport, bool force_probed_format = false);
    void stop(core::Generation generation);

    [[nodiscard]] std::optional<core::RecoveryTransport> reopen_current(
        core::Generation generation, bool force_probed_format = false,
        bool require_hls = false);
    [[nodiscard]] std::optional<core::RecoveryTransport> recreate_player(
        core::Generation generation, std::string& error);
    bool apply_buffer_phase(core::Generation generation, core::BufferPhase phase);

    void set_composition_size(int width, int height);
    void set_vsr(bool enabled, double scale);
    void set_volume(int percent);
    void set_paused(bool paused);
    void set_speed(double speed);
    void set_live_sync_state(double target_seconds, int rebuffer_count);
    void set_health_discontinuities(int count) { diagnostics_.health_discontinuities = count; }
    void observe_buffer_command_result(core::Generation generation,
                                       const PropertyCommandResult& result);

    void pump();
    [[nodiscard]] std::vector<PlayerEvent> take_events() { return events_.drain(); }
    [[nodiscard]] core::PlaybackHealthObservation health_observation() const;

    void on_swapchain(SwapchainCallback callback) { swapchain_callback_ = std::move(callback); }
    [[nodiscard]] const Diagnostics& diagnostics() const { return diagnostics_; }
    [[nodiscard]] bool initialized() const { return mpv_ != nullptr; }
    [[nodiscard]] const std::optional<PlaybackTarget>& current_target() const { return target_; }

private:
    bool initialize_backend(std::string& error);
    void destroy_backend();
    void handle_property(std::uint64_t observe_id, const mpv_event_property& property);
    void publish_swapchain(void* swapchain);
    std::uint64_t next_request_id();
    bool issue_load(bool force_probed_format);
    bool issue_buffer_property(core::Generation generation, core::BufferPhase phase,
                               BufferProperty property, double value);

    mpv_handle* mpv_ = nullptr;
    void* swapchain_ = nullptr;
    SwapchainCallback swapchain_callback_;
    Diagnostics diagnostics_;
    PlayerConfig config_;
    bool has_config_ = false;
    std::optional<PlaybackTarget> target_;
    PlayerEventAdapter events_;

    std::uint64_t request_sequence_ = 10'000;
    BufferPhaseGate buffer_phase_gate_;
    std::optional<std::int64_t> current_entry_id_;
    std::chrono::steady_clock::time_point load_started_at_{};
    bool load_in_flight_ = false;
    bool file_loaded_ = false;
    bool transport_log_armed_ = false;
    bool transport_classification_reported_ = false;

    double vsr_scale_ = 1.0;
    bool vsr_enabled_ = false;
    std::string applied_filter_;
};

}  // namespace coax::player
