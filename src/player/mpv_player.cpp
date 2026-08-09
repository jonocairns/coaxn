#include "player/mpv_player.hpp"

#include <unknwn.h>

#include <mpv/client.h>

#include <format>

#include "core/policy.hpp"
#include "core/supervisor.hpp"
#include "util/log.hpp"
#include "player/transport_log_classifier.hpp"

namespace coax::player {
namespace {

enum ObserveId : std::uint64_t {
    kDisplaySwapchain = 1,
    kHwdecCurrent,
    kVideoCodec,
    kVideoWidth,
    kVideoHeight,
    kCoreIdle,
    kPausedForCache,
    kCacheDuration,
    kCacheEnd,
    kPlaybackTime,
    kAvSync,
    kCacheSpeed,
    kEstimatedVfFps,
    kContainerFps,
};

void set_option(mpv_handle* mpv, const char* name, const std::string& value) {
    const int status = mpv_set_option_string(mpv, name, value.c_str());
    if (status < 0) {
        log::warn("mpv option {}={} rejected: {}", name, value, mpv_error_string(status));
    }
}

std::string composition_size(int width, int height) {
    return std::format("{}x{}", width < 1 ? 1 : width, height < 1 ? 1 : height);
}

PlayerEndReason normalize_end_reason(mpv_end_file_reason reason) {
    switch (reason) {
        case MPV_END_FILE_REASON_EOF: return PlayerEndReason::Eof;
        case MPV_END_FILE_REASON_STOP: return PlayerEndReason::Stop;
        case MPV_END_FILE_REASON_QUIT: return PlayerEndReason::Quit;
        case MPV_END_FILE_REASON_ERROR: return PlayerEndReason::Error;
        case MPV_END_FILE_REASON_REDIRECT: return PlayerEndReason::Redirect;
    }
    return PlayerEndReason::Unknown;
}

void observe(mpv_handle* mpv, std::uint64_t id, const char* name, mpv_format format) {
    const int status = mpv_observe_property(mpv, id, name, format);
    if (status < 0) log::warn("mpv property {} observation rejected: {}", name,
                              mpv_error_string(status));
}

constexpr const char* kDisplaySwapchainProperty = "display-swapchain";

std::uint64_t swapchain_address(void* pointer) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(pointer));
}

}  // namespace

MpvPlayer::~MpvPlayer() {
    events_.dispose();
    destroy_backend();
}

bool MpvPlayer::initialize(const PlayerConfig& config, std::string& error) {
    config_ = config;
    has_config_ = true;
    return initialize_backend(error);
}

