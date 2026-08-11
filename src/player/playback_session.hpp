#pragma once

#include <cstdint>
#include <exception>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "core/playback_health.hpp"
#include "core/supervisor_host.hpp"
#include "player/live_sync.hpp"
#include "player/live_sync_turn.hpp"
#include "player/load_diagnostics.hpp"
#include "player/playback_observability.hpp"
#include "player/player_event_adapter.hpp"

namespace coax::player {

struct ActiveLoad {
    core::Generation generation;
    core::LoadAttempt load_attempt;
};

struct HealthSampleReport {
    core::PlaybackHealthFold fold;
    core::Generation observed_generation;
    TimelineClassification classification = TimelineClassification::Unavailable;
    std::uint64_t engine_messages_since_sample = 0;
    std::uint64_t unattributed_engine_messages_since_sample = 0;
    std::optional<SanitizedEngineWarning> engine_warning;
};

// The concrete player remains outside the portable coordinator. These callbacks
// are deliberately narrow: reads are snapshots of the current engine state and
// writes are the typed commands produced by the session protocol.
struct PlaybackSessionCallbacks {
    std::function<std::optional<ActiveLoad>()> active_load;
    std::function<const Diagnostics&()> diagnostics;
    std::function<core::PlaybackHealthObservation()> health_observation;

    std::function<std::optional<core::RecoveryTransport>(
        const core::SupervisorEffect&)> execute_recovery;
    // Reports exceptions without coupling the portable session to the
    // application's logger. The session still settles the recovery if this
    // observer is missing or itself throws.
    std::function<void(const core::SupervisorEffect&, std::exception_ptr)>
        on_recovery_exception;
    // Runs after a successful player-recreation reset has synchronized the
    // session's live-sync state with the fresh backend.
    std::function<void()> restore_backend_settings;
    std::function<void(core::Generation, core::BufferPhase)> apply_buffer_phase;
    std::function<void(double)> set_speed;
    std::function<void(double, int)> set_live_sync_state;
    std::function<void(int)> set_health_discontinuities;

    std::function<void(const PlayerEvent&)> on_player_event;
    std::function<void(const core::SupervisorState&, core::SupervisorStateName)> on_state_changed;
    std::function<void(const core::SupervisorTransition&)> on_transition;
    std::function<void(const HealthSampleReport&)> on_health_sample;
    std::function<void(int, double)> on_rebuffer;
    std::function<void(double)> on_unity_speed;
};

// Owns the correctness-sensitive playback protocol. App pumps mpv and hands
// the resulting edge journal to service_turn(); portable integration tests do
// exactly the same with a fake telemetry/action boundary.
class PlaybackSession {
public:
    PlaybackSession(const core::SupervisorClock& clock,
                    PlaybackSessionCallbacks callbacks,
                    core::RecoveryPolicy policy = core::kDefaultRecoveryPolicy);

    [[nodiscard]] core::Generation begin_channel();
    void load_started(core::LoadAttempt load_attempt, core::LoadIntent intent,
                      core::RecoveryTransport transport);
    void load_failed(core::LoadAttempt load_attempt);

    void service_turn(std::span<const PlayerEvent> events,
                      const std::function<void()>& after_events = {});
    void presentation_lost();
    void dispose();

    [[nodiscard]] core::Generation generation() const { return generation_; }
    [[nodiscard]] const core::SupervisorState& state() const { return supervisor_.current(); }
    [[nodiscard]] std::optional<core::TimePoint> armed_deadline() const {
        return supervisor_.armed_deadline();
    }
    [[nodiscard]] const core::BufferHealthSnapshot& health_snapshot() const {
        return health_snapshot_;
    }
    [[nodiscard]] TimelineClassification timeline_classification() const {
        return timeline_classification_;
    }
    [[nodiscard]] double live_target_seconds() const {
        return live_sync_.target_offset_seconds();
    }

private:
    struct PendingStreamEnd {
        core::Generation generation;
        core::LoadAttempt load_attempt;
        core::EndReason reason;
        core::TimePoint dispatch_at;
    };

    void restart_health_supervision(core::LoadAttempt load_attempt);
    void process_events(std::span<const PlayerEvent> events);
    void flush_pending_stream_ends();
    void dispatch_cache_state();
    void sample_health();
    void update_live_sync();
    void on_supervisor_state_changed(const core::SupervisorState& state);
    void execute_recovery(const core::SupervisorEffect& effect);
    // Called only after a successful player recreation. Ordinary recovery
    // reopens deliberately retain the live target and speed-control history.
    void backend_recreated();
    void complete_recovery(const core::SupervisorEffect& effect,
                           std::optional<core::RecoveryTransport> transport);
    [[nodiscard]] const Diagnostics& diagnostics() const;

    const core::SupervisorClock& clock_;
    PlaybackSessionCallbacks callbacks_;
    core::PlaybackSupervisor supervisor_;
    core::SupervisorStateName supervisor_state_name_ = core::SupervisorStateName::Idle;

    core::Generation generation_;
    std::optional<core::PlaybackHealthState> playback_health_;
    core::BufferHealthSnapshot health_snapshot_;
    TimelineClassification timeline_classification_ = TimelineClassification::Unavailable;
    core::TimePoint next_health_sample_{};
    bool stall_reported_ = false;
    bool decode_stall_reported_ = false;
    bool exact_failure_reported_ = false;
    std::uint64_t last_health_engine_message_count_ = 0;
    std::uint64_t last_health_unattributed_engine_message_count_ = 0;
    std::optional<bool> last_cache_state_dispatched_;
    std::vector<PendingStreamEnd> pending_stream_ends_;

    LiveSync live_sync_;
    LiveSyncTurn live_sync_turn_;
    int rebuffer_count_ = 0;
};

}  // namespace coax::player
