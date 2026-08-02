#include "player/mpv_player.hpp"

#include <mpv/client.h>

#include <format>

#include "util/log.hpp"

namespace coax::player {
namespace {

// Properties observed for diagnostics and for swap-chain acquisition. The
// swap chain is observed rather than polled because polling after
// mpv_initialize races video-output configuration: the property reports
// unavailable until the VO exists.
enum ObserveId : uint64_t {
    kDisplaySwapchain = 1,
    kHwdecCurrent,
    kVideoCodec,
    kVideoWidth,
    kVideoHeight,
    kCoreIdle,
    kPausedForCache,
    kCacheDuration,
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

}  // namespace

MpvPlayer::~MpvPlayer() {
    if (mpv_) {
        mpv_terminate_destroy(mpv_);
        mpv_ = nullptr;
    }
}

bool MpvPlayer::initialize(const PlayerConfig& config, std::string& error) {
    mpv_ = mpv_create();
    if (!mpv_) {
        error = "mpv_create failed";
        return false;
    }

    // Presentation: mpv renders into a composition swap chain it does not
    // present to a window of its own. Coax attaches that swap chain to its own
    // DirectComposition visual so video and UI share one top-level surface.
    set_option(mpv_, "vo", "gpu-next");
    set_option(mpv_, "gpu-api", "d3d11");
    set_option(mpv_, "d3d11-output-mode", "composition");
    set_option(mpv_, "d3d11-composition-size",
               composition_size(config.composition_width, config.composition_height));

    diagnostics_.hwdec_requested = config.hardware_decode ? "d3d11va" : "no";
    set_option(mpv_, "hwdec", diagnostics_.hwdec_requested);

    // --- Buffering -------------------------------------------------------
    //
    // Capacity and latency are separate concerns. A large cache is what
    // absorbs a network stall; how far behind live we actually sit is held by
    // the live-offset controller, not by these numbers. ExoPlayer draws the
    // same distinction between DefaultLoadControl's 50s buffer and the much
    // smaller target live offset.
    // Probe limits are only applied when explicitly asked for. Truncating
    // analysis trades tune-in time for channels that never play at all.
    if (config.analyze_duration_seconds > 0.0) {
        set_option(mpv_, "demuxer-lavf-analyzeduration",
                   std::format("{:.2f}", config.analyze_duration_seconds));
    }
    if (config.probe_size_bytes > 0) {
        set_option(mpv_, "demuxer-lavf-probesize", std::to_string(config.probe_size_bytes));
    }

    set_option(mpv_, "cache", "yes");

    // A ceiling on the time targets below, not a target itself. The demuxer
    // reads it once at creation, so it can never become a per-phase value.
    // 64MiB is the figure qualified against real provider streams in the
    // Electron implementation; the 400MiB this previously used was guesswork
    // and accounted for most of the process working set.
    set_option(mpv_, "demuxer-max-bytes", "64MiB");
    set_option(mpv_, "demuxer-max-back-bytes", "16MiB");

    // Single-phase for now. The qualified design phases this: ~1s while tuning
    // in so the opening read burst is small, then ~10s once playing. Phasing
    // needs the supervisor to drive the transition, so until then this is one
    // conservative value biased towards absorbing stalls.
    set_option(mpv_, "demuxer-readahead-secs", "10");
    set_option(mpv_, "cache-secs", "10");

    // Rebuffer thresholds, mirroring DefaultLoadControl:
    // BUFFER_FOR_PLAYBACK_MS = 1s, BUFFER_FOR_PLAYBACK_AFTER_REBUFFER_MS = 2s.
    // Pausing to refill beats stuttering through a dry cache.
    set_option(mpv_, "cache-pause", "yes");
    set_option(mpv_, "cache-pause-initial", "yes");
    set_option(mpv_, "cache-pause-wait", "2");

    // --- Transport recovery ----------------------------------------------
    //
    // Deliberately off by default for live streams.
    //
    // FFmpeg's reconnect works by re-opening the URL and resuming at a byte
    // offset. A live TS stream is not seekable, so there is no correct resume
    // point: the server replies from the head of its own buffer and playback
    // replays content it has already shown. The failure is invisible to every
    // signal we watch, because the cache never runs dry and paused-for-cache
    // never fires.
    //
    // A supervisor reloading the channel at the live edge is the correct
    // recovery mechanism for this stream type. Left switchable so the two can
    // be compared against a real provider rather than argued about.
    if (config.transport_reconnect) {
        set_option(mpv_, "stream-lavf-o",
                   "reconnect=1,"
                   "reconnect_on_network_error=1,"
                   "reconnect_delay_max=5");
    }
    set_option(mpv_, "network-timeout", "20");

    // Small speed changes must not shift pitch, or the live-offset controller
    // would be audible.
    set_option(mpv_, "audio-pitch-correction", "yes");

    // Appliance behaviour: stay alive between channels, never take over the
    // terminal, and keep the live edge rather than buffering for latency.
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
        mpv_terminate_destroy(mpv_);
        mpv_ = nullptr;
        return false;
    }