bool MpvPlayer::initialize_backend(std::string& error) {
    mpv_ = mpv_create();
    if (!mpv_) { error = "mpv_create failed"; return false; }

    set_option(mpv_, "vo", "gpu-next");
    set_option(mpv_, "gpu-api", "d3d11");
    set_option(mpv_, "d3d11-output-mode", "composition");
    set_option(mpv_, "d3d11-composition-size",
               composition_size(config_.composition_width, config_.composition_height));
    diagnostics_.hwdec_requested = config_.hardware_decode ? "d3d11va" : "no";
    set_option(mpv_, "hwdec", diagnostics_.hwdec_requested);

    // Provider MPEG-TS needs complete PMT probing. These are opt-in only; zero
    // deliberately preserves the pinned runtime defaults.
    if (config_.analyze_duration_seconds > 0.0) {
        set_option(mpv_, "demuxer-lavf-analyzeduration",
                   std::format("{:.2f}", config_.analyze_duration_seconds));
    }
    if (config_.probe_size_bytes > 0) {
        set_option(mpv_, "demuxer-lavf-probesize", std::to_string(config_.probe_size_bytes));
    }

    set_option(mpv_, "cache", "yes");
    set_option(mpv_, "demuxer-max-bytes", "64MiB");
    set_option(mpv_, "demuxer-max-back-bytes", "16MiB");
    set_option(mpv_, "cache-secs", "1");
    set_option(mpv_, "demuxer-readahead-secs", "1");
    set_option(mpv_, "cache-pause", "yes");
    set_option(mpv_, "cache-pause-initial", "yes");
    set_option(mpv_, "cache-pause-wait", "2");

    if (config_.transport_reconnect) {
        set_option(mpv_, "stream-lavf-o",
                   "reconnect=1,reconnect_on_network_error=1,reconnect_delay_max=5");
    }
    // No network-timeout override. The multi-signal health fold owns prolonged
    // silence, while mpv retains its pinned-runtime socket timing policy.

    set_option(mpv_, "audio-pitch-correction", "yes");
    set_option(mpv_, "idle", "yes");
    set_option(mpv_, "terminal", "no");
    set_option(mpv_, "keep-open", "no");
    set_option(mpv_, "force-window", "no");
    set_option(mpv_, "osc", "no");
    set_option(mpv_, "input-default-bindings", "no");
    set_option(mpv_, "input-vo-keyboard", "no");
    set_option(mpv_, "audio-client-name", "Coax");

    const int status = mpv_initialize(mpv_);
    if (status < 0) {
        error = std::format("mpv_initialize failed: {}", mpv_error_string(status));
        destroy_backend();
        return false;
    }

    mpv_request_log_messages(mpv_, "warn");
    observe(mpv_, kDisplaySwapchain, kDisplaySwapchainProperty, MPV_FORMAT_INT64);
    observe(mpv_, kHwdecCurrent, "hwdec-current", MPV_FORMAT_STRING);
    observe(mpv_, kVideoCodec, "video-codec", MPV_FORMAT_STRING);
    observe(mpv_, kVideoWidth, "width", MPV_FORMAT_INT64);
    observe(mpv_, kVideoHeight, "height", MPV_FORMAT_INT64);
    observe(mpv_, kCoreIdle, "core-idle", MPV_FORMAT_FLAG);
    observe(mpv_, kPausedForCache, "paused-for-cache", MPV_FORMAT_FLAG);
    observe(mpv_, kCacheDuration, "demuxer-cache-duration", MPV_FORMAT_DOUBLE);
    observe(mpv_, kCacheEnd, "demuxer-cache-time", MPV_FORMAT_DOUBLE);
    observe(mpv_, kPlaybackTime, "playback-time", MPV_FORMAT_DOUBLE);
    observe(mpv_, kAvSync, "avsync", MPV_FORMAT_DOUBLE);
    observe(mpv_, kCacheSpeed, "cache-speed", MPV_FORMAT_DOUBLE);
    observe(mpv_, kEstimatedVfFps, "estimated-vf-fps", MPV_FORMAT_DOUBLE);
    observe(mpv_, kContainerFps, "container-fps", MPV_FORMAT_DOUBLE);

    log::info("libmpv initialized (client API {}.{})",
              mpv_client_api_version() >> 16, mpv_client_api_version() & 0xFFFF);
    return true;
}

void MpvPlayer::destroy_backend() {
    // Detach before mpv can release its own reference. The visual holds a
    // reference of DirectComposition's own, so clearing the content is what
    // lets the object die; doing it afterwards is the stale-pointer window.
    detach_swapchain();
    if (mpv_) mpv_terminate_destroy(mpv_);
    mpv_ = nullptr;
    current_entry_id_.reset();
}

std::uint64_t MpvPlayer::next_request_id() { return ++request_sequence_; }

bool MpvPlayer::play(const std::string& url, core::Generation generation,
                     core::LoadAttempt load_attempt, core::RecoveryTransport transport,
                     bool force_probed_format,
                     SourceCorrelation correlation) {
    if (!mpv_) return false;
    target_ = PlaybackTarget{url, generation, load_attempt, core::LoadIntent::FreshSelection,
                             transport, force_probed_format, correlation};
    reset_load_observations(diagnostics_);
    buffer_phase_gate_.begin_load(generation);
    apply_buffer_phase(generation, core::BufferPhase::Zap);
    return issue_load(force_probed_format);
}

