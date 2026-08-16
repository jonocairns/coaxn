#include "app/app.hpp"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <exception>
#include <format>
#include <string_view>
#include <utility>

#include "app/theme.hpp"
#include "app/widgets.hpp"
#include "util/log.hpp"
#include "win/credential_store.hpp"
#include "win/settings_store.hpp"

namespace coax::app {
namespace {

constexpr int kInitialWidth  = 1600;
constexpr int kInitialHeight = 900;

// Dimensions belonging to one surface each. These are not spacing and so are
// not on the scale: a login card is as wide as a provider's URL needs, and
// rounding that to the nearest four would say nothing.
constexpr float kLoginCardWidth   = 430.0f;
constexpr float kBrowserMaxWidth  = 430.0f;
constexpr float kConnectHeight    = 40.0f;
constexpr float kLoadingMaxWidth  = 360.0f;
constexpr float kOverlayRampExtra = 46.0f;
constexpr float kVolumeWidth      = 124.0f;
constexpr float kLogWidth         = 560.0f;
constexpr float kLogHeight        = 200.0f;

// The strip along the top edge that stands in for a title bar when the window
// draws its own frame. On the spacing scale rather than measured from a
// caption, because nothing is drawn in it: it is a margin the interface leaves
// clear so the window has somewhere to be dragged by.
constexpr float kCaptionHeight = theme::kSpace7;

// The right-click menu. Named rather than inlined because it is opened from one
// place and drawn in another.
constexpr const char* kWindowMenu = "##window-menu";

// Where the volume track marks unity. The ceiling above it is core::kMaxVolume,
// which is also the range the settings file is validated against — one number,
// one home. This one is only ever a tick on a slider, so it stays here.
constexpr int kUnityVolume = 100;

// Idle time before the overlays start to go, and how long they take to cross.
// Long enough that reaching for the volume does not race it. Shared by the
// playback bar and the title strip: they are two edges of one frame, and an
// application whose chrome arrived and left at two different speeds would read
// as two overlays that happen to be on screen together.
constexpr double kIdleSeconds = 2.5;
constexpr float  kFadeSeconds = 0.22f;

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

// The longest prefix of `text` that fits in `width`, with an ellipsis where it
// had to be cut. ImGui elides its own labels, but only for widgets that own
// their text; a row drawn by hand has to do it itself, and a name allowed to
// run into the edge of the panel reads as a rendering fault rather than as a
// long name.
std::string elide(const std::string& text, float width) {
    const float full = ImGui::CalcTextSize(text.c_str()).x;
    if (full <= width || text.empty()) {
        return text;
    }

    static constexpr std::string_view kEllipsis = "\xe2\x80\xa6";  // U+2026
    const float room =
        width - ImGui::CalcTextSize(kEllipsis.data(), kEllipsis.data() + kEllipsis.size()).x;
    if (room <= 0.0f) {
        return std::string(kEllipsis);
    }

    // A byte is a cut point unless it is a UTF-8 continuation, all of which
    // match 10xxxxxx. Handing ImGui half a code point draws a replacement
    // glyph, so every candidate below lands on a boundary.
    const auto boundary = [&](std::size_t at) {
        return at == 0 || at >= text.size() ||
               (static_cast<unsigned char>(text[at]) & 0xC0) != 0x80;
    };
    const auto fits = [&](std::size_t at) {
        return ImGui::CalcTextSize(text.c_str(), text.c_str() + at).x <= room;
    };

    // Proportional first guess, then step to the edge of the fit. Measuring
    // one prefix per character would be a loop over the glyphs of a loop over
    // the characters, for every overlong name on screen; a name that overflows
    // is usually close, so this settles in a step or two.
    std::size_t cut = std::min<std::size_t>(
        text.size(), static_cast<std::size_t>(static_cast<float>(text.size()) * room / full) + 1);
    while (cut > 0 && !boundary(cut)) {
        --cut;
    }

    if (fits(cut)) {
        for (std::size_t next = cut + 1; next <= text.size(); ++next) {
            if (!boundary(next)) continue;
            if (!fits(next)) break;
            cut = next;
        }
    } else {
        while (cut > 0) {
            do {
                --cut;
            } while (cut > 0 && !boundary(cut));
            if (fits(cut)) break;
        }
    }

    return text.substr(0, cut) + std::string(kEllipsis);
}

std::string signed_seconds(std::optional<double> value) {
    return value ? std::format("{:+.3f}s", *value) : "unavailable";
}

const char* optional_pause(std::optional<bool> value) {
    return !value ? "unavailable" : (*value ? "yes" : "no");
}

const char* control_baseline(std::optional<bool> retained) {
    return !retained ? "unavailable" : (*retained ? "retained" : "adjacent");
}

void log_recovery_exception(const core::SupervisorEffect& effect,
                            const std::exception_ptr& failure) {
    try {
        if (failure) std::rethrow_exception(failure);
    } catch (const std::exception& error) {
        log::warn("Recovery effect {} threw: {}",
                  core::effect_name(effect.payload), error.what());
        return;
    } catch (...) {
        log::warn("Recovery effect {} threw an unknown exception",
                  core::effect_name(effect.payload));
        return;
    }
    log::warn("Recovery effect {} failed without exception details",
              core::effect_name(effect.payload));
}

}  // namespace

App::App()
    : playback_session_(
          supervisor_clock_,
          {.active_load = [this]() -> std::optional<player::ActiveLoad> {
               const auto& target = player_.current_target();
               if (!target) return std::nullopt;
               return player::ActiveLoad{target->generation, target->load_attempt};
           },
           .diagnostics = [this]() -> const player::Diagnostics& {
               return player_.diagnostics();
           },
           .health_observation = [this] { return player_.health_observation(); },
           .execute_recovery = [this](const core::SupervisorEffect& effect) {
               return execute_supervisor_effect(effect);
           },
           .on_recovery_exception = [](const core::SupervisorEffect& effect,
                                       std::exception_ptr failure) {
               log_recovery_exception(effect, failure);
           },
           .restore_backend_settings = [this] {
               player_.set_volume(volume_);
               if (playback_control_capability_ ==
                   player::PlaybackControlCapability::ResumeFromPosition) {
                   player_.set_paused(player::position_preserving_pause_requested(
                       playback_control_capability_, playback_intent_));
               }
               apply_vsr();
           },
           .apply_buffer_phase = [this](core::Generation generation,
                                        core::BufferPhase phase) {
               player_.apply_buffer_phase(generation, phase);
           },
           .set_speed = [this](double speed) { player_.set_speed(speed); },
           .set_live_sync_state = [this](double target, int rebuffers) {
               player_.set_live_sync_state(target, rebuffers);
           },
           .set_health_discontinuities = [this](int count) {
               player_.set_health_discontinuities(count);
           },
           .on_player_event = [this](const player::PlayerEvent& event) {
               observe_player_event(event);
           },
           .on_state_changed = [this](const core::SupervisorState& state,
                                      core::SupervisorStateName previous) {
               on_supervisor_state_changed(state, previous);
           },
           .on_transition = [this](const core::SupervisorTransition& transition) {
               log::info("Supervisor {} -> {} generation {} load-attempt {} intent {} "
                         "attempt {} reason {} command-admission-elapsed {:.0f}ms",
                         core::to_string(transition.from), core::to_string(transition.to),
                         transition.generation.value(), transition.load_attempt.value(),
                         core::to_string(transition.load_intent), transition.attempt,
                         transition.reason,
                         transition.elapsed_budget.count() * 1000.0);
               if (transition.outcome != core::RecoveryOutcome::None) {
                   const auto& diagnostics = player_.diagnostics();
                   player::RecoveryDecisionEvidence evidence;
                   evidence.cache_paused = diagnostics.paused_for_cache;
                   evidence.playback_movement_seconds =
                       playback_session_.health_snapshot().timeline.playback_movement_seconds;
                   evidence.cache_end_movement_seconds =
                       playback_session_.health_snapshot().timeline.cache_end_movement_seconds;
                   evidence.engine_warning = diagnostics.last_engine_message;
                   log::info("{}", player::format_recovery_telemetry(
                       transition, diagnostics.request_shape, evidence));
               }
           },
           .on_health_sample = [this](const player::HealthSampleReport& report) {
               log_health_sample(report);
           },
           .on_rebuffer = [](int count, double target) {
               log::info("Rebuffer #{}; live target now {:.1f}s", count, target);
           },
           .on_unity_speed = [](double speed) {
               log::warn("Live-sync telemetry invalid; holding playback at {:.2f}x", speed);
           }}) {}

App::~App() {
    if (connect_thread_.joinable()) {
        connect_thread_.join();
    }
}

bool App::initialize(std::string& error) {
    // Before the window, not after it: the frame is chosen at creation, and
    // applying it afterwards would open every session with a caption that then
    // vanishes. Defaults on any failure, so a missing or damaged file costs a
    // preference rather than a start-up.
    settings_ = win::SettingsStore::load();
    window_.set_minimal_frame(settings_.minimal_mode);

    volume_            = settings_.volume;
    volume_last_frame_ = volume_;
    // Unmuting has to land somewhere audible. A session restored at zero would
    // otherwise put the speaker back to zero and look broken.
    pre_mute_volume_ = volume_ > 0 ? volume_ : kUnityVolume;

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
    presentation_ready_ = true;

    player::PlayerConfig config;
    config.composition_width  = window_.width();
    config.composition_height = window_.height();
    if (!player_.initialize(config, error)) {
        return false;
    }

    // The engine starts at unity, so a restored volume has to be pushed to it.
    // restore_backend_settings replays this after a rebuild; nothing replays it
    // for the first one.
    player_.set_volume(volume_);

    // Attaching on the callback rather than polling: the property is
    // unavailable until mpv's video output exists, and mpv may replace the
    // swap chain later.
    player_.on_swapchain([this](void* swapchain) {
        const bool attached = composition_.set_video_content(
            static_cast<IUnknown*>(swapchain));
        // Only a content change the tree accepted counts. Otherwise the
        // backdrop stays drawn, which is the honest thing to show when the
        // video visual is holding nothing.
        video_attached_ = attached && swapchain != nullptr;
        return attached;
    });

    window_.on_resize([this](int width, int height) { handle_resize(width, height); });
    // Registered only now that everything they touch exists: all of these fire
    // from the window procedure, which runs as soon as the window is created.
    window_.on_paint([this] { draw_frame(); });
    window_.on_dpi_changed([this](float scale) { theme::set_dpi_scale(scale); });
    window_.on_display_change([this] { handle_display_change(); });
    window_.on_minimized_changed([this](bool) { update_playback_power(); });
    window_.on_resume([this] { handle_resume(); });

    if (!direct_media_.empty()) {
        stage_ = Stage::Browsing;
        play_direct_media();
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

void App::handle_display_change() {
    // Size and scale first, because both can have moved: the window may now be
    // on a different monitor at a different DPI without any of the messages
    // that usually announce that having been sent.
    theme::set_dpi_scale(window_.dpi_scale());
    handle_resize(window_.width(), window_.height());
    // The tree is re-committed even when nothing above changed anything.
    // DirectComposition binds its target to a display topology, and a
    // reconfiguration can leave the previous commit describing one that no
    // longer exists.
    composition_.commit();
    // A monitor change can be the visible half of an adapter reset. Asking is
    // cheap; discovering it on the next present is a lost frame either way.
    ui_.verify_device();
}

void App::handle_resume() {
    // Suspend can reset the adapter, and nothing reports that until work is
    // submitted, so the device is treated as suspect until it says otherwise.
    // A failed check latches a loss the frame loop then acts on; it does not
    // rebuild from inside the window procedure.
    if (!ui_.verify_device()) {
        log::warn("Display device did not survive suspend");
    }
}

void App::service_presentation() {
    if (auto loss = ui_.take_device_loss()) {
        last_device_loss_ = std::move(loss->detail);
        ++device_loss_events_;
        presentation_budget_.request(supervisor_clock_.now());
    }

    // The other way the surface can become unusable: a resize recreates the
    // swap chain's buffers and then fails to build a render target over them,
    // for a reason that is not a device loss and so latches nothing. Rebuilding
    // is the same remedy, and the budget bounds it the same way. Guarded so it
    // reports once per episode rather than once per frame — after this the
    // surface is either back or presentation_ready_ is false.
    if (presentation_ready_ && !ui_.has_render_target() &&
        !presentation_budget_.outstanding() && !presentation_budget_.exhausted()) {
        log::warn("UI render target missing with a live device; rebuilding presentation");
        presentation_budget_.request(supervisor_clock_.now());
    }

    switch (presentation_budget_.poll(supervisor_clock_.now())) {
        case core::RebuildDecision::Hold:
            return;
        case core::RebuildDecision::Exhausted:
            set_status("Display device lost and could not be rebuilt", true);
            // Terminal, and said so. The budget is spent, no further attempt is
            // coming, and presentation_phase() reports Failed from here on —
            // which is what lets the frame loop wait for messages instead of
            // polling for a rebuild that will never be scheduled again.
            log::error("Presentation rebuild abandoned after {} attempts; last loss: {}. "
                       "The frame loop is now idle; playback recovery continues.",
                       presentation_budget_.attempts(),
                       last_device_loss_.empty() ? "none reported" : last_device_loss_);
            return;
        case core::RebuildDecision::Attempt:
            break;
    }

    if (!rebuild_presentation()) {
        presentation_budget_.failed(supervisor_clock_.now());
        return;
    }
    presentation_budget_.succeeded();

    // The surface is back, but mpv still holds a device that no longer exists.
    // Recreating it and resuming the channel is playback recovery, so it goes
    // to the supervisor: that is what bounds the attempts and what stops a
    // superseded channel from being resurrected by a rebuild that started
    // before the newer one was requested.
    playback_session_.presentation_lost();
}

bool App::rebuild_presentation() {
    log::warn("Rebuilding presentation, attempt {} of {}",
              presentation_budget_.attempts(), presentation_budget_.attempt_ceiling());

    // Outwards from the content. mpv's swap chain leaves the visual before the
    // tree holding it goes, and the tree leaves the device before the device
    // does. The player moves to a new epoch as it detaches, so the address it
    // just released cannot be mistaken for whatever the rebuilt mpv reports.
    // Nothing may draw from here until the far end of this function. Tearing
    // the UI layer down shuts the ImGui D3D11 backend down with it, and a
    // frame drawn against that backend does not fail — it dereferences a null
    // pointer. A rebuild that fails partway is the ordinary case, not an
    // exotic one: an adapter that is mid-reset refuses device creation, which
    // is exactly why the attempt is retried.
    presentation_ready_ = false;
    player_.detach_swapchain();
    video_attached_ = false;
    composition_.destroy();
    ui_.destroy();

    std::string error;
    if (!ui_.create(window_.width(), window_.height(), error)) {
        log::error("UI layer rebuild failed: {}", error);
        return false;
    }
    if (!composition_.create(window_.handle(), ui_.dxgi_device(), error)) {
        log::error("Composition tree rebuild failed: {}", error);
        return false;
    }
    composition_.set_ui_content(ui_.swapchain());

    presentation_ready_ = true;
    ++presentation_rebuilds_;
    log::info("Presentation rebuilt ({}x{})", window_.width(), window_.height());
    return true;
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
    // Redacted even though the field is now only ever a base URL: the session
    // log sits next to the executable in plain text, and nothing stops someone
    // pasting a link with credentials in its query into a box that asks for a
    // URL. Masking a string that turns out to have had no secret in it costs
    // nothing; the other way round cannot be undone.
    log::info("Restored saved portal");

    // Straight through to the catalogue. Someone whose credentials are already
    // on this machine has said what they want to connect to, and a login screen
    // whose only remaining action is to press a button is a screen that exists
    // to be dismissed.
    //
    // Only when all three fields survived. A partial record would reach
    // begin_connect and come back as an error banner over an empty form, which
    // is a worse first frame than the form on its own.
    if (!portal_url_.empty() && !username_.empty() && !password_.empty()) {
        begin_connect();
    }
}

void App::save_portal() const {
    win::CredentialStore::save(std::format("{}\n{}\n{}", portal_url_, username_, password_));
}

void App::begin_connect() {
    if (stage_ == Stage::Connecting) {
        return;
    }

    xtream::Credentials credentials{
        .base_url = portal_url_,
        .username = username_,
        .password = password_,
    };

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
        // The UI may present the actionable provider error, but the persistent
        // session log must not inherit a WinHTTP error containing the origin.
        log::error("Provider connection failed");
        return;
    }

    client_ = std::make_unique<xtream::Client>(credentials_);
    channels_.reset(std::move(catalog.categories), std::move(catalog.channels));
    const auto provider_session = target_registry_.begin_provider_session();
    log::info("Provider session {} connected", provider_session);
    stage_  = Stage::Browsing;
    set_status(std::format("{} channels", channels_.channel_count()));
    save_portal();
}

void App::play(const core::Channel& channel) {
    play(channel, target_registry_.identify_channel(channel.id));
}

void App::play(const core::Channel& channel, player::SourceCorrelation correlation) {
    if (!client_) {
        return;
    }
    direct_media_active_ = false;
    playing_channel_id_   = channel.id;
    playing_channel_name_ = channel.name;
    playback_control_capability_ =
        player::PlaybackControlCapability::RestartAtLiveEdge;
    playback_intent_ = player::PlaybackIntent::Running;

    // Latency learned on one channel says nothing about the next.
    const auto generation = playback_session_.begin_channel();
    loading_channel_generation_ = generation;
    set_status(std::format("Loading {}", channel.name));
    log::info("Channel selected generation {} provider-session={} channel-session={}",
              generation.value(), correlation.provider_session,
              correlation.channel_session);
    const core::LoadAttempt load_attempt{1};
    // Xtream resolves this endpoint as a continuous .ts request; transport is
    // therefore resolved with the load rather than guessed from HTTP(S).
    if (player_.play(client_->stream_url(channel), generation, load_attempt,
                     core::RecoveryTransport::MpegTs, false, correlation)) {
        playback_session_.load_started(load_attempt, core::LoadIntent::FreshSelection,
                                       core::RecoveryTransport::MpegTs,
                                       player::TimelineRecoveryCapability::
                                           ContinuousRawMpegTs);
    } else {
        playback_session_.load_failed(load_attempt);
    }
    apply_vsr();
}

void App::play_direct_media() {
    if (direct_media_.empty()) return;
    direct_media_active_ = true;
    playing_channel_id_.clear();
    playing_channel_name_ = "Direct media";
    playback_control_capability_ =
        player::PlaybackControlCapability::RestartAtLiveEdge;
    playback_intent_ = player::PlaybackIntent::Running;

    const auto generation = playback_session_.begin_channel();
    loading_channel_generation_ = generation;
    set_status("Loading Direct media");
    const core::LoadAttempt load_attempt{1};
    if (player_.play(direct_media_, generation, load_attempt,
                     core::RecoveryTransport::MpegTs, false,
                     {.provider_session = 0,
                      .channel_session = generation.value()})) {
        playback_session_.load_started(load_attempt, core::LoadIntent::FreshSelection,
                                       core::RecoveryTransport::MpegTs);
    } else {
        playback_session_.load_failed(load_attempt);
    }
    apply_vsr();
}

void App::stop_playback() {
    if (playback_control_capability_ !=
            player::PlaybackControlCapability::RestartAtLiveEdge ||
        (playing_channel_id_.empty() && playing_channel_name_.empty()) ||
        (playback_intent_ == player::PlaybackIntent::StoppedByUser &&
         playback_session_.state().name == core::SupervisorStateName::Idle)) return;

    const auto generation = playback_session_.generation();
    if (!playback_session_.stop(generation)) return;
    playback_intent_ = player::PlaybackIntent::StoppedByUser;
    loading_channel_generation_.reset();
    player_.stop(generation);
    set_status("Stopped");
    update_playback_power();
}

void App::start_playback() {
    if (playback_control_capability_ !=
            player::PlaybackControlCapability::RestartAtLiveEdge ||
        playback_intent_ != player::PlaybackIntent::StoppedByUser) return;

    // Direct media is a presentation-path test source, not a retained catalog
    // channel. It still follows Stop/Start semantics when that mode is active.
    if (direct_media_active_) {
        play_direct_media();
        return;
    }

    const core::Channel* retained = channels_.find(playing_channel_id_);
    switch (player::prepare_live_start(
                playing_channel_id_, playback_intent_, retained != nullptr)) {
        case player::LiveStartDecision::NoSelection:
            return;
        case player::LiveStartDecision::RetainedChannelMissing:
            playing_channel_name_.clear();
            loading_channel_generation_.reset();
            set_status("Channel no longer available", true);
            update_playback_power();
            return;
        case player::LiveStartDecision::StartFresh:
            break;
    }

    // Resolution above deliberately precedes the Running intent and the fresh
    // generation/channel-session correlation minted for returning live.
    play(*retained, target_registry_.identify_fresh_channel(retained->id));
}

void App::toggle_playback() {
    if (playing_channel_id_.empty() && playing_channel_name_.empty()) return;
    if (playback_control_capability_ !=
        player::PlaybackControlCapability::RestartAtLiveEdge) return;
    if (playback_intent_ == player::PlaybackIntent::StoppedByUser) start_playback();
    else stop_playback();
}

void App::observe_player_event(const player::PlayerEvent& event) {
    std::visit(Overloaded{
        [&](const player::LoadCommandResult& result) {
            if (!result.accepted) {
                log::warn("Load command rejected for generation {} with structured error {}",
                          event.generation.value(), result.error);
            }
        },
        [&](const player::BackendFailed& failed) {
            log::warn("libmpv backend failed for generation {} with structured error {}",
                      event.generation.value(), failed.error);
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
        [](const auto&) {}}, event.payload);
}

void App::log_health_sample(const player::HealthSampleReport& report) {
    const auto& fold = report.fold;
    if (!fold.observation_accepted) {
        log::warn("Dropped stale health observation generation {} while generation {} is active",
                  report.observed_generation.value(), fold.state.generation.value());
        return;
    }
    const auto& health_snapshot = fold.state.snapshot;
    const auto evidence_generation = health_snapshot.timeline.generation;
    const double playback_speed = player_.diagnostics().playback_speed;
    const auto warning_component = report.engine_messages_since_sample > 0 &&
            report.engine_warning
        ? player::to_string(report.engine_warning->component) : "none";
    const auto warning_category = report.engine_messages_since_sample > 0 &&
            report.engine_warning
        ? player::to_string(report.engine_warning->category) : "none";
    const auto warning_severity = report.engine_messages_since_sample > 0 &&
            report.engine_warning
        ? player::to_string(report.engine_warning->severity) : "none";
    log::debug(
        "Timeline sample generation {} load-attempt={} kind={} elapsed={} playback-move={} "
        "playback-deviation={} cache-end-move={} control-playback-move={} "
        "control-playback-deviation={} control-baseline={} cache-paused={} playback-speed={:.2f}x "
        "previous-cache-paused={} engine-messages-since-sample={} "
        "unattributed-engine-messages-since-sample={} warning-severity={} "
        "warning-component={} warning-category={} timeline-recovery={} "
        "baseline-live-gap={} current-live-gap={} cache-relative-loss={} "
        "live-gap-increase={} rebuffer-age={} cache-resume-related={} "
        "supervisor-accepted={}",
        evidence_generation.value(), health_snapshot.timeline.load_attempt.value(),
        player::to_string(report.classification),
        signed_seconds(health_snapshot.timeline.elapsed_seconds),
        signed_seconds(health_snapshot.timeline.playback_movement_seconds),
        signed_seconds(health_snapshot.timeline.playback_deviation_seconds),
        signed_seconds(health_snapshot.timeline.cache_end_movement_seconds),
        signed_seconds(health_snapshot.timeline.control_playback_movement_seconds),
        signed_seconds(health_snapshot.timeline.control_playback_deviation_seconds),
        control_baseline(health_snapshot.timeline.control_baseline_retained),
        health_snapshot.timeline.cache_paused ? "yes" : "no",
        playback_speed,
        optional_pause(health_snapshot.timeline.previous_cache_paused),
        report.engine_messages_since_sample,
        report.unattributed_engine_messages_since_sample,
        warning_severity, warning_component, warning_category,
        player::to_string(report.timeline_recovery.outcome),
        signed_seconds(report.timeline_recovery.baseline_live_gap_seconds),
        signed_seconds(report.timeline_recovery.current_live_gap_seconds),
        signed_seconds(report.timeline_recovery.cache_relative_loss_seconds),
        signed_seconds(report.timeline_recovery.live_gap_increase_seconds),
        signed_seconds(report.timeline_recovery.rebuffer_age_seconds),
        report.timeline_recovery.cache_resume_related ? "yes" : "no",
        optional_pause(report.timeline_recovery.supervisor_accepted));
    if (fold.discontinuity) {
        log::warn(
            "Timeline discontinuity #{} generation {} load-attempt={} kind={} playback-move={} "
            "playback-deviation={} cache-end-move={} control-playback-move={} "
            "control-playback-deviation={} control-baseline={} "
            "engine-messages-since-sample={} "
            "unattributed-engine-messages-since-sample={} "
            "warning-severity={} warning-component={} warning-category={}",
            fold.state.discontinuities, evidence_generation.value(),
            fold.state.load_attempt.value(),
            player::to_string(report.classification),
            signed_seconds(health_snapshot.timeline.playback_movement_seconds),
            signed_seconds(health_snapshot.timeline.playback_deviation_seconds),
            signed_seconds(health_snapshot.timeline.cache_end_movement_seconds),
            signed_seconds(health_snapshot.timeline.control_playback_movement_seconds),
            signed_seconds(health_snapshot.timeline.control_playback_deviation_seconds),
            control_baseline(health_snapshot.timeline.control_baseline_retained),
            report.engine_messages_since_sample,
            report.unattributed_engine_messages_since_sample,
            warning_severity, warning_component, warning_category);
    }
}

std::optional<core::RecoveryTransport> App::execute_supervisor_effect(
    const core::SupervisorEffect& effect) {
    std::string error;
    const auto transport = std::visit(Overloaded{
        [&](const core::ReopenStream&) {
            return player_.reopen_current(effect.generation, effect.load_attempt);
        },
        [&](const core::ReloadHlsLive&) {
            return player_.reopen_current(
                effect.generation, effect.load_attempt, false, true);
        },
        [&](const core::ReopenProbed&) {
            return player_.reopen_current(effect.generation, effect.load_attempt, true);
        },
        [&](const core::RecreatePlayer&) {
            return player_.recreate_player(effect.generation, effect.load_attempt, error);
        }}, effect.payload);
    if (!transport) {
        if (!error.empty()) log::error("Player recreation failed: {}", error);
        else log::warn("Recovery effect {} rejected for stale or unavailable target",
                       core::effect_name(effect.payload));
    }
    return transport;
}

void App::on_supervisor_state_changed(const core::SupervisorState& state,
                                      core::SupervisorStateName previous_state) {
    if (loading_channel_generation_ &&
        state.generation == *loading_channel_generation_ && state.first_frame_at) {
        loading_channel_generation_.reset();
        set_status(std::format("Playing {}", playing_channel_name_));
    }
    if (state.name == core::SupervisorStateName::Failed &&
        previous_state != core::SupervisorStateName::Failed) {
        if (loading_channel_generation_ &&
            state.generation == *loading_channel_generation_) {
            loading_channel_generation_.reset();
        }
        failure_power_grace_until_ =
            supervisor_clock_.now() + core::kTerminalFailurePowerGrace;
    } else if (state.name != core::SupervisorStateName::Failed) {
        failure_power_grace_until_.reset();
    }
    if (previous_state == core::SupervisorStateName::Failed &&
        state.name == core::SupervisorStateName::Zap &&
        state.generation == playback_session_.generation()) {
        // PlaybackSession admitted the still-running load back into probation
        // and restarted its health fold; App only reports that recovery.
        set_status("Playback resumed; confirming stability");
    }
    if (state.name == core::SupervisorStateName::Failed) {
        set_status(state.failure
                       ? std::format("Playback failed: {}", core::to_string(*state.failure))
                       : "Playback failed",
                   true);
    }
    update_playback_power(state);
}

void App::update_playback_power() {
    update_playback_power(playback_session_.state());
}

void App::update_playback_power(const core::SupervisorState& state) {
    const bool terminal_failure = state.name == core::SupervisorStateName::Failed;
    const bool grace_active = terminal_failure && failure_power_grace_until_ &&
                              supervisor_clock_.now() < *failure_power_grace_until_;
    power_request_.set_mode(core::decide_playback_power_mode({
        .session_active = state.name != core::SupervisorStateName::Idle,
        .user_paused = player::position_preserving_pause_requested(
            playback_control_capability_, playback_intent_),
        .window_minimized = window_.minimized(),
        .terminal_failure = terminal_failure,
        .terminal_failure_grace_active = grace_active,
    }));
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
    const float card_width = std::min(viewport->WorkSize.x - theme::scaled(theme::kSpace8),
                                      theme::scaled(kLoginCardWidth));
    const float padding    = theme::scaled(theme::kSpace7);

    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(card_width, 0.0f), ImGuiCond_Always);

    {
        // Only across Begin: the window reads its padding once, as it opens.
        theme::ScopedStyle style;
        style.var(ImGuiStyleVar_WindowPadding, ImVec2(padding, padding));
        ImGui::Begin("##login", nullptr,
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
                         ImGuiWindowFlags_NoScrollbar);
    }

    // Mark and wordmark share a line: the mark is sized from the title's own
    // height, so the two stay aligned whatever the scale is. Semibold and set
    // tight — the one place in the application allowed to be confident, which
    // is what buys everything below it the right to be quiet.
    {
        theme::ScopedStyle style;
        style.strong(1.9f);
        const float  mark   = ImGui::GetFontSize();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        theme::draw_logo(ImGui::GetWindowDrawList(),
                         ImVec2(origin.x + mark * 0.5f, origin.y + mark * 0.5f),
                         mark * 0.44f);
        ImGui::Dummy(ImVec2(mark, mark));
        ImGui::SameLine(0.0f, theme::scaled(theme::kSpace3));
        ImGui::TextUnformatted("Coax");
    }

    // The wordmark sits straight on the form, so it takes the gap the
    // explanatory paragraph used to occupy rather than butting against the
    // first label.
    ImGui::Dummy(ImVec2(0.0f, theme::scaled(theme::kSpace4)));

    // Labels sit above their fields rather than beside them: full-width inputs
    // leave room for the long URLs providers hand out, and the three rows read
    // as one column instead of two ragged ones.
    bool submit = false;
    auto field  = [&](const char* label, const char* hint, std::string& value,
                     ImGuiInputTextFlags flags) {
        // The label belongs to the field beneath it, so the gap between the
        // two is closed to well under the gap between one field and the next.
        // Proximity is what groups them; without it three labels and three
        // fields read as six evenly spaced rows.
        {
            theme::ScopedStyle style;
            style.var(ImGuiStyleVar_ItemSpacing,
                      ImVec2(ImGui::GetStyle().ItemSpacing.x, theme::scaled(theme::kSpace1)));
            theme::micro_label(label);
        }

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

    field("PORTAL URL", "https://provider.example.com:8080", portal_url_, 0);
    field("USERNAME", "", username_, 0);
    field("PASSWORD", "", password_, ImGuiInputTextFlags_Password);

    ImGui::Dummy(ImVec2(0.0f, theme::scaled(theme::kSpace2)));

    const bool connecting = stage_ == Stage::Connecting;
    ImGui::BeginDisabled(connecting);
    {
        theme::ScopedStyle style;
        style.color(ImGuiCol_Button, theme::kAccentFill)
             .color(ImGuiCol_ButtonHovered, theme::kAccentFillHover)
             .color(ImGuiCol_ButtonActive, theme::kAccentFillActive)
             .color(ImGuiCol_Text, theme::kOnAccent)
             .strong();
        if (ImGui::Button(connecting ? "Connecting..." : "Connect",
                          ImVec2(-FLT_MIN, theme::scaled(kConnectHeight)))) {
            submit = true;
        }
    }
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
            ImVec2(std::max(left, bar_min.x), bar_max.y - theme::scaled(theme::kStrokeTrack)),
            ImVec2(std::min(left + span, bar_max.x), bar_max.y),
            theme::kOnAccent);
    }

    if (!status_.empty()) {
        theme::ScopedStyle style;
        style.color(ImGuiCol_Text, status_error_ ? theme::kError : theme::kTextDim);
        ImGui::TextWrapped("%s", status_.c_str());
    }

    // Secondary and destructive, so it is a ghost button rather than a peer of
    // Connect: same weight for both would make forgetting the portal as easy
    // to hit as using it.
    {
        theme::ScopedStyle style;
        style.color(ImGuiCol_Button, theme::kTransparent)
             .color(ImGuiCol_Text, theme::kTextDim)
             .var(ImGuiStyleVar_FrameBorderSize, 0.0f)
             // No horizontal frame padding, so the label starts on the same
             // column as the labels and the status line above it rather than
             // a few pixels in.
             .var(ImGuiStyleVar_FramePadding,
                  ImVec2(0.0f, ImGui::GetStyle().FramePadding.y));
        if (ImGui::Button("Forget saved portal")) {
            win::CredentialStore::clear();
            portal_url_.clear();
            username_.clear();
            password_.clear();
            set_status("Saved portal cleared");
        }
    }

    ImGui::SameLine();
    const char* version = "v" COAX_VERSION;
    {
        theme::ScopedStyle style;
        style.micro().color(ImGuiCol_Text, theme::kTextFaint);
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - padding -
                             ImGui::CalcTextSize(version).x);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(version);
    }