    // Without this, MPV_EVENT_LOG_MESSAGE is never delivered and mpv's own
    // diagnostics -- including FFmpeg demuxer and protocol warnings -- are
    // silently discarded.
    mpv_request_log_messages(mpv_, "warn");

    mpv_observe_property(mpv_, kDisplaySwapchain, "display-swapchain", MPV_FORMAT_INT64);
    mpv_observe_property(mpv_, kHwdecCurrent,     "hwdec-current",     MPV_FORMAT_STRING);
    mpv_observe_property(mpv_, kVideoCodec,       "video-codec",       MPV_FORMAT_STRING);
    mpv_observe_property(mpv_, kVideoWidth,       "width",             MPV_FORMAT_INT64);
    mpv_observe_property(mpv_, kVideoHeight,      "height",            MPV_FORMAT_INT64);
    mpv_observe_property(mpv_, kCoreIdle,         "core-idle",         MPV_FORMAT_FLAG);
    mpv_observe_property(mpv_, kPausedForCache,   "paused-for-cache",  MPV_FORMAT_FLAG);
    mpv_observe_property(mpv_, kCacheDuration,    "demuxer-cache-duration", MPV_FORMAT_DOUBLE);

    log::info("libmpv initialized (client API {}.{})",
              mpv_client_api_version() >> 16, mpv_client_api_version() & 0xFFFF);
    return true;
}

void MpvPlayer::play(const std::string& url) {
    if (!mpv_) {
        return;
    }
    // The authenticated URL is passed through the client API only; it is never
    // written to the log or shown in the UI.
    log::info("Loading {}", log::redact_stream_url(url));

    load_started_at_    = std::chrono::steady_clock::now();
    load_in_flight_     = true;
    first_restart_seen_ = false;

    const char* command[] = {"loadfile", url.c_str(), "replace", nullptr};
    const int   status    = mpv_command_async(mpv_, 0, command);
    if (status < 0) {
        log::error("loadfile failed: {}", mpv_error_string(status));
        load_in_flight_ = false;
    }
}

void MpvPlayer::stop() {
    if (!mpv_) {
        return;
    }
    const char* command[] = {"stop", nullptr};
    mpv_command_async(mpv_, 0, command);
}

void MpvPlayer::set_composition_size(int width, int height) {
    if (!mpv_) {
        return;
    }
    const std::string value = composition_size(width, height);
    mpv_set_property_string(mpv_, "d3d11-composition-size", value.c_str());
}

void MpvPlayer::set_vsr(bool enabled, double scale) {
    if (!mpv_) {
        return;
    }
    vsr_enabled_ = enabled;
    vsr_scale_   = scale;

    // scaling-mode=nvidia selects NVIDIA RTX Video Super Resolution inside the
    // D3D11 video processor. Attachment is not activation: the driver may
    // decline for the source format, resolution or driver configuration, and
    // mpv reports no signal that would let us claim otherwise.
    const std::string filter =
        enabled ? std::format("d3d11vpp=scaling-mode=nvidia:scale={:.3f}", scale)
                : std::string{};

    // Assigning "vf" rebuilds the filter graph and reconfigures the video
    // chain even when the value is unchanged, which interrupts decoding.
    // Only touch it when the graph would actually differ.
    if (filter == applied_filter_) {
        return;
    }
    applied_filter_ = filter;

    const int status = mpv_set_property_string(mpv_, "vf", filter.c_str());
    diagnostics_.vsr_requested       = enabled;
    diagnostics_.vsr_filter_attached = enabled && status >= 0;

    if (status < 0) {
        log::warn("Setting video filter failed: {}", mpv_error_string(status));
    } else {
        log::info("Video filter set to '{}'", filter.empty() ? "(none)" : filter);
    }
}

void MpvPlayer::set_volume(int percent) {
    if (!mpv_) {
        return;
    }
    const std::string value = std::to_string(percent);
    mpv_set_property_string(mpv_, "volume", value.c_str());
}

void MpvPlayer::set_speed(double speed) {
    if (!mpv_) {
        return;
    }
    const std::string value = std::format("{:.4f}", speed);
    mpv_set_property_string(mpv_, "speed", value.c_str());
    diagnostics_.playback_speed = speed;
}

void MpvPlayer::set_live_sync_state(double target_seconds, int rebuffer_count) {
    diagnostics_.live_target_seconds = target_seconds;
    diagnostics_.rebuffer_count      = rebuffer_count;
}

void MpvPlayer::set_paused(bool paused) {
    if (!mpv_) {
        return;
    }
    mpv_set_property_string(mpv_, "pause", paused ? "yes" : "no");
}

void MpvPlayer::publish_swapchain(void* swapchain) {
    if (swapchain == swapchain_) {
        return;
    }
    swapchain_ = swapchain;
    diagnostics_.swapchain_state = swapchain ? "attached" : "none";

    log::info("Composition swap chain {}", swapchain ? "available" : "released");
    if (swapchain_callback_) {
        swapchain_callback_(swapchain);
    }
}

void MpvPlayer::handle_property(uint64_t observe_id, const mpv_event_property& property) {
    switch (observe_id) {
        case kDisplaySwapchain: {
            // The property carries the IDXGISwapChain address as an int64.
            // Treated as a borrowed pointer: DirectComposition takes its own
            // reference when the visual content is set.
            void* pointer = nullptr;
            if (property.format == MPV_FORMAT_INT64 && property.data) {
                pointer = reinterpret_cast<void*>(
                    static_cast<intptr_t>(*static_cast<int64_t*>(property.data)));
            }
            publish_swapchain(pointer);
            break;
        }
        case kHwdecCurrent:
            if (property.format == MPV_FORMAT_STRING && property.data) {
                diagnostics_.hwdec_active = *static_cast<char**>(property.data);
            }
            break;
        case kVideoCodec:
            if (property.format == MPV_FORMAT_STRING && property.data) {
                diagnostics_.video_codec = *static_cast<char**>(property.data);
            }
            break;
        case kVideoWidth:
            if (property.format == MPV_FORMAT_INT64 && property.data) {
                diagnostics_.video_width = static_cast<int>(*static_cast<int64_t*>(property.data));
            }
            break;
        case kVideoHeight:
            if (property.format == MPV_FORMAT_INT64 && property.data) {
                diagnostics_.video_height = static_cast<int>(*static_cast<int64_t*>(property.data));
            }
            break;
        case kCoreIdle:
            if (property.format == MPV_FORMAT_FLAG && property.data) {
                diagnostics_.core_idle = *static_cast<int*>(property.data) != 0;
            }
            break;
        case kPausedForCache:
            if (property.format == MPV_FORMAT_FLAG && property.data) {
                diagnostics_.paused_for_cache = *static_cast<int*>(property.data) != 0;
            }
            break;
        case kCacheDuration:
            if (property.format == MPV_FORMAT_DOUBLE && property.data) {
                diagnostics_.cache_seconds = *static_cast<double*>(property.data);
            }
            break;
        default:
            break;
    }
}

void MpvPlayer::pump() {
    if (!mpv_) {
        return;
    }

    for (;;) {
        mpv_event* event = mpv_wait_event(mpv_, 0.0);
        if (!event || event->event_id == MPV_EVENT_NONE) {
            break;
        }

        switch (event->event_id) {
            case MPV_EVENT_PROPERTY_CHANGE:
                handle_property(event->reply_userdata,
                                *static_cast<mpv_event_property*>(event->data));
                break;

            case MPV_EVENT_LOG_MESSAGE: {
                const auto* message = static_cast<mpv_event_log_message*>(event->data);
                std::string text(message->text);
                while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
                    text.pop_back();
                }
                log::warn("mpv/{}: {}", message->prefix, text);
                break;
            }

            case MPV_EVENT_END_FILE: {
                const auto* end = static_cast<mpv_event_end_file*>(event->data);
                if (end->reason == MPV_END_FILE_REASON_ERROR) {
                    log::error("Playback ended with error: {}", mpv_error_string(end->error));
                }
                break;
            }

            case MPV_EVENT_PLAYBACK_RESTART:
                // Fires once when playback begins, and again whenever mpv has
                // to resynchronise -- which on this provider means a timeline
                // discontinuity in the stream. Only the later ones count.
                if (first_restart_seen_) {
                    ++diagnostics_.discontinuities;
                    log::warn("Stream discontinuity #{} (mpv resynchronised)",
                              diagnostics_.discontinuities);
                } else {
                    first_restart_seen_ = true;
                }
                break;

            case MPV_EVENT_FILE_LOADED:
                if (load_in_flight_) {
                    diagnostics_.last_load_seconds =
                        std::chrono::duration<double>(
                            std::chrono::steady_clock::now() - load_started_at_).count();
                    load_in_flight_ = false;
                    log::info("Channel ready in {:.2f}s", diagnostics_.last_load_seconds);
                }
                log::info("File loaded");
                // mpv clears the graph between files, so the cached value no
                // longer reflects reality and the filter must be reassigned.
                applied_filter_.clear();
                if (vsr_enabled_) {
                    set_vsr(true, vsr_scale_);
                }
                break;

            case MPV_EVENT_SHUTDOWN:
                log::warn("mpv requested shutdown");
                return;

            default:
                break;
        }
    }
}

}  // namespace coax::player