bool MpvPlayer::issue_load(bool force_probed_format) {
    if (!mpv_ || !target_) return false;
    diagnostics_.request_shape = inspect_request_shape(
        target_->url, target_->load_intent, target_->load_attempt,
        target_->transport, force_probed_format,
        target_->correlation);
    const auto& request = *diagnostics_.request_shape;
    log::info(
        "Load request generation {} load-attempt={} provider-session={} channel-session={} "
        "intent={} command=loadfile mode=replace transport={} "
        "scheme={} target={} query={} userinfo={} forced-format={}; "
        "HTTP method/range/headers unobserved below libmpv",
        target_->generation.value(), target_->load_attempt.value(),
        request.correlation.provider_session,
        request.correlation.channel_session, to_string(request.intent),
        core::to_string(request.transport), to_string(request.scheme),
        to_string(request.target), request.query_present ? "present" : "absent",
        request.userinfo_present ? "present" : "absent",
        request.forced_format ? "yes" : "no");
    load_started_at_ = std::chrono::steady_clock::now();
    load_in_flight_ = true;
    file_loaded_ = false;
    transport_log_armed_ = false;
    transport_classification_reported_ = false;
    engine_diagnostic_log_gate_.reset();
    target_->probed_format_forced = force_probed_format;

    const std::uint64_t request_id = next_request_id();
    events_.track_load(request_id, target_->generation, target_->load_attempt);

    mpv_node command{};
    mpv_node values[5]{};
    mpv_node_list list{};
    command.format = MPV_FORMAT_NODE_ARRAY;
    command.u.list = &list;
    list.values = values;
    list.num = 3;
    values[0].format = MPV_FORMAT_STRING;
    values[0].u.string = const_cast<char*>("loadfile");
    values[1].format = MPV_FORMAT_STRING;
    values[1].u.string = const_cast<char*>(target_->url.c_str());
    values[2].format = MPV_FORMAT_STRING;
    values[2].u.string = const_cast<char*>("replace");

    mpv_node option_values[2]{};
    char* option_keys[2]{};
    mpv_node_list options{};
    std::string lavf_options;
    const char* forced_format = nullptr;
    if (force_probed_format) {
        forced_format = target_->transport == core::RecoveryTransport::Hls ? "hls" : "mpegts";
    }
    int option_count = 0;
    if (forced_format) {
        option_keys[option_count] = const_cast<char*>("demuxer-lavf-format");
        option_values[option_count].format = MPV_FORMAT_STRING;
        option_values[option_count].u.string = const_cast<char*>(forced_format);
        ++option_count;
    }
    if (target_->transport == core::RecoveryTransport::Hls) {
        lavf_options = std::format("live_start_index={},{}", core::kHlsLiveStartIndex,
                                   core::kHlsRuntimeRetryOptions);
        option_keys[option_count] = const_cast<char*>("demuxer-lavf-o");
        option_values[option_count].format = MPV_FORMAT_STRING;
        option_values[option_count].u.string = lavf_options.data();
        ++option_count;
    }
    if (option_count > 0) {
        values[3].format = MPV_FORMAT_INT64;
        values[3].u.int64 = -1;
        values[4].format = MPV_FORMAT_NODE_MAP;
        values[4].u.list = &options;
        options.num = option_count;
        options.values = option_values;
        options.keys = option_keys;
        list.num = 5;
    }

    const int status = mpv_command_node_async(mpv_, request_id, &command);
    if (status < 0) {
        load_in_flight_ = false;
        events_.command_rejected_immediately(request_id, status);
        log::error("loadfile command rejected: {}", mpv_error_string(status));
        return false;
    }
    return true;
}

