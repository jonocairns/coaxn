#pragma once

#include <optional>

namespace coax::player {

// Tuning for the live-offset controller. Defaults mirror ExoPlayer's
// DefaultLivePlaybackSpeedControl so behaviour is comparable to a known-good
// implementation rather than invented.
struct LiveSyncConfig {
    // ExoPlayer: DEFAULT_FALLBACK_MIN/MAX_PLAYBACK_SPEED.
    double min_speed = 0.97;
    double max_speed = 1.03;

    // ExoPlayer: DEFAULT_PROPORTIONAL_CONTROL_FACTOR.
    double proportional_control_factor = 0.1;

    // ExoPlayer: DEFAULT_MAX_LIVE_OFFSET_ERROR_MS_FOR_UNIT_SPEED (20ms).
    // Inside this band the speed is held at exactly 1.0 so the controller does
    // not chase noise.
    double deadband_seconds = 0.020;

    // ExoPlayer: DEFAULT_MIN_UPDATE_INTERVAL_MS (1000ms).
    double min_update_interval_seconds = 1.0;

    // ExoPlayer: DEFAULT_TARGET_LIVE_OFFSET_INCREMENT_ON_REBUFFER_MS (500ms).
    // Each rebuffer concedes a little more latency; the controller then holds
    // the new target rather than drifting further.
    double rebuffer_increment_seconds = 0.5;

    double initial_target_offset_seconds = 4.0;
    double min_target_offset_seconds     = 1.0;
    double max_target_offset_seconds     = 30.0;
};

// Keeps playback near a target distance behind the live edge by nudging the
// playback speed, rather than by seeking -- a raw TS live stream cannot seek,
// so speed is the only control surface available.
//
// A port of ExoPlayer's DefaultLivePlaybackSpeedControl, with one deliberate
// difference: ExoPlayer learns the true live offset from an HLS or DASH
// manifest. A plain TS stream carries no manifest, so this uses the demuxer's
// buffered duration as a proxy. It tracks the same quantity in practice but
// will drift where a manifest-driven implementation would not.
class LiveSync {
public:
    explicit LiveSync(LiveSyncConfig config = {});

    // Starts a new channel: target returns to the initial offset and the speed
    // to 1.0. Learned latency is deliberately not carried across channels.
    void reset();

    // Called on the rising edge of a rebuffer. Concedes latency by pushing the
    // target further from the live edge, bounded by max_target_offset_seconds.
    void notify_rebuffer();

    // Returns a new playback speed when one is due, or nullopt when the update
    // interval has not elapsed or the speed is unchanged.
    std::optional<double> update(double buffered_seconds, double now_seconds);

    [[nodiscard]] double target_offset_seconds() const { return target_offset_seconds_; }
    [[nodiscard]] double speed() const { return speed_; }

private:
    LiveSyncConfig config_;
    double         target_offset_seconds_;
    double         speed_          = 1.0;
    double         last_update_at_ = 0.0;
    bool           has_updated_    = false;
};

}  // namespace coax::player
