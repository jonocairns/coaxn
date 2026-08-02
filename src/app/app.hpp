#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "core/channel_index.hpp"
#include "player/live_sync.hpp"
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
    void draw_status_bar();
    void draw_diagnostics();

    void begin_connect();
    void finish_connect();
    void update_live_sync();
    void play(const core::Channel& channel);
    void apply_vsr();
    void handle_resize(int width, int height);
    void load_saved_portal();
    void save_portal() const;

    win::AppWindow       window_;
    win::UiLayer         ui_;
    win::CompositionTree composition_;
    player::MpvPlayer    player_;
    core::ChannelIndex   channels_;

    std::unique_ptr<xtream::Client> client_;
    xtream::Credentials             credentials_;

    Stage       stage_ = Stage::Login;
    std::string status_;
    std::string direct_media_;

    // Catalog loading runs off the UI thread; these hand the result back.
    std::thread              connect_thread_;
    std::atomic<bool>        connect_done_{false};
    std::mutex               connect_mutex_;
    xtream::Catalog          connect_catalog_;
    std::string              connect_error_;

    // UI state
    std::string portal_url_;
    std::string username_;
    std::string password_;
    std::string search_;
    std::string playing_channel_id_;
    std::string playing_channel_name_;
    bool        show_browser_     = true;
    bool        show_diagnostics_ = false;
    bool        vsr_enabled_      = true;
    bool        paused_           = false;
    int         volume_           = 100;

    // mpv reports video dimensions asynchronously after a load, so the scale
    // factor has to be recomputed when they arrive rather than at play() time.
    int         last_video_width_  = 0;
    int         last_video_height_ = 0;

    // Live-offset control. The rising edge of paused-for-cache is what counts
    // as a rebuffer; the flag stays true for the whole stall.
    player::LiveSync live_sync_;
    bool             was_paused_for_cache_ = false;
    int              rebuffer_count_       = 0;
};

}  // namespace coax::app