void MpvPlayer::stop(core::Generation generation) {
    if (!mpv_) return;
    if (const auto entry = events_.active_entry()) {
        events_.intentional_stop(*entry, generation, IntentionalStopKind::Requested);
    }
    const char* command[] = {"stop", nullptr};
    mpv_command_async(mpv_, next_request_id(), command);
}

std::optional<core::RecoveryTransport> MpvPlayer::reopen_current(
    core::Generation generation, core::LoadAttempt load_attempt,
    bool force_probed_format, bool require_hls) {
    if (!target_ || target_->generation != generation || !mpv_ ||
        load_attempt <= target_->load_attempt ||
        (require_hls && target_->transport != core::RecoveryTransport::Hls)) return std::nullopt;
    target_->load_attempt = load_attempt;
    target_->load_intent = core::LoadIntent::RecoveryReopen;
    reset_load_observations(diagnostics_);
    buffer_phase_gate_.begin_load(generation);
    apply_buffer_phase(generation, core::BufferPhase::Zap);
    if (!issue_load(force_probed_format)) return std::nullopt;
    return target_->transport;
}

std::optional<core::RecoveryTransport> MpvPlayer::recreate_player(
    core::Generation generation, core::LoadAttempt load_attempt, std::string& error) {
    if (!target_ || target_->generation != generation || !has_config_ ||
        load_attempt <= target_->load_attempt) return std::nullopt;
    const auto target = *target_;
    destroy_backend();
    events_ = PlayerEventAdapter{};
    if (!initialize_backend(error)) return std::nullopt;
    // Synchronous UI-thread recreation cannot be superseded mid-call, but the
    // equality check is retained as the explicit replacement fence.
    if (!target_ || target_->generation != generation) return std::nullopt;
    target_->load_attempt = load_attempt;
    target_->load_intent = core::LoadIntent::PlayerRecreation;
    reset_load_observations(diagnostics_);
    buffer_phase_gate_.begin_load(generation);
    apply_buffer_phase(generation, core::BufferPhase::Zap);
    if (!issue_load(target.probed_format_forced)) {
        return std::nullopt;
    }
    return target.transport;
}

bool MpvPlayer::apply_buffer_phase(core::Generation generation, core::BufferPhase phase) {
    if (!mpv_ || !target_ || target_->generation != generation ||
        !buffer_phase_gate_.begin(generation, phase)) return false;
    diagnostics_.buffer_phase = phase;
    diagnostics_.buffer_phase_command_state = BufferPhaseCommandState::Pending;
    const auto targets = core::buffer_phase_targets(phase);
    const bool cache = issue_buffer_property(generation, phase, BufferProperty::CacheSeconds,
                                             targets.cache_seconds);
    const bool readahead = issue_buffer_property(generation, phase,
        BufferProperty::ReadaheadSeconds, targets.readahead_seconds);
    return cache && readahead;
}

void MpvPlayer::observe_buffer_command_result(core::Generation generation,
                                              const PropertyCommandResult& result) {
    result.accepted ? ++diagnostics_.buffer_commands_accepted
                    : ++diagnostics_.buffer_commands_rejected;
    const auto property = result.property == BufferProperty::CacheSeconds
        ? BufferPhaseProperty::CacheSeconds : BufferPhaseProperty::ReadaheadSeconds;
    const auto settlement = buffer_phase_gate_.settle(
        generation, result.phase, property, result.accepted);
    if (!settlement) return;
    diagnostics_.buffer_phase_command_state = *settlement;
}

