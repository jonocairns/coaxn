#pragma once

#include <optional>

namespace coax::player {

// What the engine is reporting this turn, as the live-offset controller needs
// to see it.
struct LiveSyncSample {
    // mpv's demuxer-cache-duration. Absent means mpv could not report it, which
    // is a different reading from zero buffered seconds and must not be
    // controlled on -- see LiveSyncGate::observe.
    std::optional<double> buffered_seconds;
    bool paused_for_cache = false;
    bool core_idle = false;
    // Whether the supervisor has confirmed this load as steady -- a first frame
    // followed by a healthy window. Deliberately not "has a frame been seen":
    // see LiveSyncGate::observe for what that weaker reading cannot separate.
    bool playback_established = false;
};

// What the caller should do with the controller this turn. At most one of
// hold_unity_speed and control_input is set; rebuffered is independent of both,
// because conceding latency is a decision about the target rather than the
// speed.
struct LiveSyncStep {
    bool rebuffered = false;
    bool hold_unity_speed = false;
    std::optional<double> control_input;
};

// Decides whether a live-sync turn may learn from what it was handed. This is
// protocol rather than view, so it lives beside the controller it guards and is
// tested with it, instead of inside the application's frame tick.
//
// Two engine behaviours make it necessary. mpv publishes the same
// paused-for-cache state for an initial fill as for an underrun, because
// cache-pause-initial=yes shares the low-cache path; and it documents
// demuxer-cache-duration as often unavailable, which arrives as an absent
// reading rather than an error.
class LiveSyncGate {
public:
    // Starts a new channel or a new backend, alongside LiveSync::reset(). A
    // pause edge belongs to the load that produced it, so the previous load's
    // state is dropped rather than carried into the next one.
    //
    // There is deliberately no arm state to clear here. Recovery reopens keep
    // the learned pause and controller state and never call this, but they are
    // new loads and must not concede for their own fill; arming therefore
    // travels in the sample, from a reading the reopen resets.
    void reset() { was_paused_for_cache_ = false; }

    LiveSyncStep observe(const LiveSyncSample& sample) {
        LiveSyncStep step;

        // Only the transition into a stall counts, and only once the load is
        // established. The flag stays true for the whole stall, so an edge test
        // avoids conceding latency once per frame.
        //
        // Two weaker readings of "established" were tried against this runtime
        // and both failed. "A frame has been seen" fails because the
        // application reports both signals as of one turn -- its event drain
        // sets the flag before it reads the pause property -- and the
        // initial-fill pause edge commonly coincides with playback start.
        // "A frame, plus a turn that is not filling" fails because mpv
        // publishes exactly one such turn as playback begins and then re-enters
        // the same opening fill a millisecond later, which is a rising edge
        // against a state that reads as established. Only the supervisor's
        // steady confirmation -- a first frame plus a healthy window -- is a
        // reading the opening fill cannot manufacture.
        step.rebuffered =
            sample.paused_for_cache && !was_paused_for_cache_ && sample.playback_established;
        was_paused_for_cache_ = sample.paused_for_cache;

        // A draining or idle cache is not a valid timing measurement. Do not
        // have the controller chase it, and take any installed correction off
        // while playback is unable to make normal forward progress.
        if (sample.paused_for_cache || sample.core_idle) {
            step.hold_unity_speed = true;
            return step;
        }

        // Missing telemetry read as zero is a positive error against a positive
        // target, which installs the minimum speed and accumulates latency for
        // as long as the property stays away. Unity is the only safe speed to
        // hold without a measurement.
        if (!sample.buffered_seconds) {
            step.hold_unity_speed = true;
            return step;
        }

        step.control_input = sample.buffered_seconds;
        return step;
    }

private:
    bool was_paused_for_cache_ = false;
};

}  // namespace coax::player
