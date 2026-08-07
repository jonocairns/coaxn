#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "app/update_check.hpp"
#include "core/channel_index.hpp"
#include "core/playback_health.hpp"
#include "core/presentation.hpp"
#include "core/supervisor_host.hpp"
#include "player/live_sync.hpp"
#include "player/live_sync_gate.hpp"
#include "player/mpv_player.hpp"
#include "win/app_window.hpp"
#include "win/composition.hpp"
#include "win/ui_layer.hpp"
#include "xtream/xtream_client.hpp"

namespace coax::app {

// Wires the portable core, the provider client and the Windows presentation
// layer together and owns the frame loop.
class App {
public:
    App();
    ~App();

    // Plays a media URL directly at startup instead of showing the login
    // screen. Used to exercise the presentation path without a provider —
    // mpv's synthetic sources work here, e.g. av://lavfi:testsrc2.
    void set_direct_media(std::string url) { direct_media_ = std::move(url); }

    // Returns the process exit code.
    int run();

private:
    enum class Stage { Login, Connecting, Browsing };

    bool initialize(std::string& error);
    void shutdown();

    void draw_frame();
    void draw_login();
    void draw_browser();
    // The column the channel list occupies. Shared with the playback overlay,
    // which starts where it ends rather than running underneath it.
    [[nodiscard]] static float browser_width();
    void draw_status_bar();
    void draw_update_banner();
    void draw_diagnostics();

    void begin_connect();
    void finish_connect();
    void begin_update_check();
    void finish_update_check();
    void begin_health_load();
    void process_player_events();
    void flush_pending_stream_ends();
    void sample_playback_health();
    void execute_supervisor_effect(const core::SupervisorEffect& effect);
    void on_supervisor_state_changed(const core::SupervisorState& state);
    void update_live_sync();
    void play(const core::Channel& channel);
    void apply_vsr();
    void handle_resize(int width, int height);
    void handle_display_change();
    void handle_resume();
    // Drains any device loss the UI layer latched and drives the bounded
    // rebuild. Called once per frame, which is also what paces the retries.
    void service_presentation();
    // Tears the presentation surface down and builds it again on a fresh
    // device. Resuming the channel is not done here: that is playback recovery
    // and belongs to the supervisor.
    bool rebuild_presentation();
    void load_saved_portal();
    void save_portal() const;

    // The status line is both the progress report and the error channel, so
    // what it is carrying has to be recorded alongside the text for the login
    // screen to colour it.
    void set_status(std::string text, bool error = false) {
        status_       = std::move(text);
        status_error_ = error;
    }

    win::AppWindow       window_;
    win::UiLayer         ui_;
    win::CompositionTree composition_;
    player::MpvPlayer    player_;
    core::ChannelIndex   channels_;

    core::SteadySupervisorClock supervisor_clock_;
    core::PlaybackSupervisor    supervisor_;

    std::unique_ptr<xtream::Client> client_;
    xtream::Credentials             credentials_;

    Stage       stage_ = Stage::Login;
    std::string status_;
    bool        status_error_ = false;
    std::string direct_media_;

    // Catalog loading runs off the UI thread; these hand the result back.
    std::thread              connect_thread_;
    std::atomic<bool>        connect_done_{false};
    std::mutex               connect_mutex_;
    xtream::Catalog          connect_catalog_;
    std::string              connect_error_;

    // Update check. Runs once at startup on its own thread. The worker only
    // ever touches update_result_; the UI thread joins before moving it into
    // update_available_, which is the one the frame loop reads.
    std::thread               update_thread_;
    std::atomic<bool>         update_done_{false};
    std::optional<UpdateInfo> update_result_;
    std::optional<UpdateInfo> update_available_;
    bool                      update_dismissed_ = false;

    // UI state
    std::string portal_url_;
    std::string username_;
    std::string password_;
    std::string search_;
    std::string playing_channel_id_;
    std::string playing_channel_name_;
    bool        show_browser_     = true;
    // Whether the previous frame was filtering, so the frame a search clears
    // can collapse the categories it opened. Without the edge the collapsed
    // state would have to be forced every frame, which is what stopped the
    // headers from opening on click.
    bool        search_was_active_ = false;
    bool        show_diagnostics_ = false;
    // Reused destination for the log ring's snapshot. The panel cannot iterate
    // the ring in place -- worker threads are writing it -- and holding the
    // buffer keeps the per-frame copy from also being a per-frame allocation.
    std::vector<std::string> log_snapshot_;
    // Whether mpv's swap chain is in the composition tree. Nothing else paints
    // the area behind the UI, so this decides whether the backdrop has to.
    bool        video_attached_   = false;
    bool        vsr_enabled_      = true;
    bool        paused_           = false;
    int         volume_           = 100;
    // Where the volume was before the speaker was clicked, so unmuting puts it
    // back instead of leaving the way up to a drag.
    int         pre_mute_volume_  = 100;

    // The playback overlay hides itself once the pointer settles. Held as a
    // fraction rather than a boolean so it crosses rather than blinks.
    double      last_pointer_activity_ = 0.0;
    float       status_bar_fade_       = 1.0f;
    // Whether the overlay's settings menu was open last frame. The fade is
    // decided before the window owning that popup is submitted, so the answer
    // has to be carried over rather than asked for.
    bool        overlay_menu_open_     = false;

    // mpv reports video dimensions asynchronously after a load, so the scale
    // factor has to be recomputed when they arrive rather than at play() time.
    int         last_video_width_  = 0;
    int         last_video_height_ = 0;

    // Live-offset control. The gate decides what each turn is allowed to learn
    // from mpv's cache signalling; the controller decides the speed.
    player::LiveSync     live_sync_;
    player::LiveSyncGate live_sync_gate_;
    int                  rebuffer_count_ = 0;

    // Presentation lifetime. The budget bounds and paces rebuilds; the rest is
    // what F1 reports, kept here rather than in the player's diagnostics
    // because it outlives the UI device it describes.
    // Whether the UI device and composition tree are both up. A rebuild drops
    // it, and only a complete rebuild restores it: between those points the
    // ImGui D3D11 backend has been shut down, and drawing a frame against it
    // dereferences a null backend rather than failing.
    bool presentation_ready_ = false;
    core::PresentationRebuildBudget presentation_budget_;
    std::string last_device_loss_;
    int         device_loss_events_    = 0;
    int         presentation_rebuilds_ = 0;

    core::Generation generation_;
    std::optional<core::PlaybackHealthState> playback_health_;
    core::BufferHealthSnapshot health_snapshot_;
    core::SupervisorStatsSnapshot supervisor_snapshot_;
    core::TimePoint next_health_sample_{};
    bool first_frame_seen_ = false;
    bool stall_reported_ = false;
    bool decode_stall_reported_ = false;
    bool exact_failure_reported_ = false;
    std::optional<bool> last_cache_state_dispatched_;

    struct PendingStreamEnd {
        core::Generation generation;
        core::EndReason reason;
        core::TimePoint dispatch_at;
    };
    std::vector<PendingStreamEnd> pending_stream_ends_;
};

}  // namespace coax::app