bool MpvPlayer::issue_buffer_property(core::Generation generation, core::BufferPhase phase,
                                      BufferProperty property, double value) {
    const char* name = property == BufferProperty::CacheSeconds
        ? "cache-secs" : "demuxer-readahead-secs";
    const std::string seconds_value = std::format("{}", value);
    // libmpv's C client command surface uses input.conf command names. JSON
    // IPC calls this operation `set_property`, while the native command is
    // `set`; mixing the two is rejected as MPV_ERROR_INVALID_PARAMETER.
    const char* command[] = {"set", name, seconds_value.c_str(), nullptr};
    const std::uint64_t request_id = next_request_id();
    events_.track_property(request_id, generation, phase, property);
    const int status = mpv_command_async(mpv_, request_id, command);
    if (status < 0) {
        events_.command_rejected_immediately(request_id, status);
        return false;
    }
    return true;
}

void MpvPlayer::set_composition_size(int width, int height) {
    config_.composition_width = width;
    config_.composition_height = height;
    if (!mpv_) return;
    const std::string value = composition_size(width, height);
    mpv_set_property_string(mpv_, "d3d11-composition-size", value.c_str());
}

void MpvPlayer::set_vsr(bool enabled, double scale) {
    if (!mpv_) return;
    vsr_enabled_ = enabled;
    vsr_scale_ = scale;
    const std::string filter = enabled
        ? std::format("d3d11vpp=scaling-mode=nvidia:scale={:.3f}", scale) : std::string{};
    if (filter == applied_filter_) return;
    applied_filter_ = filter;
    const int status = mpv_set_property_string(mpv_, "vf", filter.c_str());
    diagnostics_.vsr_requested = enabled;
    diagnostics_.vsr_filter_attached = enabled && status >= 0;
    if (status < 0) log::warn("Setting video filter failed: {}", mpv_error_string(status));
}

void MpvPlayer::set_volume(int percent) {
    if (!mpv_) return;
    const std::string value = std::to_string(percent);
    mpv_set_property_string(mpv_, "volume", value.c_str());
}
void MpvPlayer::set_speed(double speed) {
    if (!mpv_) return;
    const std::string value = std::format("{:.4f}", speed);
    mpv_set_property_string(mpv_, "speed", value.c_str());
    diagnostics_.playback_speed = speed;
}
void MpvPlayer::set_live_sync_state(double target_seconds, int rebuffer_count) {
    diagnostics_.live_target_seconds = target_seconds;
    diagnostics_.rebuffer_count = rebuffer_count;
}
void MpvPlayer::set_paused(bool paused) {
    if (mpv_) mpv_set_property_string(mpv_, "pause", paused ? "yes" : "no");
}

