#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/playback_health.hpp"
#include "core/playback_types.hpp"
#include "core/presentation.hpp"
#include "player/player_event_adapter.hpp"
#include "player/buffer_phase_gate.hpp"
#include "win/com_ptr.hpp"

struct mpv_handle;
struct mpv_event_property;
struct IUnknown;

namespace coax::player {

// Which mpv path produced the current attachment. mpv's client API documents an
// initial property notification but warns that later changes may not always
// notify, so both paths exist; this records which one is actually doing the
// work, rather than assuming observation alone is sufficient.
enum class SwapchainAcquisition { None, PropertyObservation, VideoReconfig };

const char* to_string(SwapchainAcquisition value);

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

    // Presentation attachment, reported as separate readings rather than one
    // word. "attached" alone cannot distinguish a live attachment from a stale
    // one, which is the failure this exists to make visible. Epochs count from
    // one, so zero is reserved for the detached identity.
    bool swapchain_attached = false;
    std::uint64_t swapchain_epoch = 1;
    // Replacements are the ones that matter: mpv built a different object, at a
    // different address. Re-attachments also count the precautionary cycles the
    // epoch rule forces when a reconfiguration reports the same address — which
    // a window resize can produce steadily, so a single combined number would
    // say nothing about whether the swap chain was ever actually replaced.
    int swapchain_replacements = 0;
    int swapchain_reattachments = 0;
    SwapchainAcquisition swapchain_acquisition = SwapchainAcquisition::None;

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
    // Returns whether the presentation layer now holds exactly what it was
    // handed. A refused attachment must not be recorded as one: it would both
    // report a live attachment that does not exist and suppress the next
    // identical notification as a duplicate, leaving no way back.
    using SwapchainCallback = std::function<bool(void* swapchain)>;

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

    // Clears the attachment and moves to a new epoch, so an address equal to
    // the one just released cannot be mistaken for it later. Called before the
    // composition tree holding the content is torn down.
    void detach_swapchain();

    void on_swapchain(SwapchainCallback callback) { swapchain_callback_ = std::move(callback); }
    [[nodiscard]] const Diagnostics& diagnostics() const { return diagnostics_; }
    [[nodiscard]] bool initialized() const { return mpv_ != nullptr; }
    [[nodiscard]] const std::optional<PlaybackTarget>& current_target() const { return target_; }

private:
    bool initialize_backend(std::string& error);
    void destroy_backend();
    void handle_property(std::uint64_t observe_id, const mpv_event_property& property);
    void publish_swapchain(void* swapchain, SwapchainAcquisition source);
    // Reads display-swapchain directly. The observation path is the primary
    // one, but the client API only guarantees the initial notification.
    void acquire_swapchain(SwapchainAcquisition source);
    // Moves past every address mpv has published so far. Called wherever the
    // video output can be torn down or rebuilt, which is the only boundary
    // across which an address can be reused by a different object.
    void bump_swapchain_epoch();
    std::uint64_t next_request_id();
    bool issue_load(bool force_probed_format);
    bool issue_buffer_property(core::Generation generation, core::BufferPhase phase,
                               BufferProperty property, double value);

    mpv_handle* mpv_ = nullptr;
    // The reference is held here for exactly as long as the swap chain is
    // attached. DirectComposition takes its own on SetContent, but the window
    // between reading the property and that call is otherwise unowned, and
    // mpv's video output does not tear down on the UI thread.
    win::ComPtr<IUnknown> held_swapchain_;
    core::SwapchainIdentity attached_;
    // From one, so that the zero in a default-constructed identity can only
    // ever mean "nothing attached".
    std::uint64_t swapchain_epoch_ = 1;
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