    ImGui::End();
}

float App::browser_width() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    return std::min(viewport->WorkSize.x * 0.34f, theme::scaled(kBrowserMaxWidth));
}

bool App::channel_loading() const {
    if (!loading_channel_generation_) return false;
    const auto& state = playback_session_.state();
    return state.generation == *loading_channel_generation_ && !state.first_frame_at &&
           state.name != core::SupervisorStateName::Idle &&
           state.name != core::SupervisorStateName::Failed;
}

void App::draw_channel_loading() {
    if (!channel_loading()) return;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float left = viewport->WorkPos.x + (show_browser_ ? browser_width() : 0.0f);
    const float available_width = viewport->WorkPos.x + viewport->WorkSize.x - left;
    if (available_width <= 0.0f) return;

    ImGui::SetNextWindowPos(ImVec2(left, viewport->WorkPos.y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(available_width, viewport->WorkSize.y), ImGuiCond_Always);
    ImGui::Begin("##channel-loading", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoFocusOnAppearing);

    const float radius    = theme::scaled(11.0f);
    const float thickness = theme::scaled(2.0f);
    const float gap       = theme::scaled(theme::kSpace3);
    const float pad       = theme::scaled(theme::kSpace5);
    const float text_room = std::max(
        1.0f, std::min(theme::scaled(kLoadingMaxWidth), available_width - pad * 2.0f));
    const std::string label = elide(std::format("Loading {}", playing_channel_name_), text_room);
    const ImVec2 text_size = ImGui::CalcTextSize(label.c_str());

    const float content_width  = std::max(radius * 2.0f, text_size.x);
    const float content_height = radius * 2.0f + gap + text_size.y;
    const ImVec2 center(left + available_width * 0.5f,
                        viewport->WorkPos.y + viewport->WorkSize.y * 0.5f);
    const ImVec2 surface_min(center.x - content_width * 0.5f - pad,
                             center.y - content_height * 0.5f - pad);
    const ImVec2 surface_max(center.x + content_width * 0.5f + pad,
                             center.y + content_height * 0.5f + pad);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(surface_min, surface_max, theme::kScrim,
                        theme::scaled(theme::kSpace2));

    const ImVec2 spinner(center.x, surface_min.y + pad + radius);
    draw->AddCircle(spinner, radius, theme::kTrackMark, 32, thickness);

    constexpr float kTau = 6.2831853071795864769f;
    const float start = static_cast<float>(ImGui::GetTime()) * kTau * 0.85f;
    draw->PathArcTo(spinner, radius, start, start + kTau * 0.72f, 24);
    draw->PathStroke(theme::kAccent, ImDrawFlags_None, thickness);

    draw->AddText(ImVec2(center.x - text_size.x * 0.5f,
                         spinner.y + radius + gap),
                  theme::kText, label.c_str());
    ImGui::End();
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

    // The panel still reaches the top edge — a band of video above it would
    // read as a mistake rather than as a margin — but its contents start below
    // the drag strip. What the strip covers is blank panel, which is the one
    // thing in the column that can be dragged without taking a click from
    // something else.
    //
    // The cursor is moved rather than a spacer drawn: a Dummy is an item, and
    // an item under the pointer is exactly what tells the window the strip is
    // blocked — the gap would stop being draggable by existing.
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + caption_height());

    // Heading and keys on one line, the keys pushed to the far edge: at micro
    // size and faint they are a footnote to the panel rather than a second
    // label competing with its name.
    theme::micro_label("CHANNELS");
    ImGui::SameLine();
    {
        theme::ScopedStyle style;
        style.micro().color(ImGuiCol_Text, theme::kTextFaint);
        const char* keys = "TAB HIDES \xc2\xb7 F1 DIAGNOSTICS";
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x -
                             ImGui::CalcTextSize(keys).x);
        ImGui::TextUnformatted(keys);
    }

    TextField search(search_);
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputTextWithHint("##search", "Search channels and categories",
                                 search.buffer, sizeof(search.buffer))) {
        search_ = search.buffer;
    }

    const auto groups = channels_.filtered(search_);

    std::size_t shown = 0;
    for (const auto& group : groups) {
        shown += group.channels.size();
    }
    ImGui::TextDisabled("%zu of %zu channels", shown, channels_.channel_count());

    // Under the count rather than above it: the rule divides the header from
    // the list, and the count is part of the header — it describes what the
    // search left, not what the first category holds.
    ImGui::Separator();

    ImGui::BeginChild("channel-scroll", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);

    // A list is not a form. The window's row spacing is what separates one
    // control from the next, and applied to several hundred channels it turns
    // a column of names into a column of gaps — more scrolling, and no more
    // legible for it. Zero here: a row carries its own height, so consecutive
    // rows meet, and the band under the pointer is the row rather than a strip
    // inside it with a gap above and below.
    //
    // Scoped to the child rather than to the function, because ImGui checks
    // that a window leaves the style stacks as deep as it found them: a guard
    // still holding a push at EndChild fails that check even when nothing is
    // drawn between the two.
    {
        theme::ScopedStyle rows_style;
        rows_style.var(ImGuiStyleVar_ItemSpacing,
                       ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));

        // A channel row, laid out once here rather than per row. The row itself
        // spans the panel, so the band under the pointer is the whole width of the
        // list; inside it are three columns — a marker for the channel that is
        // playing, the provider's number, then the name.
        //
        // The name column sits where a category's own label sits, so the arrow
        // that opens a group and the number of a channel share one gutter and
        // every piece of text in the panel starts on one of two edges. The number
        // is measured from the widest in the whole catalogue rather than from
        // what happens to be on screen, so that edge does not move as the list is
        // scrolled or filtered.
        const float text_height  = ImGui::GetTextLineHeight();
        const float row_height   = text_height + theme::scaled(theme::kSpace2);
        const float marker_width = theme::scaled(theme::kStrokeMarker);
        const float number_gap   = theme::scaled(theme::kSpace3);
        const float number_left  = marker_width + theme::scaled(theme::kSpace2);
        const float label_indent =
            ImGui::GetTreeNodeToLabelSpacing() + ImGui::GetStyle().FramePadding.x;

        float number_width = 0.0f;
        if (const int widest = channels_.max_channel_number(); widest > 0) {
            // Capped at four digits. `num` is nominally the provider's display
            // position, but some of them fill it with the stream id instead, and
            // sizing the column from the largest value in the catalogue then
            // indents all nine hundred names by the width of one eight-digit
            // outlier. Four is what a channel number plausibly runs to; anything
            // longer keeps its digits and overhangs the gutter to the left, which
            // costs that one row rather than the list.
            //
            // Zeros rather than the number itself: the digits of a proportional
            // face are not all one width, and a column sized to "1000" is short of
            // what "8888" needs.
            const std::string zeros(std::min<std::size_t>(std::to_string(widest).size(), 4), '0');
            number_width = ImGui::CalcTextSize(zeros.c_str()).x;
        }
        // Wide enough for the numbers, and never narrower than the category labels
        // above them, so the names keep one edge with the headings they hang from.
        const float name_left = std::max(label_indent, number_left + number_width + number_gap);

        // Every group holding a match is opened while a search is running, and the
        // frame the search clears closes them again. Outside those two cases the
        // open state is left alone. Forcing it unconditionally — which is what
        // this did — re-closed a header on the frame after the click that opened
        // it, so the unfiltered list could not be expanded at all.
        const bool searching   = !search_.empty();
        const bool force_state = searching || search_was_active_;
        search_was_active_     = searching;

        bool first_group = true;
        for (const auto& group : groups) {
            const char* label = group.category ? group.category->name.c_str() : "Uncategorised";

            // Rows inside a category are two pixels apart, so without this the
            // last channel of one group and the heading of the next are as close
            // as two channels are, and the grouping stops reading.
            if (!first_group) {
                ImGui::Dummy(ImVec2(0.0f, theme::scaled(theme::kSpace2)));
            }
            first_group = false;

            if (force_state) {
                ImGui::SetNextItemOpen(searching, ImGuiCond_Always);
            }
            // Semibold, so a category reads as the thing the channels under it
            // hang from. Same size as the channels themselves: this is a list of
            // one kind of thing grouped, not two levels of navigation.
            //
            // And no fill at rest. ImGui paints one from ImGuiCol_Header
            // unconditionally, which turned every category into a filled bar and
            // the list into a stack of buttons. Hover and press keep their own
            // colours, so the control loses nothing but its weight. Scoped to the
            // header rather than to the list, because the same colour is what
            // marks the playing channel in the Selectables below.
            bool open = false;
            {
                theme::ScopedStyle style;
                style.color(ImGuiCol_Header, theme::kTransparent)
                     // Tighter than a framed control elsewhere gets. The window's
                     // frame padding is sized for something you click into and
                     // type; on a row that is only ever a heading it doubles the
                     // height of every collapsed category and turns the list into
                     // a column of headings and air.
                     .var(ImGuiStyleVar_FramePadding,
                          ImVec2(ImGui::GetStyle().FramePadding.x,
                                 theme::scaled(theme::kSpace1)))
                     .strong();
                open = ImGui::CollapsingHeader(label);
            }
            if (!open) {
                continue;
            }

            // Only the visible slice of a category is submitted to ImGui, so a
            // provider with tens of thousands of channels still scrolls smoothly.
            // The clipper needs the row height up front, because it decides which
            // rows to submit before any of them has been measured.
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(group.channels.size()), row_height);
            while (clipper.Step()) {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                    const core::Channel* channel = group.channels[static_cast<std::size_t>(i)];
                    const bool           playing = channel->id == playing_channel_id_;

                    // The row is a hit box and a wash; everything in it is drawn
                    // afterwards, over the top. A Selectable carrying the name
                    // could not put the number in a column of its own, and the
                    // playing channel would be marked by a wash so faint that
                    // finding it meant reading the list.
                    // Where the columns are measured from. Not the row's own left
                    // edge: a Selectable widens its box by half the horizontal
                    // item spacing so that a run of them has no dead gaps to
                    // click through, and text laid out against that edge sits
                    // half a spacing to the left of every other column in the
                    // panel — including the category headings these hang from.
                    const float text_left = ImGui::GetCursorScreenPos().x;

                    ImGui::PushID(channel->id.c_str());
                    const bool clicked =
                        ImGui::Selectable("##row", playing, ImGuiSelectableFlags_None,
                                          ImVec2(0.0f, row_height));
                    ImGui::PopID();

                    const ImVec2 row_min = ImGui::GetItemRectMin();
                    const ImVec2 row_max = ImGui::GetItemRectMax();
                    const float  base    = row_min.y + (row_height - text_height) * 0.5f;
                    ImDrawList*  rows    = ImGui::GetWindowDrawList();

                    if (playing) {
                        // A rule down the leading edge. The selected wash behind
                        // it is two levels of grey and disappears over a long
                        // list; this is the only accent in the panel, so it is
                        // findable at a glance from anywhere in the scroll.
                        rows->AddRectFilled(row_min, ImVec2(row_min.x + marker_width, row_max.y),
                                            theme::kAccent);
                    }

                    if (channel->number > 0) {
                        const std::string number = std::to_string(channel->number);
                        const float       width  = ImGui::CalcTextSize(number.c_str()).x;
                        // Right-aligned against the name column, so the units line
                        // up as the numbers grow a digit rather than the leading
                        // edge staying put and the gap to the name closing.
                        rows->AddText(ImVec2(text_left + name_left - number_gap - width, base),
                                      theme::kTextFaint, number.c_str());
                    }

                    const float room = row_max.x - (text_left + name_left) - theme::scaled(theme::kSpace2);
                    rows->AddText(ImVec2(text_left + name_left, base), theme::kText,
                                  elide(channel->name, room).c_str());

                    if (clicked) {
                        play(*channel);
                    }
                }
            }
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