void MpvPlayer::publish_swapchain(void* swapchain, SwapchainAcquisition source) {
    const core::SwapchainIdentity incoming{swapchain_address(swapchain), swapchain_epoch_};
    const auto transition = core::decide_swapchain_transition(attached_, incoming);
    if (transition == core::SwapchainTransition::Ignore) return;
    // A replacement at a new address is mpv having built a different object. A
    // replacement at the same address is this epoch rule doing its job: the two
    // are worth telling apart when reading a log after a failure.
    const bool address_changed = attached_.address != incoming.address;

    // Reference first, before the address is handed anywhere. Between reading
    // the property and DirectComposition taking its own reference on
    // SetContent, nothing owns this object, and mpv's video output does not
    // tear down on this thread.
    win::ComPtr<IUnknown> acquired;
    if (incoming.present()) acquired.copy_from(static_cast<IUnknown*>(swapchain));

    // The callback clears the old content and sets the new. Only once it has
    // returned is the previous reference dropped, which is what keeps the
    // compositor from being left holding the last reference to an object mpv
    // is already tearing down.
    //
    // The presentation layer, not this one, is the authority on whether the
    // content actually attached: SetContent can refuse. Recording a refusal as
    // an attachment would report a live swap chain that is on nothing, and
    // would make the next identical notification a suppressed duplicate — so
    // a refusal leaves nothing attached, and a later reconfiguration or
    // observation republishes and tries again.
    const bool published = swapchain_callback_ ? swapchain_callback_(acquired.get()) : true;

    // The previous reference is dropped either way. The old content is cleared
    // before the new is set and that clear is committed even when the attach
    // fails, so the object has left the visual in both outcomes.
    held_swapchain_ = published ? std::move(acquired) : win::ComPtr<IUnknown>{};
    attached_ = published && incoming.present() ? incoming : core::SwapchainIdentity{};

    if (!published) {
        log::error("Composition tree refused the swap chain from {} (epoch {}); "
                   "nothing is attached", to_string(source), swapchain_epoch_);
    }
    if (published && transition == core::SwapchainTransition::Reattach) {
        ++diagnostics_.swapchain_reattachments;
        if (address_changed) ++diagnostics_.swapchain_replacements;
    }
    diagnostics_.swapchain_attached = attached_.present();
    diagnostics_.swapchain_epoch = swapchain_epoch_;
    diagnostics_.swapchain_acquisition = attached_.present() ? source
                                                             : SwapchainAcquisition::None;

    switch (published ? transition : core::SwapchainTransition::Ignore) {
        case core::SwapchainTransition::Attach:
            log::info("Composition swap chain attached via {} (epoch {})",
                      to_string(source), swapchain_epoch_);
            break;
        case core::SwapchainTransition::Reattach:
            // Only a real replacement is logged. The same-address case is
            // routine — a reconfiguration advances the epoch on every window
            // resize — and a line per occurrence would evict the history worth
            // having from a bounded log. Its count is in the diagnostics.
            if (address_changed) {
                log::info("Composition swap chain replaced via {} (epoch {}, replacement {})",
                          to_string(source), swapchain_epoch_,
                          diagnostics_.swapchain_replacements);
            }
            break;
        case core::SwapchainTransition::Detach:
            log::info("Composition swap chain detached (epoch {})", swapchain_epoch_);
            break;
        case core::SwapchainTransition::Ignore:
            break;
    }
}

void MpvPlayer::acquire_swapchain(SwapchainAcquisition source) {
    if (!mpv_) return;
    std::int64_t value = 0;
    const int status = mpv_get_property(mpv_, kDisplaySwapchainProperty,
                                        MPV_FORMAT_INT64, &value);
    if (status < 0) {
        // Unavailable means the video output does not exist yet, not that the
        // attachment is gone: the observation path is what reports a real
        // teardown. Leaving the attachment alone here avoids detaching a live
        // swap chain because a read raced the video output's creation, and is
        // the expected answer either side of one, so it is not worth a warning.
        if (status != MPV_ERROR_PROPERTY_UNAVAILABLE) {
            log::warn("display-swapchain read during {} failed: {}", to_string(source),
                      mpv_error_string(status));
        }
        return;
    }
    publish_swapchain(reinterpret_cast<void*>(static_cast<std::intptr_t>(value)), source);
}

void MpvPlayer::bump_swapchain_epoch() {
    ++swapchain_epoch_;
    diagnostics_.swapchain_epoch = swapchain_epoch_;
}

void MpvPlayer::detach_swapchain() {
    publish_swapchain(nullptr, SwapchainAcquisition::None);
    // A later address equal to the one just released belongs to a different
    // object, and the epoch is the only thing that can say so.
    bump_swapchain_epoch();
}

