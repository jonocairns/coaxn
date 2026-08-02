#include "app/app.hpp"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <format>
#include <string_view>

#include "app/theme.hpp"
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

    // Before anything is measured or drawn: every size in the style, and every
    // hand-written one through theme::scaled, depends on it.
    theme::set_dpi_scale(window_.dpi_scale());

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
        video_attached_ = swapchain != nullptr;
    });

    window_.on_resize([this](int width, int height) { handle_resize(width, height); });
    // Registered only now that everything they touch exists: both fire from
    // the window procedure, which runs as soon as the window is created.
    window_.on_paint([this] { draw_frame(); });
    window_.on_dpi_changed([this](float scale) { theme::set_dpi_scale(scale); });

    if (!direct_media_.empty()) {
        stage_                = Stage::Browsing;
        playing_channel_name_ = "Direct media";
        set_status("Direct media");
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
    set_status("Saved portal loaded");
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
        set_status("Portal URL, username and password are all required", true);
        return;
    }

    credentials_ = credentials;
    stage_       = Stage::Connecting;
    set_status("Connecting...");
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
        set_status(error, true);
        log::error("Connect failed: {}", error);
        return;
    }

    client_ = std::make_unique<xtream::Client>(credentials_);
    channels_.reset(std::move(catalog.categories), std::move(catalog.channels));
    stage_  = Stage::Browsing;
    set_status(std::format("{} channels", channels_.channel_count()));
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
    set_status(std::format("Playing {}", channel.name));
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
        set_status(state.failure
                       ? std::format("Playback failed: {}", core::to_string(*state.failure))
                       : "Playback failed",
                   true);
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
    const float card_width = std::min(viewport->WorkSize.x - theme::scaled(48.0f),
                                      theme::scaled(430.0f));
    const float padding    = theme::scaled(26.0f);

    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(card_width, 0.0f), ImGuiCond_Always);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padding, padding));
    ImGui::Begin("##login", nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();

    // Mark and wordmark share a line: the mark is sized from the title's own
    // height, so the two stay aligned whatever the scale is.
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.9f);
    const float  mark   = ImGui::GetFontSize();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    theme::draw_logo(ImGui::GetWindowDrawList(),
                     ImVec2(origin.x + mark * 0.5f, origin.y + mark * 0.5f), mark * 0.46f);
    ImGui::Dummy(ImVec2(mark, mark));
    ImGui::SameLine();
    ImGui::TextUnformatted("Coax");
    ImGui::PopFont();

    ImGui::PushStyleColor(ImGuiCol_Text, theme::kTextDim);
    ImGui::TextWrapped(
        "Paste a full Xtream portal link (player_api.php or get.php) to fill the "
        "username and password in automatically, or enter them separately.");
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0.0f, theme::scaled(6.0f)));

    // Labels sit above their fields rather than beside them: full-width inputs
    // leave room for the long URLs providers hand out, and the three rows read
    // as one column instead of two ragged ones.
    bool submit = false;
    auto field  = [&](const char* label, const char* hint, std::string& value,
                     ImGuiInputTextFlags flags) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::kTextDim);
        ImGui::TextUnformatted(label);
        ImGui::PopStyleColor();

        TextField buffer(value);
        ImGui::PushID(label);
        ImGui::SetNextItemWidth(-FLT_MIN);
        // Enter submits, so the return value cannot double as the edited flag;
        // the copy back is unconditional and is a no-op when nothing changed.
        submit |= ImGui::InputTextWithHint("##field", hint, buffer.buffer, sizeof(buffer.buffer),
                                           flags | ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::PopID();
        value = buffer.buffer;
    };

    field("Portal URL", "https://provider.example.com:8080", portal_url_, 0);
    field("Username", "", username_, 0);
    field("Password", "", password_, ImGuiInputTextFlags_Password);

    ImGui::Dummy(ImVec2(0.0f, theme::scaled(6.0f)));

    const bool connecting = stage_ == Stage::Connecting;
    ImGui::BeginDisabled(connecting);
    ImGui::PushStyleColor(ImGuiCol_Button, theme::kAccent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::kAccentHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::kAccentActive);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::kOnAccent);
    if (ImGui::Button(connecting ? "Connecting..." : "Connect",
                      ImVec2(-FLT_MIN, theme::scaled(38.0f)))) {
        submit = true;
    }
    ImGui::PopStyleColor(4);
    ImGui::EndDisabled();

    if (submit && !connecting) {
        begin_connect();
    }

    // An indeterminate sweep under the button. The catalog fetch has no
    // progress to report, so this says "still working" and claims nothing else.
    const ImVec2 bar_min = ImGui::GetItemRectMin();
    const ImVec2 bar_max = ImGui::GetItemRectMax();
    if (connecting) {
        const float width = bar_max.x - bar_min.x;
        const float span  = width * 0.3f;
        const float phase = static_cast<float>(std::fmod(ImGui::GetTime(), 1.4) / 1.4);
        const float left  = bar_min.x + (width + span) * phase - span;
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(std::max(left, bar_min.x), bar_max.y - theme::scaled(3.0f)),
            ImVec2(std::min(left + span, bar_max.x), bar_max.y),
            theme::kOnAccent);
    }

    if (!status_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, status_error_ ? theme::kError : theme::kTextDim);
        ImGui::TextWrapped("%s", status_.c_str());
        ImGui::PopStyleColor();
    }

    // Secondary and destructive, so it is a ghost button rather than a peer of
    // Connect: same weight for both would make forgetting the portal as easy
    // to hit as using it.
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, theme::kTextDim);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    // No horizontal frame padding, so the label starts on the same column as
    // the labels and the status line above it rather than a few pixels in.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(0.0f, ImGui::GetStyle().FramePadding.y));
    if (ImGui::Button("Forget saved portal")) {
        win::CredentialStore::clear();
        portal_url_.clear();
        username_.clear();
        password_.clear();
        set_status("Saved portal cleared");
    }
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);

    ImGui::SameLine();
    const char* version = "v" COAX_VERSION;
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - padding - ImGui::CalcTextSize(version).x);
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::kTextDim);
    ImGui::TextUnformatted(version);
    ImGui::PopStyleColor();

    ImGui::End();
}

