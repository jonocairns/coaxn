#include "app/app.hpp"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <algorithm>
#include <chrono>
#include <format>

#include "util/log.hpp"
#include "player/recovery_effect_executor.hpp"
#include "win/credential_store.hpp"

namespace coax::app {
namespace {

constexpr int kInitialWidth  = 1600;
constexpr int kInitialHeight = 900;

// Copies a std::string into a fixed ImGui text buffer and back out again.
struct TextField {
    char buffer[512]{};

    explicit TextField(const std::string& initial) {
        const std::size_t count = std::min(initial.size(), sizeof(buffer) - 1);
        std::copy_n(initial.begin(), count, buffer);
        buffer[count] = '\0';
    }
};

template<class... Ts>
struct Overloaded : Ts... { using Ts::operator()...; };

}  // namespace

App::App()
    : supervisor_(
          supervisor_clock_,
          {.on_effect = [this](const core::SupervisorEffect& effect) {
               execute_supervisor_effect(effect);
           },
           .on_state_changed = [this](const core::SupervisorState& state) {
               on_supervisor_state_changed(state);
           },
           .on_transition = [](const core::SupervisorTransition& transition) {
               log::info("Supervisor {} -> {} generation {} attempt {} reason {} budget {:.0f}ms",
                         core::to_string(transition.from), core::to_string(transition.to),
                         transition.generation.value(), transition.attempt, transition.reason,
                         transition.elapsed_budget.count() * 1000.0);
           }}) {}

App::~App() {
    if (connect_thread_.joinable()) {
        connect_thread_.join();
    }
}

bool App::initialize(std::string& error) {
    if (!window_.create(L"Coax", kInitialWidth, kInitialHeight, error)) {
        return false;
    }

    if (!ImGui_ImplWin32_Init(window_.handle())) {
        error = "ImGui Win32 backend failed to initialize";
        return false;
    }

    if (!ui_.create(window_.width(), window_.height(), error)) {
        return false;
    }

    if (!composition_.create(window_.handle(), ui_.dxgi_device(), error)) {
        return false;
    }
    composition_.set_ui_content(ui_.swapchain());

    player::PlayerConfig config;
    config.composition_width  = window_.width();
    config.composition_height = window_.height();
    if (!player_.initialize(config, error)) {
        return false;
    }

    // Attaching on the callback rather than polling: the property is
    // unavailable until mpv's video output exists, and mpv may replace the
    // swap chain later.
    player_.on_swapchain([this](void* swapchain) {
        composition_.set_video_content(static_cast<IUnknown*>(swapchain));
    });

    window_.on_resize([this](int width, int height) { handle_resize(width, height); });

    if (!direct_media_.empty()) {
        stage_                = Stage::Browsing;
        playing_channel_name_ = "Direct media";
        status_               = "Direct media";
        generation_ = core::Generation{generation_.value() + 1};
        supervisor_.dispatch(core::ChannelRequested{generation_});
        begin_health_load();
        player_.play(direct_media_, generation_, core::RecoveryTransport::MpegTs);
    } else {
        load_saved_portal();
    }

    begin_update_check();
    return true;
}

void App::begin_update_check() {
    update_done_.store(false, std::memory_order_release);
    update_thread_ = std::thread([this] {
        update_result_ = check_for_update();
        update_done_.store(true, std::memory_order_release);
    });
}

void App::finish_update_check() {
    if (!update_done_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    // Joining is what publishes the worker's write; nothing reads
    // update_available_ before this point.
    if (update_thread_.joinable()) {
        update_thread_.join();
    }
    update_available_ = std::move(update_result_);
    update_result_.reset();
}

void App::handle_resize(int width, int height) {
    ui_.resize(width, height);
    composition_.set_ui_content(ui_.swapchain());
    player_.set_composition_size(width, height);
    apply_vsr();
}

void App::load_saved_portal() {
    std::string stored;
    if (!win::CredentialStore::load(stored)) {
        return;
    }

    // Stored as three newline-separated fields.
    const auto first  = stored.find('\n');
    const auto second = stored.find('\n', first == std::string::npos ? 0 : first + 1);
    if (first == std::string::npos || second == std::string::npos) {
        return;
    }

    portal_url_ = stored.substr(0, first);
    username_   = stored.substr(first + 1, second - first - 1);
    password_   = stored.substr(second + 1);
    status_     = "Saved portal loaded";
    log::info("Restored saved portal for {}", portal_url_);
}

void App::save_portal() const {
    win::CredentialStore::save(std::format("{}\n{}\n{}", portal_url_, username_, password_));
}

void App::begin_connect() {
    if (stage_ == Stage::Connecting) {
        return;
    }

    xtream::Credentials credentials;
    // A pasted player_api.php or get.php link carries everything we need.
    if (!xtream::parse_portal_url(portal_url_, credentials)) {
        credentials.base_url = portal_url_;
        credentials.username = username_;
        credentials.password = password_;
    } else {
        username_ = credentials.username;
        password_ = credentials.password;
    }

    while (!credentials.base_url.empty() && credentials.base_url.back() == '/') {
        credentials.base_url.pop_back();
    }

    if (credentials.base_url.empty() || credentials.username.empty() ||
        credentials.password.empty()) {
        status_ = "Portal URL, username and password are all required";
        return;
    }

    credentials_ = credentials;
    stage_       = Stage::Connecting;
    status_      = "Connecting...";
    connect_done_.store(false, std::memory_order_release);

    if (connect_thread_.joinable()) {
        connect_thread_.join();
    }

    connect_thread_ = std::thread([this, credentials] {
        xtream::Client  client(credentials);
        xtream::Catalog catalog;
        std::string     error;
        const bool      ok = client.fetch_catalog(catalog, error);

        {
            std::scoped_lock lock(connect_mutex_);
            connect_catalog_ = std::move(catalog);
            connect_error_   = ok ? std::string{} : error;
        }
        connect_done_.store(true, std::memory_order_release);
    });
}

void App::finish_connect() {
    if (!connect_done_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    if (connect_thread_.joinable()) {
        connect_thread_.join();
    }

    xtream::Catalog catalog;
    std::string     error;
    {
        std::scoped_lock lock(connect_mutex_);
        catalog = std::move(connect_catalog_);
        error   = connect_error_;
    }

    if (!error.empty()) {
        stage_  = Stage::Login;
        status_ = error;
        log::error("Connect failed: {}", error);
        return;
    }

    client_ = std::make_unique<xtream::Client>(credentials_);
    channels_.reset(std::move(catalog.categories), std::move(catalog.channels));
    stage_  = Stage::Browsing;
    status_ = std::format("{} channels", channels_.channel_count());
    save_portal();
}

void App::play(const core::Channel& channel) {
    if (!client_) {
        return;
    }
    playing_channel_id_   = channel.id;
    playing_channel_name_ = channel.name;
    paused_               = false;

    // Latency learned on one channel says nothing about the next.
    live_sync_.reset();
    was_paused_for_cache_ = false;
    rebuffer_count_       = 0;
    player_.set_speed(1.0);

    generation_ = core::Generation{generation_.value() + 1};
    supervisor_.dispatch(core::ChannelRequested{generation_});
    begin_health_load();
    // Xtream resolves this endpoint as a continuous .ts request; transport is
    // therefore resolved with the load rather than guessed from HTTP(S).
    player_.play(client_->stream_url(channel), generation_, core::RecoveryTransport::MpegTs);
    apply_vsr();
    status_ = std::format("Playing {}", channel.name);
}

void App::begin_health_load() {
    const auto now = supervisor_clock_.now();
    const auto target = core::buffer_phase_targets(core::BufferPhase::Zap);
    playback_health_ = core::initial_playback_health(core::BufferPhase::Zap, now,
                                                     target.cache_seconds);
    health_snapshot_ = playback_health_->snapshot;
    next_health_sample_ = now + core::kDefaultHealthPolicy.sample_interval;
    first_frame_seen_ = false;
    stall_reported_ = false;
    decode_stall_reported_ = false;
    exact_failure_reported_ = false;
    last_cache_state_dispatched_.reset();
    player_.set_health_discontinuities(0);
}

void App::process_player_events() {
    for (const auto& event : player_.take_events()) {
        std::visit(Overloaded{
            [&](const player::LoadCommandResult& result) {
                if (!result.accepted) {
                    log::warn("Load command rejected for generation {} with structured error {}",
                              event.generation.value(), result.error);
                    supervisor_.dispatch(core::SourceFailed{event.generation});
                    return;
                }
                const auto& target = player_.current_target();
                if (!target || target->generation != event.generation) return;
                supervisor_.dispatch(core::StreamLoadIssued{event.generation,
                                                            target->transport});
            },
            [&](const player::FirstPlaybackStart&) {
                if (event.generation != generation_) return;
                first_frame_seen_ = true;
                supervisor_.dispatch(core::FirstFrame{event.generation});
            },
            [&](const player::EndFileEvent& ended) {
                if (ended.reason == player::PlayerEndReason::Stop ||
                    ended.reason == player::PlayerEndReason::Quit ||
                    ended.reason == player::PlayerEndReason::Redirect) return;
                if (event.generation == generation_ && exact_failure_reported_) return;
                core::EndReason reason = core::EndReason::Unknown;
                if (ended.reason == player::PlayerEndReason::Eof) reason = core::EndReason::Eof;
                else if (ended.reason == player::PlayerEndReason::Error)
                    reason = core::EndReason::Error;
                // The pinned runtime can emit its exact HTTP/HLS diagnostic
                // just after end-file. Hold the generic fallback briefly so
                // one physical failure cannot spend two recovery attempts.
                pending_stream_ends_.push_back({
                    event.generation, reason,
                    supervisor_clock_.now() + core::milliseconds(50)});
            },
            [&](const player::PlaybackStopped& stopped) {
                if (stopped.kind == player::IntentionalStopKind::Requested) {
                    supervisor_.dispatch(core::PlaybackStopped{event.generation});
                }
            },
            [&](const player::BackendFailed& failed) {
                log::warn("libmpv backend failed for generation {} with structured error {}",
                          event.generation.value(), failed.error);
                supervisor_.dispatch(core::ProcessExited{event.generation});
            },
            [&](const player::PropertyCommandResult& result) {
                player_.observe_buffer_command_result(event.generation, result);
                const char* property = result.property == player::BufferProperty::CacheSeconds
                    ? "cache-secs" : "demuxer-readahead-secs";
                if (result.accepted) {
                    log::info("Buffer phase command accepted: {} generation {}",
                              property, event.generation.value());
                } else {
                    log::warn("Buffer phase command rejected: {} generation {} error {}",
                              property, event.generation.value(), result.error);
                }
            },
            [&](const player::PlayerAuthenticationRejected&) {
                if (event.generation == generation_) exact_failure_reported_ = true;
                std::erase_if(pending_stream_ends_, [&](const auto& pending) {
                    return pending.generation == event.generation;
                });
                supervisor_.dispatch(core::AuthRejected{event.generation});
            },
            [&](const player::TransportFailureDetected& failure) {
                if (event.generation == generation_) exact_failure_reported_ = true;
                std::erase_if(pending_stream_ends_, [&](const auto& pending) {
                    return pending.generation == event.generation;
                });
                supervisor_.dispatch(core::StreamEnded{
                    event.generation, core::EndReason::Error, failure.reason});
            }}, event.payload);
    }
    flush_pending_stream_ends();
}

void App::flush_pending_stream_ends() {
    const auto now = supervisor_clock_.now();
    for (auto pending = pending_stream_ends_.begin(); pending != pending_stream_ends_.end();) {
        if (pending->dispatch_at > now) {
            ++pending;
            continue;
        }
        supervisor_.dispatch(core::StreamEnded{
            pending->generation, pending->reason, std::nullopt});
        pending = pending_stream_ends_.erase(pending);
    }
}

void App::sample_playback_health() {
    if (!playback_health_ || !player_.current_target()) return;
    if (supervisor_.current().name == core::SupervisorStateName::Idle ||
        supervisor_.current().name == core::SupervisorStateName::Failed) return;
    const auto now = supervisor_clock_.now();
    if (now < next_health_sample_) return;
    next_health_sample_ = now + core::kDefaultHealthPolicy.sample_interval;

    const auto& diagnostics = player_.diagnostics();
    const auto target = core::buffer_phase_targets(diagnostics.buffer_phase);
    const auto fold = core::fold_playback_health(
        *playback_health_, player_.health_observation(), now,
        {.container_fps = diagnostics.container_fps,
         .first_frame_seen = first_frame_seen_,
         .main_process_cpu_percent = std::nullopt,
         .phase = diagnostics.buffer_phase,
         .target_seconds = target.cache_seconds});
    playback_health_ = fold.state;
    health_snapshot_ = fold.state.snapshot;
    player_.set_health_discontinuities(fold.state.discontinuities);

    const auto generation = player_.current_target()->generation;
    if (!last_cache_state_dispatched_ ||
        *last_cache_state_dispatched_ != diagnostics.paused_for_cache) {
        last_cache_state_dispatched_ = diagnostics.paused_for_cache;
        supervisor_.dispatch(core::CacheState{generation, diagnostics.paused_for_cache});
    }
    if (fold.discontinuity) {
        log::warn("Timeline discontinuity #{} generation {} (classified by progress deviation)",
                  fold.state.discontinuities, generation.value());
    }
    if (fold.interrupted) {
        supervisor_.dispatch(core::PlaybackInterrupted{generation});
    }
    if (fold.stalled && !stall_reported_) {
        stall_reported_ = true;
        supervisor_.dispatch(core::PlaybackStalled{
            generation,
            fold.state.verdict == core::PlaybackHealthVerdict::OpenStalled
                ? core::StallKind::Open : core::StallKind::Progress});
    } else if (fold.decode_stalled && !decode_stall_reported_) {
        decode_stall_reported_ = true;
        supervisor_.dispatch(core::DecodeStalled{generation});
    }
}

void App::execute_supervisor_effect(const core::SupervisorEffect& effect) {
    std::string error;
    auto settle_load = [this](std::optional<core::RecoveryTransport> result) {
        if (result) begin_health_load();
        return result;
    };
    const player::RecoveryExecutor executor{
        .reopen_stream = [&](core::Generation generation) {
            return settle_load(player_.reopen_current(generation));
        },
        .reload_hls_live = [&](core::Generation generation) {
            return settle_load(player_.reopen_current(generation, false, true));
        },
        .reopen_probed = [&](core::Generation generation) {
            return settle_load(player_.reopen_current(generation, true));
        },
        .recreate_player = [&](core::Generation generation) {
            auto result = player_.recreate_player(generation, error);
            if (result) {
                // The new backend starts at 1.0x. Reset the controller too so
                // its cached old speed cannot suppress the write the new mpv needs.
                live_sync_.reset();
                was_paused_for_cache_ = false;
                rebuffer_count_ = 0;
                player_.set_speed(1.0);
                player_.set_volume(volume_);
                player_.set_paused(paused_);
                apply_vsr();
            }
            return settle_load(result);
        },
    };
    player::execute_recovery_effect(
        effect, &executor,
        [this](const core::SupervisorEvent& event) { supervisor_.dispatch(event); },
        [&](const core::SupervisorEffect&) {
            if (!error.empty()) log::error("Player recreation failed: {}", error);
            else log::warn("Recovery effect {} rejected for stale or unavailable target",
                           core::effect_name(effect.payload));
        });
}

void App::on_supervisor_state_changed(const core::SupervisorState& state) {
    supervisor_snapshot_ = core::project_supervisor_stats(state, supervisor_clock_.now());
    if (state.name == core::SupervisorStateName::Steady) {
        player_.apply_buffer_phase(state.generation, core::BufferPhase::Steady);
    } else if (state.name == core::SupervisorStateName::Failed) {
        status_ = state.failure
            ? std::format("Playback failed: {}", core::to_string(*state.failure))
            : "Playback failed";
    }
}

void App::update_live_sync() {
    const auto& diagnostics = player_.diagnostics();

    // Only the transition into a stall counts as a rebuffer; the flag stays
    // true for its whole duration, so an edge test avoids conceding latency
    // once per frame.
    if (diagnostics.paused_for_cache && !was_paused_for_cache_) {
        ++rebuffer_count_;
        live_sync_.notify_rebuffer();
        log::info("Rebuffer #{}; live target now {:.1f}s",
                  rebuffer_count_, live_sync_.target_offset_seconds());
    }
    was_paused_for_cache_ = diagnostics.paused_for_cache;

    // Holding the controller at 1.0 while stalled stops it from reacting to a
    // cache that is draining rather than mistimed.
    if (diagnostics.paused_for_cache || diagnostics.core_idle) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const double now_seconds =
        std::chrono::duration<double>(now.time_since_epoch()).count();

    if (const auto speed = live_sync_.update(diagnostics.cache_seconds, now_seconds)) {
        player_.set_speed(*speed);
    }

    player_.set_live_sync_state(live_sync_.target_offset_seconds(), rebuffer_count_);
}

void App::apply_vsr() {
    const auto& diagnostics = player_.diagnostics();

    // Only ask for super resolution when the source is actually smaller than
    // the target viewport; asking to "upscale" a 4K source is meaningless.
    double scale = 1.0;
    if (diagnostics.video_height > 0 && window_.height() > diagnostics.video_height) {
        scale = static_cast<double>(window_.height()) / diagnostics.video_height;
        scale = std::clamp(scale, 1.0, 4.0);
    }

    const bool worth_it = vsr_enabled_ && scale > 1.05;
    player_.set_vsr(worth_it, scale);
}

void App::draw_login() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(720.0f, 0.0f), ImGuiCond_Always);

    ImGui::Begin("Connect to provider", nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse);

    ImGui::TextWrapped(
        "Paste a full Xtream portal link (player_api.php or get.php) and the "
        "username and password are filled in automatically, or enter the "
        "server URL and credentials separately.");
    ImGui::Separator();

    TextField url(portal_url_);
    if (ImGui::InputText("Portal URL", url.buffer, sizeof(url.buffer))) {
        portal_url_ = url.buffer;
    }

    TextField user(username_);
    if (ImGui::InputText("Username", user.buffer, sizeof(user.buffer))) {
        username_ = user.buffer;
    }

    TextField pass(password_);
    if (ImGui::InputText("Password", pass.buffer, sizeof(pass.buffer),
                         ImGuiInputTextFlags_Password)) {
        password_ = pass.buffer;
    }

    ImGui::Spacing();

    const bool connecting = stage_ == Stage::Connecting;
    ImGui::BeginDisabled(connecting);
    if (ImGui::Button(connecting ? "Connecting..." : "Connect", ImVec2(160.0f, 0.0f))) {
        begin_connect();
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Forget saved portal")) {
        win::CredentialStore::clear();
        portal_url_.clear();
        username_.clear();
        password_.clear();
        status_ = "Saved portal cleared";
    }

    if (!status_.empty()) {
        ImGui::Spacing();
        ImGui::TextWrapped("%s", status_.c_str());
    }

    ImGui::End();
}

void App::draw_browser() {
    if (!show_browser_) {
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float          width    = std::min(viewport->WorkSize.x * 0.34f, 620.0f);

    ImGui::SetNextWindowPos(viewport->WorkPos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(width, viewport->WorkSize.y), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.88f);

    ImGui::Begin("Channels", nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    ImGui::TextUnformatted("Channels");
    ImGui::SameLine();
    ImGui::TextDisabled("(Tab hides, F1 diagnostics)");

    TextField search(search_);
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputTextWithHint("##search", "Search channels and categories",
                                 search.buffer, sizeof(search.buffer))) {
        search_ = search.buffer;
    }

    ImGui::Separator();

    const auto groups = channels_.filtered(search_);

    std::size_t shown = 0;
    for (const auto& group : groups) {
        shown += group.channels.size();
    }
    ImGui::TextDisabled("%zu of %zu channels", shown, channels_.channel_count());

    ImGui::BeginChild("channel-scroll", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);

    for (const auto& group : groups) {
        const char* label = group.category ? group.category->name.c_str() : "Uncategorised";

        // With an active search the matches are few, so opening the groups
        // saves a click; unfiltered the list stays collapsed and navigable.
        ImGui::SetNextItemOpen(!search_.empty(), ImGuiCond_Always);
        if (!ImGui::CollapsingHeader(label)) {
            continue;
        }

        // Only the visible slice of a category is submitted to ImGui, so a
        // provider with tens of thousands of channels still scrolls smoothly.
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(group.channels.size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const core::Channel* channel = group.channels[static_cast<std::size_t>(i)];
                const bool           playing = channel->id == playing_channel_id_;

                ImGui::PushID(channel->id.c_str());
                if (ImGui::Selectable(channel->name.c_str(), playing)) {
                    play(*channel);
                }
                ImGui::PopID();
            }
        }
    }

    ImGui::EndChild();
    ImGui::End();
}

void App::draw_status_bar() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float          height   = ImGui::GetFrameHeightWithSpacing() * 1.4f;

    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x,
                                   viewport->WorkPos.y + viewport->WorkSize.y - height));
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, height));
    ImGui::SetNextWindowBgAlpha(0.65f);

    ImGui::Begin("##status", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);

    if (!playing_channel_name_.empty()) {
        ImGui::Text("%s", playing_channel_name_.c_str());
        ImGui::SameLine();
    }

    const auto& diagnostics = player_.diagnostics();
    if (diagnostics.paused_for_cache) {
        ImGui::TextDisabled("| buffering");
        ImGui::SameLine();
    }

    ImGui::TextDisabled("| %s", status_.c_str());

    ImGui::SameLine(ImGui::GetWindowWidth() - 420.0f);
    if (ImGui::Checkbox("Super resolution", &vsr_enabled_)) {
        apply_vsr();
    }
    ImGui::SameLine();
    if (ImGui::Button(paused_ ? "Play" : "Pause")) {
        paused_ = !paused_;
        player_.set_paused(paused_);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::SliderInt("##volume", &volume_, 0, 130, "vol %d")) {
        player_.set_volume(volume_);
    }

    ImGui::End();
}