void App::draw_status_bar() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();

    // The row of controls, and the margin around it. Everything in the bar is
    // placed against these three rather than against the style's spacing: it
    // is one hand-laid row, not a stack of framed widgets.
    const float pad = theme::scaled(theme::kSpace5);
    const float row = theme::scaled(theme::kSpace7);
    const float gap = theme::scaled(theme::kSpace3);

    // Taller than the row it carries, because the scrim has to reach nothing
    // at its top edge. A short ramp reads as a band drawn across the picture,
    // which is the boxed-in look this replaced.
    const float height = row + pad + theme::scaled(kOverlayRampExtra);
    const float top    = viewport->WorkPos.y + viewport->WorkSize.y - height;

    // The channel list owns its column for its whole height, so the playback
    // overlay begins where that ends. Drawn full width the two overlapped, and
    // the status text was printed across the bottom of the list.
    const float left  = viewport->WorkPos.x + (show_browser_ ? browser_width() : 0.0f);
    const float width = viewport->WorkPos.x + viewport->WorkSize.x - left;

    // Held open while the pointer is over the bar, a control is being dragged
    // or the settings menu is up, so nothing can fade out from under the hand
    // using it. The menu's own state is a frame behind: it belongs to a popup
    // owned by a window that has not been submitted yet. Held open too when
    // there is no video — over the backdrop it hides nothing, and a channel
    // has yet to be chosen.
    const ImGuiIO& io   = ImGui::GetIO();
    const bool     hold = !video_attached_ || overlay_menu_open_ || ImGui::IsAnyItemActive() ||
                          (io.MousePos.y >= top && io.MousePos.x >= left);
    const bool     want = hold || (ImGui::GetTime() - last_pointer_activity_) < kIdleSeconds;

    const float step = io.DeltaTime / kFadeSeconds;
    status_bar_fade_ = std::clamp(status_bar_fade_ + (want ? step : -step), 0.0f, 1.0f);
    if (status_bar_fade_ <= 0.0f) {
        return;
    }
    const float fade = status_bar_fade_;

    ImGui::SetNextWindowPos(ImVec2(left, top));
    ImGui::SetNextWindowSize(ImVec2(width, height));
    // Kept from before the padding is flattened below, for the settings menu:
    // that is an ordinary panel and wants the application's ordinary inset.
    const ImVec2 panel_padding = ImGui::GetStyle().WindowPadding;
    // No window background and no padding: the surface is the scrim below, and
    // every item in the bar is positioned by hand. The border has to go to
    // zero as well as undrawn — NoBackground stops it being painted, but the
    // window's clip rectangle is still inset by its width, which clipped the
    // scrim a pixel short and left a bright rule of unscrimmed video down
    // three of its edges.
    theme::ScopedStyle bar_style;
    bar_style.var(ImGuiStyleVar_Alpha, fade)
             .var(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f))
             .var(ImGuiStyleVar_WindowBorderSize, 0.0f);

    ImGui::Begin("##status", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse);

    ImDrawList*  draw   = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetWindowPos();
    const ImVec2 corner(origin.x + width, origin.y + height);
    theme::draw_overlay_scrim(draw, origin, corner, fade);

    // One row along the bottom edge. Items are centred on this line rather
    // than sharing a baseline, because a glyph, a rule and a line of text have
    // nothing in common except their middle.
    const float middle = corner.y - pad - row * 0.5f;
    const auto  place  = [middle](float x, float item_height) {
        ImGui::SetCursorScreenPos(ImVec2(x, middle - item_height * 0.5f));
    };

    // The right-hand group is measured before anything is drawn, so the
    // channel name can be clipped where the controls begin rather than run
    // underneath them.
    const float volume_width = theme::scaled(kVolumeWidth);
    const float group_width  = row + gap * 0.5f + volume_width + gap + row;
    const float group_left   = corner.x - pad - group_width;

    float cursor = origin.x + pad;

    place(cursor, row);
    const bool has_selection = !playing_channel_id_.empty() || !playing_channel_name_.empty();
    const auto control = has_selection
        ? player::playback_control(playback_control_capability_, playback_intent_)
        : player::PlaybackControl::Start;
    const auto control_icon =
        control == player::PlaybackControl::Start || control == player::PlaybackControl::Resume
            ? widgets::Icon::Play
            : (control == player::PlaybackControl::Pause ? widgets::Icon::Pause
                                                         : widgets::Icon::Stop);
    if (widgets::icon_button("##playback-control", control_icon, row, fade)) {
        toggle_playback();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", player::control_label(control));
    cursor += row + gap;

    // Clipped rather than wrapped or shortened: a long channel name should run
    // out where the controls start, not push them along or fold the row in two.
    const float text_height = ImGui::GetTextLineHeight();
    ImGui::PushClipRect(ImVec2(cursor, origin.y),
                        ImVec2(std::max(group_left - gap, cursor + 1.0f), corner.y), true);

    if (!playing_channel_name_.empty()) {
        place(cursor, text_height);
        // The one piece of primary information over the picture, so it is the
        // one thing in the row set in the semibold face.
        theme::ScopedStyle style;
        style.strong();
        ImGui::TextUnformatted(playing_channel_name_.c_str());
        cursor += ImGui::GetItemRectSize().x + gap;
    }

    // Beside the name, only what the name does not already say. The old row
    // printed "| Playing <channel>" next to the channel it was repeating.
    const auto& diagnostics = player_.diagnostics();
    place(cursor, text_height);
    if (status_error_) {
        theme::ScopedStyle style;
        style.color(ImGuiCol_Text, theme::kError);
        ImGui::TextUnformatted(status_.c_str());
    } else if (playback_intent_ == player::PlaybackIntent::StoppedByUser) {
        ImGui::TextDisabled("Stopped - Start to return live");
    } else if (channel_loading()) {
        ImGui::TextDisabled("Loading");
    } else if (diagnostics.paused_for_cache) {
        ImGui::TextDisabled("Buffering");
    }

    ImGui::PopClipRect();

    cursor = group_left;

    place(cursor, row);
    const widgets::Icon speaker = volume_ == 0                        ? widgets::Icon::VolumeMuted
                                  : volume_ <= core::kMaxVolume / 2 ? widgets::Icon::VolumeLow
                                                                    : widgets::Icon::VolumeHigh;
    if (widgets::icon_button("##mute", speaker, row, fade)) {
        // Mute remembers where the volume was rather than dropping it to zero
        // and making the way back a drag.
        if (volume_ > 0) {
            pre_mute_volume_ = volume_;
            volume_          = 0;
        } else {
            volume_ = pre_mute_volume_;
        }
        player_.set_volume(volume_);
    }
    cursor += row + gap * 0.5f;

    place(cursor, row);
    if (widgets::volume_slider("##volume", volume_, core::kMaxVolume, kUnityVolume,
                               volume_width, row, fade)) {
        player_.set_volume(volume_);
    }
    cursor += volume_width + gap;

    place(cursor, row);
    if (widgets::icon_button("##settings", widgets::Icon::Settings, row, fade)) {
        ImGui::OpenPopup("##overlay-settings");
    }

    // Super resolution and the diagnostics panel live here rather than in the
    // row: both are set once and then left alone, and a permanent checkbox
    // over the picture costs more attention than it is worth. The menu is a
    // surface of its own, so it opens at full opacity whatever the overlay is
    // fading towards.
    {
        theme::ScopedStyle style;
        style.var(ImGuiStyleVar_Alpha, 1.0f).var(ImGuiStyleVar_WindowPadding, panel_padding);
        if (ImGui::BeginPopup("##overlay-settings")) {
            draw_shared_menu_items();
            ImGui::EndPopup();
        }
    }
    ImGui::End();
}

