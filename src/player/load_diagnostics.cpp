#include "player/load_diagnostics.hpp"

namespace coax::player {

const char* to_string(SwapchainAcquisition value) {
    switch (value) {
        case SwapchainAcquisition::None: return "none";
        case SwapchainAcquisition::PropertyObservation: return "property observation";
        case SwapchainAcquisition::VideoReconfig: return "video-reconfig read";
    }
    return "none";
}

void reset_load_observations(Diagnostics& diagnostics) {
    diagnostics.video_codec.clear();
    diagnostics.hwdec_active.clear();
    diagnostics.video_width = 0;
    diagnostics.video_height = 0;
    diagnostics.core_idle = false;
    diagnostics.paused_for_cache = false;
    diagnostics.last_load_seconds = 0.0;
    diagnostics.health_discontinuities = 0;
    diagnostics.buffer_phase = core::BufferPhase::Zap;
    diagnostics.buffer_phase_command_state = BufferPhaseCommandState::Unissued;
    diagnostics.av_sync_seconds.reset();
    diagnostics.cache_duration_seconds.reset();
    diagnostics.cache_end_seconds.reset();
    diagnostics.input_rate_bytes_per_second.reset();
    diagnostics.playback_time_seconds.reset();
    diagnostics.video_fps_estimate.reset();
    diagnostics.container_fps.reset();
}

}  // namespace coax::player
