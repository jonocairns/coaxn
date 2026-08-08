#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "core/playback_types.hpp"
#include "player/buffer_phase_gate.hpp"
#include "player/playback_observability.hpp"

namespace coax::player {

// Which mpv path produced the current attachment. mpv's client API documents an
// initial property notification but warns that later changes may not always
// notify, so both paths exist; this records which one is actually doing the
// work, rather than assuming observation alone is sufficient.
enum class SwapchainAcquisition { None, PropertyObservation, VideoReconfig };

const char* to_string(SwapchainAcquisition value);

// Everything one load publishes for the diagnostics overlay and the controllers
// above it. This lives away from mpv_player.hpp deliberately: the adapter owns
// an instance, but the type itself names no mpv and no Windows type, so the
// portable half of the player layer can read and reset it without depending on
// a header that only compiles natively because IUnknown is forward-declared.
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
    // Only messages observed after START_FILE for the adapter-confirmed active
    // target enter this bucket. Identity-less messages in a replacement window
    // stay visible below instead of contaminating the new load's evidence.
    std::uint64_t engine_message_count = 0;
    std::optional<SanitizedEngineWarning> last_engine_message;
    std::uint64_t unattributed_engine_message_count = 0;
    std::optional<SanitizedEngineWarning> last_unattributed_engine_message;
    std::optional<SanitizedRequestShape> request_shape;

    core::BufferPhase buffer_phase = core::BufferPhase::Zap;
    BufferPhaseCommandState buffer_phase_command_state =
        BufferPhaseCommandState::Unissued;
    int buffer_commands_accepted = 0;
    int buffer_commands_rejected = 0;

    std::optional<double> av_sync_seconds;
    // mpv documents demuxer-cache-duration as an approximate guess that is
    // often unavailable even while data is buffered, so the absence is carried
    // rather than flattened to a number. A reader that wants a double has to
    // decide what an absent reading means for it; there is deliberately no
    // zero here to fall into, because zero is also a real measurement.
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

}  // namespace coax::player