void App::draw_shared_menu_items() {
    ImGui::MenuItem("Channels", "Tab", &show_browser_);
    ImGui::MenuItem("Diagnostics", "F1", &show_diagnostics_);
    ImGui::Separator();
    if (ImGui::MenuItem("Super resolution", nullptr, &vsr_enabled_)) {
        apply_vsr();
    }
    ImGui::TextDisabled("Upscales sources smaller than the window");
}

float App::caption_height() const {
    // Nothing to stand in for while Windows is drawing the caption, and nothing
    // to drag in fullscreen — which is also the state where a strip along the
    // top edge would sit over the picture and steal from it.
    if (!window_.minimal_frame() || window_.fullscreen()) {
        return 0.0f;
    }
    return theme::scaled(kCaptionHeight);
}

void App::set_minimal_mode(bool minimal) {
    if (minimal == settings_.minimal_mode) {
        return;
    }
    // The preference is recorded now — it is what the menu draws its tick from,
    // and it is not the part that can re-enter a frame. Only the window call is
    // held over.
    settings_.minimal_mode = minimal;
    win::SettingsStore::save(settings_);
    pending_minimal_frame_ = minimal;
    log::info("Minimal mode {}", minimal ? "enabled" : "disabled");
}

void App::apply_pending_window_changes() {
    // Taken before the calls rather than after. Each one draws a frame before
    // it returns, and a request left in place while that happens is a request
    // the next turn would carry out a second time.
    const auto minimal    = std::exchange(pending_minimal_frame_, std::nullopt);
    const auto fullscreen = std::exchange(pending_fullscreen_, std::nullopt);
    const bool minimize   = std::exchange(pending_minimize_, false);
    const bool maximize   = std::exchange(pending_maximize_, false);

    // Fullscreen first: it owns the frame while it is on, and set_minimal_frame
    // defers to it rather than fighting over the same window styles.
    if (fullscreen) {
        window_.set_fullscreen(*fullscreen);
    }
    if (minimal) {
        window_.set_minimal_frame(*minimal);
    }
    if (maximize) {
        window_.toggle_maximize();
    }
    if (minimize) {
        window_.minimize();
    }
}

