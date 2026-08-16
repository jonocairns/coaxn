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
#include "core/presentation.hpp"
#include "core/settings.hpp"
#include "player/mpv_player.hpp"
#include "player/playback_control.hpp"
#include "player/playback_session.hpp"
#include "player/session_target_registry.hpp"
#include "win/app_window.hpp"
#include "win/composition.hpp"
#include "win/power_request.hpp"
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
    void draw_channel_loading();
    // The column the channel list occupies. Shared with the playback overlay,
    // which starts where it ends rather than running underneath it.
    [[nodiscard]] static float browser_width();
    void draw_status_bar();
    // The window controls, revealed on the same terms as the playback bar and
    // over the strip the window is dragged by. The controls are the lesser
    // half: what the reveal is really for is showing where that strip is.
    void draw_title_bar();
    void draw_update_banner();
    void draw_diagnostics();
    // The right-click menu, and the surface that catches the click for it.
    // Submitted before every other surface so it stays behind them.
    void draw_window_menu();
    // Tells the window how much of its top edge is a drag strip and whether the
    // interface currently wants the clicks in it. Last thing in the frame, once
    // every surface has been submitted and hover is known.
    void publish_caption_region();
    // The rows the right-click menu and the overlay's settings menu both carry.
    // Two doors onto one room rather than two lists to keep in step.
    void draw_shared_menu_items();
    // Applies the frame change and persists it. The setting is the only thing
    // that survives a restart; the frame itself is applied immediately.
    void set_minimal_mode(bool minimal);
    // Forgets the provider and returns to the login screen. Reachable from the
    // right-click menu because auto-login means the login screen — and the
    // button on it that does this — is otherwise never seen again.
    void sign_out();
    // Carries out whatever the interface asked of the window. Called by the
    // loop, between turns, and never from inside a frame — see the fields it
    // drains for why.
    void apply_pending_window_changes();
    // Records the volume once it has stopped moving. Called every frame, and
    // writes almost never: a drag and a wheel flick both change the value many
    // times over, and none of those intermediate readings is worth a file.
    void persist_volume();
    // The height of the draggable strip, in physical pixels. Zero unless the
    // window is drawing its own frame — with a caption there is nothing to
    // stand in for, and in fullscreen there is no frame at all.
    [[nodiscard]] float caption_height() const;

    void begin_connect();
    void finish_connect();
    void begin_update_check();
    void finish_update_check();
    std::optional<core::RecoveryTransport> execute_supervisor_effect(
        const core::SupervisorEffect& effect);
    void on_supervisor_state_changed(const core::SupervisorState& state,
                                     core::SupervisorStateName previous_state);
    void observe_player_event(const player::PlayerEvent& event);
    void log_health_sample(const player::HealthSampleReport& report);
    void play(const core::Channel& channel);
    void play(const core::Channel& channel, player::SourceCorrelation correlation);
    void play_direct_media();
    void stop_playback();
    void start_playback();
    void toggle_playback();
    void apply_vsr();
    void handle_resize(int width, int height);
    void handle_display_change();
    void update_playback_power();
    void update_playback_power(const core::SupervisorState& state);
    void handle_resume();
    // Drains any device loss the UI layer latched and drives the bounded
    // rebuild. Called once per frame, which is also what paces the retries.
    void service_presentation();
    // Tears the presentation surface down and builds it again on a fresh
    // device. Resuming the channel is not done here: that is playback recovery
    // and belongs to the supervisor.
    bool rebuild_presentation();
    // Whether the turn about to run will present, and if not whether that is
    // temporary. Ready has to mean the present really happens, because the
    // vsync wait inside it is what the loop is being paced by: end_frame
    // returns before presenting on a lost device or a missing render target,
    // and either of those with the phase reading Ready is the busy-wait again.
    [[nodiscard]] core::PresentationPhase presentation_phase() const {
        const bool surface_ready =
            presentation_ready_ && ui_.has_render_target() && !ui_.device_lost();
        return core::decide_presentation_phase(surface_ready, presentation_budget_.exhausted());
    }
    // How long the message pump may block before the next turn has to run.
    [[nodiscard]] DWORD next_turn_wait_ms() const;
    void load_saved_portal();
    void save_portal() const;
    [[nodiscard]] bool channel_loading() const;

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
    win::PlaybackPowerRequest power_request_;
    player::MpvPlayer    player_;
    player::SessionTargetRegistry target_registry_;
    core::ChannelIndex   channels_;

    core::SteadySupervisorClock supervisor_clock_;
    player::PlaybackSession     playback_session_;

    std::unique_ptr<xtream::Client> client_;
    xtream::Credentials             credentials_;

    // What survives a restart. Loaded before the window exists, because the
    // frame it asks for has to be in place by the time the window is created
    // rather than applied over a caption the user then watches disappear.
    core::Settings settings_;

    // Window changes the interface has asked for, held until the turn ends.
    //
    // Every one of these resizes the client area, and the SetWindowPos that
    // does it delivers WM_SIZE *synchronously* — from which the window
    // procedure draws a frame, because a resize drag owns the thread and would
    // otherwise show a stretched copy of the last one. Applied from a menu item
    // that is itself being drawn, that is a frame begun inside a frame and a
    // render target rebuilt under a live draw list.
    //
    // So the rule is that nothing drawing a frame touches the window: it leaves
    // the request here and the loop makes the call, where the WM_SIZE that
    // follows draws an ordinary frame like any other.
    std::optional<bool> pending_minimal_frame_;
    std::optional<bool> pending_fullscreen_;
    bool                pending_minimize_ = false;
    bool                pending_maximize_ = false;

    Stage       stage_ = Stage::Login;
    std::string status_;
    bool        status_error_ = false;
    std::string direct_media_;
    bool        direct_media_active_ = false;

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
    // The selected channel owns its loading state until its exact generation
    // produces a first frame or fails. A generation fence matters here: a late
    // frame from the channel being replaced must not dismiss the new loader.
    std::optional<core::Generation> loading_channel_generation_;
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
    player::PlaybackIntent playback_intent_ = player::PlaybackIntent::Running;
    player::PlaybackControlCapability playback_control_capability_ =
        player::PlaybackControlCapability::RestartAtLiveEdge;
    std::optional<core::TimePoint> failure_power_grace_until_;
    int         volume_           = 100;
    // Where the volume was before the speaker was clicked, so unmuting puts it
    // back instead of leaving the way up to a drag.
    int         pre_mute_volume_  = 100;
    // The reading on the previous frame. A value that matches it is one that
    // has stopped moving, which is the point at which it is worth recording.
    int         volume_last_frame_ = 100;

    // The playback overlay hides itself once the pointer settles. Held as a
    // fraction rather than a boolean so it crosses rather than blinks.
    double      last_pointer_activity_ = 0.0;
    float       status_bar_fade_       = 1.0f;
    // The title strip crosses on its own clock. Same constants as the bar
    // below, but it is held open by a different region and has to survive the
    // stages the playback bar is not drawn in.
    float       title_bar_fade_        = 1.0f;
    // Whether either menu was open last frame. The fade is decided before the
    // window owning those popups is submitted, so the answer has to be carried
    // over rather than asked for.
    bool        overlay_menu_open_     = false;

    // mpv reports video dimensions asynchronously after a load, so the scale
    // factor has to be recomputed when they arrive rather than at play() time.
    int         last_video_width_  = 0;
    int         last_video_height_ = 0;

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

};

}  // namespace coax::app