void App::draw_update_banner() {
    if (!update_available_ || update_dismissed_) {
        return;
    }

    // Top-right, clear of the channel list and the status bar. This is a
    // notice, not a modal: someone mid-programme can ignore or dismiss it and
    // nothing blocks playback.
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - 16.0f, viewport->WorkPos.y + 16.0f),
        ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.85f);

    ImGui::Begin("##update", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoFocusOnAppearing);

    ImGui::Text("Version %s is available", update_available_->version.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Download")) {
        open_in_browser(update_available_->page_url);
        update_dismissed_ = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Later")) {
        update_dismissed_ = true;
    }

    ImGui::End();
}

void App::draw_diagnostics() {
    if (!show_diagnostics_) {
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - 20.0f, viewport->WorkPos.y + 20.0f),
        ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.85f);

    ImGui::Begin("Diagnostics", &show_diagnostics_,
                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing);

    const auto& d = player_.diagnostics();

    ImGui::SeparatorText("Presentation");
    ImGui::Text("Composition swap chain : %s", d.swapchain_state.c_str());
    ImGui::Text("Window                 : %dx%d", window_.width(), window_.height());

    ImGui::SeparatorText("Decode");
    ImGui::Text("Codec                  : %s",
                d.video_codec.empty() ? "-" : d.video_codec.c_str());
    ImGui::Text("Source                 : %dx%d", d.video_width, d.video_height);
    ImGui::Text("Hardware decode wanted : %s", d.hwdec_requested.c_str());
    ImGui::Text("Hardware decode active : %s",
                d.hwdec_active.empty() ? "-" : d.hwdec_active.c_str());

    ImGui::SeparatorText("Super resolution");
    // Requested, attached and confirmed are distinct on purpose. There is no
    // reliable signal that the driver actually ran RTX VSR on a frame, so this
    // never claims it did.
    ImGui::Text("Requested              : %s", d.vsr_requested ? "yes" : "no");
    ImGui::Text("Filter attached        : %s", d.vsr_filter_attached ? "yes" : "no");
    ImGui::TextDisabled("Confirmed              : unavailable (no signal exposed)");

    ImGui::SeparatorText("Stream");
    ImGui::Text("Core idle              : %s", d.core_idle ? "yes" : "no");
    ImGui::Text("Paused for cache       : %s", d.paused_for_cache ? "yes" : "no");
    ImGui::Text("Demuxer cache          : %.1fs", d.cache_seconds);
    ImGui::Text("Buffer phase           : %s (target %.0fs)",
                d.buffer_phase == core::BufferPhase::Zap ? "zap" : "steady",
                core::buffer_phase_targets(d.buffer_phase).cache_seconds);
    ImGui::Text("Buffer phase command   : %s",
                player::to_string(d.buffer_phase_command_state));
    ImGui::Text("Buffer commands        : %d accepted / %d rejected",
                d.buffer_commands_accepted, d.buffer_commands_rejected);

    ImGui::Text("Tune-in time           : %.2fs", d.last_load_seconds);
    ImGui::Text("Health discontinuities : %d", d.health_discontinuities);
    ImGui::Text("mpv restart events     : %d", d.mpv_playback_restart_events);

    const auto supervisor_stats = core::project_supervisor_stats(
        supervisor_.current(), supervisor_clock_.now());
    ImGui::SeparatorText("Playback health");
    ImGui::Text("Verdict                : %s", core::to_string(health_snapshot_.verdict));
    ImGui::Text("Degraded reason        : %s",
                health_snapshot_.degraded_reason
                    ? core::to_string(*health_snapshot_.degraded_reason) : "-");
    ImGui::Text("Progressing            : %s",
                !health_snapshot_.progressing ? "unknown"
                    : (*health_snapshot_.progressing ? "yes" : "no"));
    ImGui::Text("Input advancing        : %s",
                !health_snapshot_.input_advancing ? "unknown"
                    : (*health_snapshot_.input_advancing ? "yes" : "no"));

    ImGui::SeparatorText("Supervisor");
    ImGui::Text("State                  : %s", core::to_string(supervisor_stats.state));
    ImGui::Text("Transport              : %s",
                supervisor_stats.transport ? core::to_string(*supervisor_stats.transport) : "-");
    ImGui::Text("Attempt                : %zu / %zu", supervisor_stats.attempt,
                supervisor_stats.attempt_ceiling);
    ImGui::Text("Reason                 : %s",
                supervisor_stats.reason ? supervisor_stats.reason->c_str() : "-");
    ImGui::Text("Recovery budget        : %s",
                supervisor_stats.elapsed_budget
                    ? std::format("{:.0f}ms", supervisor_stats.elapsed_budget->count() * 1000.0).c_str()
                    : "-");
    ImGui::Text("Policy                 : %s", supervisor_stats.policy_version.data());

    ImGui::SeparatorText("Live sync");
    ImGui::Text("Target offset          : %.1fs", d.live_target_seconds);
    ImGui::Text("Playback speed         : %.3fx", d.playback_speed);
    ImGui::Text("Rebuffers this channel : %d", d.rebuffer_count);
    ImGui::TextDisabled("Offset is estimated from buffer depth (no manifest)");

    if (ImGui::CollapsingHeader("Log")) {
        ImGui::BeginChild("log-scroll", ImVec2(720.0f, 260.0f));
        for (const auto& line : log::recent()) {
            ImGui::TextUnformatted(line.c_str());
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) {
            ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
    }

    ImGui::End();
}