void App::sign_out() {
    // Playback goes first, and directly rather than through stop_playback:
    // that path is the Stop button and declines in the states where there is
    // nothing a user would call stopping, which here would leave a channel
    // running behind the login screen.
    const auto generation = playback_session_.generation();
    playback_session_.stop(generation);
    player_.stop(generation);
    playback_intent_ = player::PlaybackIntent::StoppedByUser;
    loading_channel_generation_.reset();
    playing_channel_id_.clear();
    playing_channel_name_.clear();
    update_playback_power();

    // The stored credential, not just the fields: signing out and finding the
    // portal restored on the next launch would be the opposite of the ask.
    win::CredentialStore::clear();
    portal_url_.clear();
    username_.clear();
    password_.clear();

    client_.reset();
    channels_.reset({}, {});
    search_.clear();

    stage_ = Stage::Login;
    set_status("Signed out");
    log::info("Signed out");
}

void App::draw_title_bar() {
    const float height = caption_height();
    if (height <= 0.0f) {
        // No strip to reveal — Windows is drawing the caption, or the window is
        // fullscreen and there is nothing to drag. Reset rather than left where
        // it was, so the strip does not cross back in from half opacity the
        // next time minimal mode is switched on.
        title_bar_fade_ = 0.0f;
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();

    // The same column rule the playback bar follows: the channel list owns its
    // width for its whole height, so the strip begins where that ends. The
    // *draggable* region still spans the window — what stops at this edge is
    // only the part that draws.
    const float left  = viewport->WorkPos.x + (show_browser_ ? browser_width() : 0.0f);
    const float width = viewport->WorkPos.x + viewport->WorkSize.x - left;
    const float top   = viewport->WorkPos.y;

    // Held open while the pointer is in the strip, so the controls cannot fade
    // out from under the hand reaching for them, and while a menu is up. Held
    // open too when there is no video: over the backdrop it hides nothing, and
    // the login screen is the first thing anyone sees — the one moment where
    // the window most needs to say where its title bar is.
    const ImGuiIO& io   = ImGui::GetIO();
    const bool     hold = !video_attached_ || overlay_menu_open_ ||
                          (io.MousePos.y <= top + height && io.MousePos.x >= left);
    const bool     want = hold || (ImGui::GetTime() - last_pointer_activity_) < kIdleSeconds;

    const float step = io.DeltaTime / kFadeSeconds;
    title_bar_fade_  = std::clamp(title_bar_fade_ + (want ? step : -step), 0.0f, 1.0f);
    if (title_bar_fade_ <= 0.0f) {
        return;
    }
    const float fade = title_bar_fade_;

    ImGui::SetNextWindowPos(ImVec2(left, top));
    ImGui::SetNextWindowSize(ImVec2(width, height));

    // Flattened for the same reason the playback bar is: the surface is the
    // scrim, every item is placed by hand, and a border would inset the clip
    // rectangle and leave a bright rule of unscrimmed video down the edges.
    theme::ScopedStyle bar_style;
    bar_style.var(ImGuiStyleVar_Alpha, fade)
             .var(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f))
             .var(ImGuiStyleVar_WindowBorderSize, 0.0f);

    ImGui::Begin("##titlebar", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse);

    ImDrawList*  draw   = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetWindowPos();
    const ImVec2 corner(origin.x + width, origin.y + height);

    // The band itself, and the whole reason for the reveal. Three icons in a
    // corner say where the buttons are; this says where the title bar is, which
    // is the part with no other way of being found.
    theme::draw_title_scrim(draw, origin, corner, fade);

    // Square on the strip's own height, so the row of them is the strip rather
    // than something floating in it. Close is outermost, in the order Windows
    // has put these in for thirty years — this is not the place to be original.
    const float pad = theme::scaled(theme::kSpace2);
    const float row = height;
    float       cursor = corner.x - pad - row * 3.0f;

    const auto place = [&](widgets::Icon icon, const char* id) {
        ImGui::SetCursorScreenPos(ImVec2(cursor, origin.y));
        cursor += row;
        return widgets::icon_button(id, icon, row, fade);
    };

    if (place(window_.fullscreen() ? widgets::Icon::FullscreenExit : widgets::Icon::Fullscreen,
              "##title-fullscreen")) {
        pending_fullscreen_ = !window_.fullscreen();
    }
    if (place(widgets::Icon::Minimise, "##title-minimise")) {
        pending_minimize_ = true;
    }
    if (place(widgets::Icon::Close, "##title-close")) {
        window_.close();
    }

    ImGui::End();
}

