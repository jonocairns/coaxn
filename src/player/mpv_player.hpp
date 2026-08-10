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
#include "player/buffer_phase_gate.hpp"
#include "player/load_diagnostics.hpp"
#include "player/player_event_adapter.hpp"
#include "player/playback_observability.hpp"
#include "win/com_ptr.hpp"

struct mpv_handle;
struct mpv_event_property;
struct IUnknown;

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

struct PlaybackTarget {
    std::string url;
    core::Generation generation;
    core::LoadAttempt load_attempt;
    core::LoadIntent load_intent = core::LoadIntent::FreshSelection;
    core::RecoveryTransport transport = core::RecoveryTransport::MpegTs;
    bool probed_format_forced = false;
    SourceCorrelation correlation;
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
              core::LoadAttempt load_attempt, core::RecoveryTransport transport,
              bool force_probed_format = false,
              SourceCorrelation correlation = {});
    void stop(core::Generation generation);

    [[nodiscard]] std::optional<core::RecoveryTransport> reopen_current(
        core::Generation generation, core::LoadAttempt load_attempt,
        bool force_probed_format = false,
        bool require_hls = false);
    [[nodiscard]] std::optional<core::RecoveryTransport> recreate_player(
        core::Generation generation, core::LoadAttempt load_attempt, std::string& error);
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
    EngineDiagnosticLogGate engine_diagnostic_log_gate_;

    double vsr_scale_ = 1.0;
    bool vsr_enabled_ = false;
    std::string applied_filter_;
};

}  // namespace coax::player