void App::draw_frame() {
    finish_update_check();

    ui_.begin_frame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsKeyPressed(ImGuiKey_Tab, false) && !io.WantTextInput) {
        show_browser_ = !show_browser_;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) {
        show_diagnostics_ = !show_diagnostics_;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false) && !io.WantTextInput &&
        stage_ == Stage::Browsing) {
        paused_ = !paused_;
        player_.set_paused(paused_);
    }

    switch (stage_) {
        case Stage::Login:
        case Stage::Connecting:
            draw_login();
            break;
        case Stage::Browsing:
            draw_browser();
            draw_status_bar();
            break;
    }
    draw_update_banner();
    draw_diagnostics();

    ImGui::Render();
    ui_.end_frame();
}

int App::run() {
    std::string error;
    if (!initialize(error)) {
        log::error("Startup failed: {}", error);
        MessageBoxA(nullptr, error.c_str(), "Coax could not start", MB_ICONERROR | MB_OK);
        shutdown();
        return 1;
    }

    while (window_.pump_messages()) {
        player_.pump();
        process_player_events();
        supervisor_.poll();
        sample_playback_health();

        // Video dimensions arrive after the load completes, and can change
        // mid-stream when the provider switches encoder profile. Either way
        // the super-resolution scale factor is stale until recomputed.
        //
        // Zero dimensions are ignored rather than treated as a change: mpv
        // reports them transiently while reconfiguring, and reacting would
        // set the filter again, forcing another reconfiguration in a loop
        // that never lets the demuxer cache refill.
        const auto& diagnostics = player_.diagnostics();
        if (diagnostics.video_width > 0 && diagnostics.video_height > 0 &&
            (diagnostics.video_width != last_video_width_ ||
             diagnostics.video_height != last_video_height_)) {
            last_video_width_  = diagnostics.video_width;
            last_video_height_ = diagnostics.video_height;
            apply_vsr();
        }

        update_live_sync();
        finish_connect();
        draw_frame();
    }

    shutdown();
    return 0;
}

void App::shutdown() {
    if (connect_thread_.joinable()) {
        connect_thread_.join();
    }
    // The check is bounded by the WinHTTP timeouts, so this cannot hang
    // shutdown indefinitely.
    if (update_thread_.joinable()) {
        update_thread_.join();
    }
    supervisor_.dispose();
    player_.stop(generation_);
    composition_.set_video_content(nullptr);
    ui_.destroy();
    ImGui_ImplWin32_Shutdown();
    window_.destroy();
    log::info("Shutdown complete");
}

}  // namespace coax::app