void App::persist_volume() {
    const int previous = volume_last_frame_;
    volume_last_frame_ = volume_;

    // Three ways to be too early, and all of them are ordinary. Unchanged from
    // what is already on disk is the usual case and costs nothing. Different
    // from the previous frame means the value is still moving under a drag or a
    // run of wheel notches. An active item means the hand is still on it —
    // a slider released outside its own track ends the drag without the value
    // changing, so the reading alone cannot tell.
    if (volume_ == settings_.volume || volume_ != previous || ImGui::IsAnyItemActive()) {
        return;
    }

    settings_.volume = volume_;
    win::SettingsStore::save(settings_);
}

void App::draw_window_menu() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();

    // A surface covering the whole viewport, submitted before anything else and
    // kept at the back. It draws nothing: it exists to be the window the popup
    // belongs to, in every stage including the login screen, and to be present
    // over the video where no other surface reaches.
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);

    // Kept from before the host flattens it. ImGui checks that a window leaves
    // the style stacks as deep as it found them, so the guard below cannot be
    // released until after End — which means the popup, opened inside it, would
    // otherwise inherit the host's zero padding and sit tight against its own
    // edges. The menu is an ordinary panel and wants the ordinary inset.
    const ImVec2 panel_padding = ImGui::GetStyle().WindowPadding;

    theme::ScopedStyle host_style;
    host_style.var(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f))
              .var(ImGuiStyleVar_WindowBorderSize, 0.0f);

    ImGui::Begin("##window-menu-host", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoScrollWithMouse);

    // Anywhere in the window, because a gesture that works in some regions and
    // silently does nothing in others is a gesture nobody trusts. The guard is
    // against stacking a second menu on an open one — including on the gear's,
    // which carries the same rows.
    //
    // The strip along the top edge is the exception, and not by omission:
    // Windows sends its clicks as non-client messages that never reach here, so
    // right-clicking it raises the system menu instead. That is the division a
    // title bar has always had, and it is the one place the native Restore,
    // Move and Size are worth more than these rows.
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
        ImGui::OpenPopup(kWindowMenu);
    }

    {
        theme::ScopedStyle menu_style;
        menu_style.var(ImGuiStyleVar_WindowPadding, panel_padding);
        if (ImGui::BeginPopup(kWindowMenu)) {
            draw_shared_menu_items();

            // Only with a provider to leave. Direct media has no credentials
            // behind it, and the login screen already carries its own button.
            if (stage_ == Stage::Browsing && !direct_media_active_ && client_) {
                ImGui::Separator();
                if (ImGui::MenuItem("Sign out")) {
                    ImGui::CloseCurrentPopup();
                    sign_out();
                }
            }

            ImGui::Separator();

            bool minimal = settings_.minimal_mode;
            if (ImGui::MenuItem("Minimal mode", nullptr, &minimal)) {
                set_minimal_mode(minimal);
            }

            bool fullscreen = window_.fullscreen();
            if (ImGui::MenuItem("Fullscreen", "Alt+Enter", &fullscreen)) {
                pending_fullscreen_ = fullscreen;
            }
            if (ImGui::MenuItem("Minimise")) {
                pending_minimize_ = true;
            }
            // Not the same control as fullscreen, and the caption's version of
            // it went with the caption. Double-clicking the strip does this and
            // always has, but a gesture is not a replacement for something
            // there used to be a button for.
            if (ImGui::MenuItem(window_.maximized() ? "Restore" : "Maximise")) {
                pending_maximize_ = true;
            }
            if (ImGui::MenuItem("Close", "Alt+F4")) {
                window_.close();
            }
            ImGui::EndPopup();
        }
    }

    ImGui::End();
}