float App::browser_width() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    return std::min(viewport->WorkSize.x * 0.34f, theme::scaled(430.0f));
}

void App::draw_browser() {
    if (!show_browser_) {
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float          width    = browser_width();

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

    // Every group holding a match is opened while a search is running, and the
    // frame the search clears closes them again. Outside those two cases the
    // open state is left alone. Forcing it unconditionally — which is what
    // this did — re-closed a header on the frame after the click that opened
    // it, so the unfiltered list could not be expanded at all.
    const bool searching   = !search_.empty();
    const bool force_state = searching || search_was_active_;
    search_was_active_     = searching;

    for (const auto& group : groups) {
        const char* label = group.category ? group.category->name.c_str() : "Uncategorised";

        if (force_state) {
            ImGui::SetNextItemOpen(searching, ImGuiCond_Always);
        }
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
    // Idle time before the overlay starts to go, and how long it takes to
    // cross. Long enough that reaching for the volume does not race it.
    constexpr double kIdleSeconds = 2.5;
    constexpr float  kFadeSeconds = 0.22f;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float          height   = ImGui::GetFrameHeightWithSpacing() * 1.4f;
    const float          top      = viewport->WorkPos.y + viewport->WorkSize.y - height;

    // The channel list owns its column for its whole height, so the playback
    // overlay begins where that ends. Drawn full width the two overlapped, and
    // the status text was printed across the bottom of the list.
    const float left  = viewport->WorkPos.x + (show_browser_ ? browser_width() : 0.0f);
    const float width = viewport->WorkPos.x + viewport->WorkSize.x - left;

    // Held open while the pointer is over the bar or a control is being
    // dragged, so the volume slider cannot fade out from under the hand
    // holding it. Held open too when there is no video: over the backdrop it
    // hides nothing, and a channel has yet to be chosen.
    const ImGuiIO& io   = ImGui::GetIO();
    const bool     hold = !video_attached_ || ImGui::IsAnyItemActive() ||
                          (io.MousePos.y >= top && io.MousePos.x >= left);
    const bool     want = hold || (ImGui::GetTime() - last_pointer_activity_) < kIdleSeconds;

    const float step = io.DeltaTime / kFadeSeconds;
    status_bar_fade_ = std::clamp(status_bar_fade_ + (want ? step : -step), 0.0f, 1.0f);
    if (status_bar_fade_ <= 0.0f) {
        return;
    }

    ImGui::SetNextWindowPos(ImVec2(left, top));
    ImGui::SetNextWindowSize(ImVec2(width, height));
    // The background alpha is set rather than multiplied by the style, so the
    // fade has to be folded in here as well as into the contents.
    ImGui::SetNextWindowBgAlpha(0.65f * status_bar_fade_);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, status_bar_fade_);

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

    // The controls are right-aligned by measuring them rather than by a
    // hand-tuned offset, which is what previously pinned this row to one font
    // at one scale. The toggle is sized to the wider of its two labels so
    // pausing does not shuffle everything beside it.
    const ImGuiStyle& style        = ImGui::GetStyle();
    const float       slider_width = theme::scaled(96.0f);
    const float       toggle_width = std::max(ImGui::CalcTextSize("Play").x,
                                              ImGui::CalcTextSize("Pause").x) +
                                     style.FramePadding.x * 2.0f;
    const float       controls =
        ImGui::GetFrameHeight() + style.ItemInnerSpacing.x +
        ImGui::CalcTextSize("Super resolution").x + style.ItemSpacing.x +
        toggle_width + style.ItemSpacing.x + slider_width;

    ImGui::SameLine(ImGui::GetWindowWidth() - style.WindowPadding.x - controls);
    if (ImGui::Checkbox("Super resolution", &vsr_enabled_)) {
        apply_vsr();
    }
    ImGui::SameLine();
    if (ImGui::Button(paused_ ? "Play" : "Pause", ImVec2(toggle_width, 0.0f))) {
        paused_ = !paused_;
        player_.set_paused(paused_);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(slider_width);
    if (ImGui::SliderInt("##volume", &volume_, 0, 130, "vol %d")) {
        player_.set_volume(volume_);
    }

    ImGui::End();
    ImGui::PopStyleVar();
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
        ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - theme::scaled(16.0f),
               viewport->WorkPos.y + theme::scaled(16.0f)),
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

    // Denser rows than the rest of the application: this is a wall of
    // readings, and the default row spacing turns it into a page of scrolling.
    // Spacing only — the panel is drawn at the same scale as everything else,
    // because a surface that sizes itself is how a scale system comes apart.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(ImGui::GetStyle().ItemSpacing.x, theme::scaled(3.0f)));
    // The gutter between the two columns of sections.
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(theme::scaled(14.0f), 0.0f));

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - theme::scaled(20.0f),
               viewport->WorkPos.y + theme::scaled(20.0f)),
        ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    // Nearly opaque. It reads over whatever the video happens to be showing,
    // and it is only on screen while somebody is deliberately looking at it.
    ImGui::SetNextWindowBgAlpha(0.94f);

    ImGui::Begin("Diagnostics", &show_diagnostics_,
                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing);

    const auto& d = player_.diagnostics();

    // The labels used to be padded out to a fixed character count, which lines
    // the colons up in a monospace font and does not in a proportional one.
    // The value column is measured from the longest labels below instead, so
    // it holds whatever the font is. Equal character counts are not equal
    // widths here, hence measuring all three rather than picking one.
    // Aligned by padding each label out to the widest one, rather than by an
    // absolute column. SameLine's offset form measures from an origin that
    // moves — window origin in a plain window, cell origin inside a table,
    // neither including the same padding — and getting it wrong stays
    // invisible until some label happens to be long enough to collide. The
    // spacing form is measured from the end of the label, so it cannot.
    // Equal character counts are not equal widths, hence measuring all three.
    const float label_column = std::max({ImGui::CalcTextSize("Composition swap chain").x,
                                         ImGui::CalcTextSize("Hardware decode wanted").x,
                                         ImGui::CalcTextSize("Rebuffers this channel").x});
    const float column_gap   = theme::scaled(18.0f);

    // Label dim, value bright: the reading is what is being looked for, and
    // the two columns separate without a rule between them.
    auto field = [label_column, column_gap](const char* label, std::string_view value,
                                            bool dim = false) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::kTextDim);
        ImGui::TextUnformatted(label);
        ImGui::PopStyleColor();
        // A label wider than the measured maximum keeps the gap and loses the
        // alignment, which is the harmless way round.
        ImGui::SameLine(0.0f, std::max(label_column - ImGui::CalcTextSize(label).x, 0.0f) +
                                  column_gap);
        if (dim) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::kTextDim);
        }
        ImGui::TextUnformatted(value.data(), value.data() + value.size());
        if (dim) {
            ImGui::PopStyleColor();
        }
    };

    // Two columns of whole sections. Stacked, the readings run to about a
    // third more than the window is tall and the panel scrolls; the sections
    // are self-contained, so splitting them costs nothing.
    if (!ImGui::BeginTable("##readings", 2, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::End();
        ImGui::PopStyleVar(2);
        return;
    }
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);

    ImGui::SeparatorText("Presentation");
    field("Composition swap chain", d.swapchain_state);
    field("Window", std::format("{}x{}", window_.width(), window_.height()));

    ImGui::SeparatorText("Decode");
    field("Codec", d.video_codec.empty() ? "-" : d.video_codec);
    field("Source", std::format("{}x{}", d.video_width, d.video_height));
    field("Hardware decode wanted", d.hwdec_requested);
    field("Hardware decode active", d.hwdec_active.empty() ? "-" : d.hwdec_active);

    ImGui::SeparatorText("Super resolution");
    // Requested, attached and confirmed are distinct on purpose. There is no
    // reliable signal that the driver actually ran RTX VSR on a frame, so this
    // never claims it did.
    field("Requested", d.vsr_requested ? "yes" : "no");
    field("Filter attached", d.vsr_filter_attached ? "yes" : "no");
    field("Confirmed", "unavailable (no signal exposed)", true);

    ImGui::SeparatorText("Stream");
    field("Core idle", d.core_idle ? "yes" : "no");
    field("Paused for cache", d.paused_for_cache ? "yes" : "no");
    field("Demuxer cache", std::format("{:.1f}s", d.cache_seconds));
    field("Buffer phase",
          std::format("{} (target {:.0f}s)",
                      d.buffer_phase == core::BufferPhase::Zap ? "zap" : "steady",
                      core::buffer_phase_targets(d.buffer_phase).cache_seconds));
    field("Buffer phase command", player::to_string(d.buffer_phase_command_state));
    field("Buffer commands", std::format("{} accepted / {} rejected",
                                         d.buffer_commands_accepted, d.buffer_commands_rejected));
    field("Tune-in time", std::format("{:.2f}s", d.last_load_seconds));
    field("Health discontinuities", std::format("{}", d.health_discontinuities));
    field("mpv restart events", std::format("{}", d.mpv_playback_restart_events));

    ImGui::TableSetColumnIndex(1);

    const auto supervisor_stats = core::project_supervisor_stats(
        supervisor_.current(), supervisor_clock_.now());
    ImGui::SeparatorText("Playback health");
    field("Verdict", core::to_string(health_snapshot_.verdict));
    field("Degraded reason", health_snapshot_.degraded_reason
                                 ? core::to_string(*health_snapshot_.degraded_reason) : "-");
    field("Progressing", !health_snapshot_.progressing ? "unknown"
                             : (*health_snapshot_.progressing ? "yes" : "no"));
    field("Input advancing", !health_snapshot_.input_advancing ? "unknown"
                                 : (*health_snapshot_.input_advancing ? "yes" : "no"));

    ImGui::SeparatorText("Supervisor");
    field("State", core::to_string(supervisor_stats.state));
    field("Transport",
          supervisor_stats.transport ? core::to_string(*supervisor_stats.transport) : "-");
    field("Attempt", std::format("{} / {}", supervisor_stats.attempt,
                                 supervisor_stats.attempt_ceiling));
    field("Reason", supervisor_stats.reason ? std::string_view(*supervisor_stats.reason) : "-");
    field("Recovery budget",
          supervisor_stats.elapsed_budget
              ? std::format("{:.0f}ms", supervisor_stats.elapsed_budget->count() * 1000.0)
              : std::string("-"));
    field("Policy", supervisor_stats.policy_version);

    ImGui::SeparatorText("Live sync");
    field("Target offset", std::format("{:.1f}s", d.live_target_seconds));
    field("Playback speed", std::format("{:.3f}x", d.playback_speed));
    field("Rebuffers this channel", std::format("{}", d.rebuffer_count));
    ImGui::PushStyleColor(ImGuiCol_Text, theme::kTextDim);
    ImGui::TextUnformatted("Offset is estimated from buffer depth (no manifest)");
    ImGui::PopStyleColor();

    ImGui::EndTable();

    // Full width, under both columns: log lines are long and splitting them
    // into a column would wrap every one of them.
    if (ImGui::CollapsingHeader("Log")) {
        ImGui::BeginChild("log-scroll",
                          ImVec2(theme::scaled(560.0f), theme::scaled(200.0f)));
        for (const auto& line : log::recent()) {
            ImGui::TextUnformatted(line.c_str());
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) {
            ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
    }

    ImGui::End();
    ImGui::PopStyleVar(2);
}

void App::draw_frame() {
    finish_update_check();

    ui_.begin_frame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    ImGuiIO& io = ImGui::GetIO();

    // What keeps the playback overlay up. Movement rather than position: a
    // pointer parked over the video is somebody watching, not somebody using
    // the controls.
    if (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f || io.MouseWheel != 0.0f ||
        ImGui::IsAnyMouseDown()) {
        last_pointer_activity_ = ImGui::GetTime();
    }

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

    // Until mpv owns the video plane there is nothing behind the UI layer but
    // an empty composition visual, which the desktop shows through as white.
    // The backdrop is what the window is made of in every state before the
    // first frame arrives — logging in, connected but idle, and mid-load.
    if (!video_attached_) {
        theme::draw_backdrop();
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
