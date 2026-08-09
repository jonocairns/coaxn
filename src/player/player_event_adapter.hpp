#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <unordered_map>
#include <variant>
#include <vector>

#include "core/playback_types.hpp"
#include "core/supervisor.hpp"

namespace coax::player {

enum class PlayerEndReason { Eof, Stop, Quit, Error, Redirect, Unknown };
enum class BufferProperty { CacheSeconds, ReadaheadSeconds };
enum class IntentionalStopKind { Requested, Replaced };

struct LoadCommandResult {
    std::uint64_t request_id;
    bool accepted;
    int error;
};
struct FirstPlaybackStart {};
struct EndFileEvent {
    PlayerEndReason reason;
    int error;
};
struct PlaybackStopped {
    IntentionalStopKind kind;
};
struct BackendFailed {
    int error;
};
struct PlayerAuthenticationRejected {};
struct TransportFailureDetected { core::TransportFailureReason reason; };
struct PropertyCommandResult {
    std::uint64_t request_id;
    core::BufferPhase phase;
    BufferProperty property;
    bool accepted;
    int error;
};

using PlayerEventPayload = std::variant<LoadCommandResult, FirstPlaybackStart,
                                        EndFileEvent, PlaybackStopped,
                                        BackendFailed, PropertyCommandResult,
                                        PlayerAuthenticationRejected,
                                        TransportFailureDetected>;
struct PlayerEvent {
    core::Generation generation;
    core::LoadAttempt load_attempt;
    PlayerEventPayload payload;
};

// Lossless edge journal and correlation model kept separate from the mpv API
// so its ordering and generation fences can be contract-tested with no media.
class PlayerEventAdapter {
public:
    void track_load(std::uint64_t request_id, core::Generation generation,
                    core::LoadAttempt load_attempt);
    void track_property(std::uint64_t request_id, core::Generation generation,
                        core::BufferPhase phase, BufferProperty property);
    void command_result(std::uint64_t request_id, int error);
    void command_rejected_immediately(std::uint64_t request_id, int error);

    void start_file(std::int64_t playlist_entry_id);
    void playback_restart(std::int64_t playlist_entry_id);
    void intentional_stop(std::int64_t playlist_entry_id, core::Generation report_as,
                          IntentionalStopKind kind);
    void end_file(std::int64_t playlist_entry_id, PlayerEndReason reason, int error,
                  std::int64_t playlist_insert_id = 0,
                  int playlist_insert_num_entries = 0);
    void backend_failed(core::Generation generation, core::LoadAttempt load_attempt, int error);
    void authentication_rejected(core::Generation generation, core::LoadAttempt load_attempt);
    void transport_failure(core::Generation generation, core::LoadAttempt load_attempt,
                           core::TransportFailureReason reason);
    void dispose();

    [[nodiscard]] std::vector<PlayerEvent> drain();
    [[nodiscard]] std::optional<core::Generation> active_generation() const;
    [[nodiscard]] std::optional<core::LoadAttempt> active_load_attempt() const;
    [[nodiscard]] std::optional<std::int64_t> active_entry() const { return active_entry_; }

private:
    struct LoadIdentity {
        core::Generation generation;
        core::LoadAttempt load_attempt;
    };
    struct PendingLoad { std::uint64_t request_id; LoadIdentity identity; };
    struct PropertyRequest {
        core::Generation generation;
        core::BufferPhase phase;
        BufferProperty property;
    };
    struct StopIntent { core::Generation report_as; IntentionalStopKind kind; };

    void remove_pending_load(std::uint64_t request_id);
    void clear_correlations();

    std::deque<PendingLoad> pending_loads_;
    std::unordered_map<std::uint64_t, LoadIdentity> load_requests_;
    std::unordered_map<std::uint64_t, PropertyRequest> property_requests_;
    std::unordered_map<std::int64_t, LoadIdentity> entries_;
    std::unordered_map<std::int64_t, StopIntent> stop_intents_;
    std::unordered_map<std::int64_t, bool> first_started_;
    std::optional<std::int64_t> active_entry_;
    bool backend_failure_reported_ = false;
    std::vector<PlayerEvent> events_;
};

}  // namespace coax::player