void App::publish_caption_region() {
    // Runs after every surface has been submitted, and cannot be folded into
    // the menu above: ImGui clears the hovered item at the start of each frame
    // and fills it in as items are drawn, so asked any earlier this reports
    // nothing hovered and the strip would swallow clicks meant for the
    // interface.
    //
    // Blocked wherever a click belongs to the interface rather than to the
    // window: over any widget, while text is being typed, and while a menu is
    // up — a popup opening under the strip would otherwise have its first row
    // taken by a drag.
    const ImGuiIO& io = ImGui::GetIO();
    window_.set_caption_height(static_cast<int>(caption_height()));
    window_.set_caption_blocked(ImGui::IsAnyItemHovered() || io.WantTextInput ||
                                ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId));

    // Either menu holds both overlays open — the gear is drawn on the bar it
    // would otherwise let fade, and the right-click menu can be opened
    // anywhere. Recorded here rather than where the bar is drawn because the
    // bar is not drawn in every stage, and a flag left behind by the last
    // stage that did draw it would hold the overlays open for good.
    overlay_menu_open_ =
        ImGui::IsPopupOpen("##overlay-settings") || ImGui::IsPopupOpen(kWindowMenu);
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
        ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - theme::scaled(theme::kSpace4),
               viewport->WorkPos.y + theme::scaled(theme::kSpace4) + caption_height()),
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
    theme::ScopedStyle panel_style;
    panel_style.var(ImGuiStyleVar_ItemSpacing,
                    ImVec2(ImGui::GetStyle().ItemSpacing.x,
                           theme::scaled(theme::kStrokeTrack)))
               // The gutter between the two columns of sections.
               .var(ImGuiStyleVar_CellPadding, ImVec2(theme::scaled(theme::kSpace4), 0.0f));

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - theme::scaled(theme::kSpace5),
               viewport->WorkPos.y + theme::scaled(theme::kSpace5) + caption_height()),
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
    const float label_column = std::max({ImGui::CalcTextSize("Presentation rebuilds").x,
                                         ImGui::CalcTextSize("Hardware decode wanted").x,
                                         ImGui::CalcTextSize("Rebuffers this channel").x});
    const float column_gap   = theme::scaled(theme::kSpace5);

    // Label dim, value bright: the reading is what is being looked for, and
    // the two columns separate without a rule between them.
    auto field = [label_column, column_gap](const char* label, std::string_view value,
                                            bool dim = false) {
        {
            theme::ScopedStyle style;
            style.color(ImGuiCol_Text, theme::kTextDim);
            ImGui::TextUnformatted(label);
        }
        // A label wider than the measured maximum keeps the gap and loses the
        // alignment, which is the harmless way round.
        ImGui::SameLine(0.0f, std::max(label_column - ImGui::CalcTextSize(label).x, 0.0f) +
                                  column_gap);
        theme::ScopedStyle value_style;
        if (dim) {
            value_style.color(ImGuiCol_Text, theme::kTextDim);
        }
        ImGui::TextUnformatted(value.data(), value.data() + value.size());
    };

    // Two columns of whole sections. Stacked, the readings run to about a
    // third more than the window is tall and the panel scrolls; the sections
    // are self-contained, so splitting them costs nothing.
    if (!ImGui::BeginTable("##readings", 2, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::End();
        return;
    }
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);

    // Broken out rather than reported as one word, because "attached" cannot
    // distinguish a live attachment from a stale one — which is the whole
    // failure. The epoch says which generation of mpv's video output the
    // attached object belongs to, and the acquisition path says whether
    // property observation is really carrying the contract or the
    // reconfiguration fallback is doing the work.
    theme::separator_label("PRESENTATION");
    field("Swap chain attached", d.swapchain_attached ? "yes" : "no");
    field("Swap chain epoch", std::format("{}", d.swapchain_epoch));
    field("Acquired via", player::to_string(d.swapchain_acquisition));
    field("Replacements", std::format("{}", d.swapchain_replacements));
    field("Re-attachments", std::format("{}", d.swapchain_reattachments));
    field("Device losses", std::format("{}", device_loss_events_));
    field("Last device loss", last_device_loss_.empty() ? "-" : last_device_loss_,
          last_device_loss_.empty());
    field("Presentation rebuilds", std::format("{}", presentation_rebuilds_));
    field("Window", std::format("{}x{}", window_.width(), window_.height()));

    // Runs the whole loss path — bounded rebuild, player recreation,
    // re-attach at a new epoch — without a real device removal. §7.3 is
    // re-checked on every runtime upgrade, and the honest triggers for that
    // (disabling the display adapter, suspending the machine) are disruptive
    // enough that in practice they do not get run, which would leave the
    // recovery path re-verified by nothing. Deliberately reached only from
    // this panel rather than a keybind, so it cannot be hit while watching.
    //
    // It does not fake a device loss: the loss counters and the last-loss
    // reason describe what DXGI actually reported, and a forced rebuild is
    // not that.
    if (ImGui::SmallButton("Force rebuild")) {
        log::warn("Presentation rebuild forced from diagnostics");
        presentation_budget_.request(supervisor_clock_.now());
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(exercises the device-loss recovery path)");

    theme::separator_label("DECODE");
    field("Codec", d.video_codec.empty() ? "-" : d.video_codec);
    field("Source", std::format("{}x{}", d.video_width, d.video_height));
    field("Hardware decode wanted", d.hwdec_requested);
    field("Hardware decode active", d.hwdec_active.empty() ? "-" : d.hwdec_active);

    theme::separator_label("SUPER RESOLUTION");
    // Requested, attached and confirmed are distinct on purpose. There is no
    // reliable signal that the driver actually ran RTX VSR on a frame, so this
    // never claims it did.
    field("Requested", d.vsr_requested ? "yes" : "no");
    field("Filter attached", d.vsr_filter_attached ? "yes" : "no");
    field("Confirmed", "unavailable (no signal exposed)", true);

    theme::separator_label("STREAM");
    field("Core idle", d.core_idle ? "yes" : "no");
    field("Paused for cache", d.paused_for_cache ? "yes" : "no");
    // Distinguished from a genuine 0.0s: mpv often cannot report this even
    // while data is buffered, and the controller holds 1.0x when it cannot.
    if (d.cache_duration_seconds) {
        field("Demuxer cache", std::format("{:.1f}s", *d.cache_duration_seconds));
    } else {
        field("Demuxer cache", "unavailable", true);
    }
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
        playback_session_.state(), supervisor_clock_.now());
    const auto& health_snapshot = playback_session_.health_snapshot();
    theme::separator_label("PLAYBACK HEALTH");
    field("Verdict", core::to_string(health_snapshot.verdict));
    field("Degraded reason", health_snapshot.degraded_reason
                                 ? core::to_string(*health_snapshot.degraded_reason) : "-");
    field("Progressing", !health_snapshot.progressing ? "unknown"
                             : (*health_snapshot.progressing ? "yes" : "no"));
    field("Input advancing", !health_snapshot.input_advancing ? "unknown"
                                 : (*health_snapshot.input_advancing ? "yes" : "no"));
    field("Timeline kind", player::to_string(playback_session_.timeline_classification()));
    field("Sample elapsed", signed_seconds(health_snapshot.timeline.elapsed_seconds));
    field("Playback movement",
          signed_seconds(health_snapshot.timeline.playback_movement_seconds));
    field("Playback deviation",
          signed_seconds(health_snapshot.timeline.playback_deviation_seconds));
    field("Cache-end movement",
          signed_seconds(health_snapshot.timeline.cache_end_movement_seconds));
    field("Health movement",
          signed_seconds(health_snapshot.timeline.control_playback_movement_seconds));
    field("Health deviation",
          signed_seconds(health_snapshot.timeline.control_playback_deviation_seconds));
    field("Health baseline",
          control_baseline(health_snapshot.timeline.control_baseline_retained));
    field("Previous cache pause",
          optional_pause(health_snapshot.timeline.previous_cache_paused));
    field("Last active-entry engine message",
          d.last_engine_message
              ? std::format("{} / {} / {}", player::to_string(d.last_engine_message->severity),
                            player::to_string(d.last_engine_message->component),
                            player::to_string(d.last_engine_message->category))
              : std::string("-"));
    field("Active-entry engine messages", std::format("{}", d.engine_message_count));
    field("Last unattributed engine message",
          d.last_unattributed_engine_message
              ? std::format("{} / {} / {}",
                            player::to_string(d.last_unattributed_engine_message->severity),
                            player::to_string(d.last_unattributed_engine_message->component),
                            player::to_string(d.last_unattributed_engine_message->category))
              : std::string("-"));
    field("Unattributed engine messages",
          std::format("{}", d.unattributed_engine_message_count));
    if (d.request_shape) {
        field("Request shape",
              std::format("p{} / c{} / load {} / {} / {} / {}",
                          d.request_shape->correlation.provider_session,
                          d.request_shape->correlation.channel_session,
                          d.request_shape->load_attempt.value(),
                          core::to_string(d.request_shape->intent),
                          player::to_string(d.request_shape->scheme),
                          player::to_string(d.request_shape->target)));
    } else {
        field("Request shape", "-");
    }

    theme::separator_label("SUPERVISOR");
    field("State", core::to_string(supervisor_stats.state));
    field("Transport",
          supervisor_stats.transport ? core::to_string(*supervisor_stats.transport) : "-");
    field("Attempt", std::format("{} / {}", supervisor_stats.attempt,
                                 supervisor_stats.attempt_ceiling));
    field("Load attempt", std::format("{} / {}",
                                      supervisor_stats.load_attempt.value(),
                                      core::to_string(supervisor_stats.load_intent)));
    field("Player recreation used",
          supervisor_stats.short_load_recreation_used ? "yes" : "no");
    field("Reason", supervisor_stats.reason ? std::string_view(*supervisor_stats.reason) : "-");
    field("Recovery budget",
          supervisor_stats.elapsed_budget
              ? std::format("{:.0f}ms", supervisor_stats.elapsed_budget->count() * 1000.0)
              : std::string("-"));
    field("Policy", supervisor_stats.policy_version);

    theme::separator_label("LIVE SYNC");
    field("Target offset", std::format("{:.1f}s", d.live_target_seconds));
    field("Playback speed", std::format("{:.3f}x", d.playback_speed));
    field("Rebuffers this channel", std::format("{}", d.rebuffer_count));
    {
        theme::ScopedStyle style;
        style.color(ImGuiCol_Text, theme::kTextDim);
        ImGui::TextUnformatted("Offset is estimated from buffer depth (no manifest)");
    }

    ImGui::EndTable();

    // Full width, under both columns: log lines are long and splitting them
    // into a column would wrap every one of them.
    if (ImGui::CollapsingHeader("Log")) {
        ImGui::BeginChild("log-scroll",
                          ImVec2(theme::scaled(kLogWidth), theme::scaled(kLogHeight)));
        log::recent_into(log_snapshot_);
        for (const auto& line : log_snapshot_) {
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

    // Between a rebuild's teardown and its completion there is no backend to
    // draw with. Skipped rather than guarded further in, because a partial
    // frame is worth nothing: the composition tree that would present it does
    // not exist either. Reached from the window procedure as well as the
    // frame loop, so the check has to live here rather than at the call site.
    if (!presentation_ready_) {
        return;
    }

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
        toggle_playback();
    }

    // Until mpv owns the video plane there is nothing behind the UI layer but
    // an empty composition visual, which the desktop shows through as white.
    // The backdrop is what the window is made of in every state before the
    // first frame arrives — logging in, connected but idle, and mid-load.
    if (!video_attached_) {
        theme::draw_backdrop();
    }

    // First, so the surface carrying the right-click menu sits behind every
    // panel drawn below rather than over them.
    draw_window_menu();

    switch (stage_) {
        case Stage::Login:
        case Stage::Connecting:
            draw_login();
            break;
        case Stage::Browsing:
            draw_browser();
            draw_channel_loading();
            draw_status_bar();
            break;
    }
    // After the stages and before the panels that float above everything: the
    // strip belongs to the window rather than to what is in it, and is drawn in
    // every stage including the login screen.
    draw_title_bar();
    draw_update_banner();
    draw_diagnostics();
    persist_volume();
    publish_caption_region();

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

    // Zero on the first turn: nothing has been drawn yet, so there is nothing
    // to be paced against.
    DWORD wait_ms = 0;
    while (window_.pump_messages(wait_ms)) {
        player_.pump();
        const auto events = player_.take_events();
        playback_session_.service_turn(events, [this] {
            // Preserve the production ordering: presentation loss is
            // dispatched after player edges but before cache/health levels and
            // the deadline poll for this turn.
            service_presentation();
        });

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

        finish_connect();
        update_playback_power();
        draw_frame();
        // After the frame, never inside one. This is the only place the window
        // is resized on the interface's behalf.
        apply_pending_window_changes();
        wait_ms = next_turn_wait_ms();
    }

    shutdown();
    return 0;
}

DWORD App::next_turn_wait_ms() const {
    const auto now  = supervisor_clock_.now();
    const auto wait = core::decide_frame_wait(presentation_phase(), now,
                                              presentation_budget_.next_decision_at(now),
                                              playback_session_.armed_deadline());
    // Absent means the turn just drawn ended in a present, whose vsync wait is
    // already the throttle.
    if (!wait) {
        return 0;
    }
    // Rounded up rather than truncated, so a deadline a fraction of a
    // millisecond out is one short sleep instead of a run of zero-length waits
    // that is the spin under another name.
    return static_cast<DWORD>(std::chrono::ceil<std::chrono::milliseconds>(*wait).count());
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
    playback_session_.dispose();
    power_request_.set_mode(core::PlaybackPowerMode::AllowSleep);
    player_.stop(playback_session_.generation());
    // Outwards from the content, the same order the rebuild path uses. The
    // player detaches again when it is destroyed with this object, but that is
    // member destruction order deciding it; doing it here is what makes the
    // ordering the code's rather than the layout's.
    player_.detach_swapchain();
    composition_.destroy();
    ui_.destroy();
    ImGui_ImplWin32_Shutdown();
    window_.destroy();
    log::info("Shutdown complete");
}

}  // namespace coax::app
