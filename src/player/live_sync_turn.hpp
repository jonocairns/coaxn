#pragma once

#include <span>
#include <variant>

#include "core/playback_types.hpp"
#include "player/live_sync_gate.hpp"
#include "player/load_diagnostics.hpp"
#include "player/player_event_adapter.hpp"

namespace coax::player {

// One application turn's worth of live-sync input, assembled in the order the
// frame loop assembles it, plus the per-load lifecycle that decides what the
// turn is allowed to learn.
//
// This exists because the ordering is the defect. The gate's rules can be
// correct in isolation and still be defeated by how the application feeds them:
// process_player_events() sets the load's first-frame flag, and
// update_live_sync() then reads a pause property that was already true in the
// same turn. Nothing separates the two signals in time, so a fixture that
// supplies a pre-first-frame paused observation tests a sequence the runtime
// commonly does not produce. The captured 2026-08-07 session charged a rebuffer
// within a millisecond of first frame on 15 of 18 channel starts while every
// isolated gate case passed.
//
// The arm lifetime lives here rather than behind LiveSyncGate::reset(), which
// runs on a new channel or a new backend but deliberately not on a recovery
// reopen: the gate's pause state and the controller's learned target are meant
// to survive an episode, while a reopen's own fill must concede nothing. Those
// are opposite lifetimes, so they are separate flags with separate entry points.
class LiveSyncTurn {
public:
    // A new load, user-requested or a recovery reopen. Mirrors
    // App::begin_health_load(), which every load path reaches.
    void begin_load() {
        first_frame_seen_     = false;
        playback_established_ = false;
    }

    // A new channel or a new backend, alongside LiveSync::reset(). Recovery
    // reopens deliberately do not come through here.
    void reset() { gate_.reset(); }

    // The event drain, filtered by the exact active load identity just like
    // App::process_player_events(): a late frame belonging to a superseded
    // generation or replaced attempt says nothing about the one now playing.
    void observe_events(std::span<const PlayerEvent> events,
                        core::Generation active_generation,
                        core::LoadAttempt active_load_attempt) {
        for (const auto& event : events) {
            if (event.generation != active_generation ||
                event.load_attempt != active_load_attempt) continue;
            if (std::holds_alternative<FirstPlaybackStart>(event.payload)) {
                first_frame_seen_ = true;
            }
        }
    }

    // The supervisor confirming this load as steady: a first frame, then a
    // healthy window. That is the application's existing definition of playback
    // having actually started, and the only one the opening fill cannot
    // manufacture -- mpv publishes a first frame and a momentarily unpaused
    // turn while still filling, but it cannot publish five healthy seconds.
    void note_playback_established() { playback_established_ = true; }

    [[nodiscard]] bool first_frame_seen() const { return first_frame_seen_; }
    [[nodiscard]] bool playback_established() const { return playback_established_; }

    // The property snapshot, read after the events of the same turn.
    [[nodiscard]] LiveSyncStep observe(const Diagnostics& diagnostics) {
        return gate_.observe({.buffered_seconds     = diagnostics.cache_duration_seconds,
                              .paused_for_cache     = diagnostics.paused_for_cache,
                              .core_idle            = diagnostics.core_idle,
                              .playback_established = playback_established_});
    }

private:
    LiveSyncGate gate_;
    // Both per load. The first is what the health fold needs; the second is
    // what the gate needs, and it is strictly later.
    bool first_frame_seen_     = false;
    bool playback_established_ = false;
};

}  // namespace coax::player