void MpvPlayer::handle_property(std::uint64_t id, const mpv_event_property& property) {
    auto optional_double = [&]() -> std::optional<double> {
        if (property.format == MPV_FORMAT_DOUBLE && property.data)
            return *static_cast<double*>(property.data);
        return std::nullopt;
    };
    switch (id) {
        case kDisplaySwapchain: {
            void* pointer = nullptr;
            if (property.format == MPV_FORMAT_INT64 && property.data) {
                pointer = reinterpret_cast<void*>(static_cast<std::intptr_t>(
                    *static_cast<std::int64_t*>(property.data)));
            }
            publish_swapchain(pointer, SwapchainAcquisition::PropertyObservation); break;
        }
        case kHwdecCurrent:
            diagnostics_.hwdec_active = property.format == MPV_FORMAT_STRING && property.data
                ? *static_cast<char**>(property.data) : ""; break;
        case kVideoCodec:
            diagnostics_.video_codec = property.format == MPV_FORMAT_STRING && property.data
                ? *static_cast<char**>(property.data) : ""; break;
        case kVideoWidth:
            diagnostics_.video_width = property.format == MPV_FORMAT_INT64 && property.data
                ? static_cast<int>(*static_cast<std::int64_t*>(property.data)) : 0; break;
        case kVideoHeight:
            diagnostics_.video_height = property.format == MPV_FORMAT_INT64 && property.data
                ? static_cast<int>(*static_cast<std::int64_t*>(property.data)) : 0; break;
        case kCoreIdle:
            diagnostics_.core_idle = property.format == MPV_FORMAT_FLAG && property.data &&
                *static_cast<int*>(property.data) != 0; break;
        case kPausedForCache:
            diagnostics_.paused_for_cache = property.format == MPV_FORMAT_FLAG && property.data &&
                *static_cast<int*>(property.data) != 0; break;
        case kCacheDuration: diagnostics_.cache_duration_seconds = optional_double(); break;
        case kCacheEnd: diagnostics_.cache_end_seconds = optional_double(); break;
        case kPlaybackTime: diagnostics_.playback_time_seconds = optional_double(); break;
        case kAvSync: diagnostics_.av_sync_seconds = optional_double(); break;
        case kCacheSpeed: diagnostics_.input_rate_bytes_per_second = optional_double(); break;
        case kEstimatedVfFps: diagnostics_.video_fps_estimate = optional_double(); break;
        case kContainerFps: diagnostics_.container_fps = optional_double(); break;
        default: break;
    }
}

core::PlaybackHealthObservation MpvPlayer::health_observation() const {
    return {.generation = target_ ? target_->generation : core::Generation{},
            .load_attempt = target_ ? target_->load_attempt : core::LoadAttempt{},
            .av_sync_seconds = diagnostics_.av_sync_seconds,
            .buffer_seconds = diagnostics_.cache_duration_seconds,
            .cache_end_seconds = diagnostics_.cache_end_seconds,
            .cache_paused = diagnostics_.paused_for_cache,
            .input_rate_bytes_per_second = diagnostics_.input_rate_bytes_per_second,
            .ipc_round_trip_ms = std::nullopt,
            .playback_time_seconds = diagnostics_.playback_time_seconds,
            .video_fps_estimate = diagnostics_.video_fps_estimate};
}

void MpvPlayer::pump() {
    if (!mpv_) return;
    for (;;) {
        mpv_event* event = mpv_wait_event(mpv_, 0.0);
        if (!event || event->event_id == MPV_EVENT_NONE) break;
        switch (event->event_id) {
            case MPV_EVENT_PROPERTY_CHANGE:
                handle_property(event->reply_userdata,
                    *static_cast<mpv_event_property*>(event->data)); break;
            case MPV_EVENT_COMMAND_REPLY:
                events_.command_result(event->reply_userdata, event->error);
                break;
            case MPV_EVENT_START_FILE: {
                const auto* start = static_cast<mpv_event_start_file*>(event->data);
                current_entry_id_ = start->playlist_entry_id;
                events_.start_file(start->playlist_entry_id);
                transport_log_armed_ = true;
                applied_filter_.clear();
                break;
            }
            case MPV_EVENT_FILE_LOADED:
                file_loaded_ = true;
                if (load_in_flight_) {
                    diagnostics_.last_load_seconds = std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - load_started_at_).count();
                    load_in_flight_ = false;
                    log::info("Channel ready in {:.2f}s", diagnostics_.last_load_seconds);
                }
                if (vsr_enabled_) set_vsr(true, vsr_scale_);
                break;
            case MPV_EVENT_PLAYBACK_RESTART:
                ++diagnostics_.mpv_playback_restart_events;
                if (current_entry_id_) events_.playback_restart(*current_entry_id_);
                break;
            case MPV_EVENT_VIDEO_RECONFIG:
                // The video output rebuilds its swap chain here, and the client
                // API only guarantees the *initial* property notification — a
                // later change may not notify at all. This is both the fallback
                // acquisition and the epoch boundary: a replacement allocated at
                // the address of the object it replaced is indistinguishable
                // from it without one.
                bump_swapchain_epoch();
                acquire_swapchain(SwapchainAcquisition::VideoReconfig);
                break;
            case MPV_EVENT_END_FILE: {
                const auto* end = static_cast<mpv_event_end_file*>(event->data);
                events_.end_file(end->playlist_entry_id, normalize_end_reason(end->reason),
                                 end->error, end->playlist_insert_id,
                                 end->playlist_insert_num_entries);
                if (end->reason == MPV_END_FILE_REASON_ERROR) {
                    log::error("Playback ended with structured error {}", end->error);
                }
                break;
            }
            case MPV_EVENT_LOG_MESSAGE: {
                const auto* message = static_cast<mpv_event_log_message*>(event->data);
                const std::optional<core::Generation> target_generation = target_
                    ? std::optional{target_->generation} : std::nullopt;
                const auto attribution = classify_engine_message_attribution(
                    transport_log_armed_, events_.active_generation(), target_generation);
                if (attribution == EngineMessageAttribution::ActiveEntry &&
                    !transport_classification_reported_ && target_) {
                    if (const auto classification = classify_transport_log(
                            message->text, target_->transport, file_loaded_,
                            target_->probed_format_forced)) {
                        transport_classification_reported_ = true;
                        if (std::holds_alternative<AuthenticationRejected>(*classification)) {
                            events_.authentication_rejected(target_->generation,
                                                            target_->load_attempt);
                        } else {
                            events_.transport_failure(
                                target_->generation, target_->load_attempt,
                                std::get<core::TransportFailureReason>(*classification));
                        }
                    }
                }
                // mpv warnings can embed authenticated stream URLs, headers
                // and query tokens. Preserve only closed categories; neither
                // prefix nor message text crosses this event turn.
                const auto warning = sanitize_engine_warning(
                    message->prefix, message->text, message->level);
                if (attribution == EngineMessageAttribution::ActiveEntry) {
                    diagnostics_.last_engine_message = warning;
                    ++diagnostics_.engine_message_count;
                } else {
                    diagnostics_.last_unattributed_engine_message = warning;
                    ++diagnostics_.unattributed_engine_message_count;
                }
                if (!engine_diagnostic_log_gate_.first_occurrence(attribution, warning)) break;

                const std::string generation =
                    attribution == EngineMessageAttribution::ActiveEntry && target_
                        ? std::format("{}", target_->generation.value())
                        : "unattributed";
                const std::string summary = std::format(
                    "mpv diagnostic generation={} attribution={} severity={} "
                    "component={} category={}",
                    generation, to_string(attribution), to_string(warning.severity),
                    to_string(warning.component), to_string(warning.category));
                if (warning.severity == EngineLogSeverity::Error ||
                    warning.severity == EngineLogSeverity::Fatal) log::error("{}", summary);
                else log::warn("{}", summary);
                break;
            }
            case MPV_EVENT_QUEUE_OVERFLOW:
                events_.backend_failed(target_ ? target_->generation : core::Generation{},
                                       target_ ? target_->load_attempt : core::LoadAttempt{},
                                       MPV_ERROR_EVENT_QUEUE_FULL);
                break;
            case MPV_EVENT_SHUTDOWN:
                events_.backend_failed(target_ ? target_->generation : core::Generation{},
                                       target_ ? target_->load_attempt : core::LoadAttempt{},
                                       MPV_ERROR_UNINITIALIZED);
                return;
            default: break;
        }
    }
}

}  // namespace coax::player
