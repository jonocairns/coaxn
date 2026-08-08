#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "core/playback_types.hpp"
#include "core/policy.hpp"

namespace coax::core {

enum class SupervisorStateName { Idle, Loading, Zap, Steady, Recovering, Failed };
enum class RecoveryAction { ReopenStream, ReloadHlsLive, ReopenProbed, RecreatePlayer };
enum class TransportFailureReason {
    FormatProbeRequired,
    HlsPlaylistFailed,
    HlsSegmentUnavailable,
    HttpRequestTimeout,
};
enum class DetectionReason {
    DecodeStall,
    FormatProbeRequired,
    HlsPlaylistFailed,
    HlsSegmentUnavailable,
    HttpRequestTimeout,
    IpcUnresponsive,
    OpenStall,
    PresentationDeviceLost,
    ProcessExited,
    ProgressStall,
    StreamEndedEof,
    StreamEndedError,
    StreamEndedUnknown,
};
enum class FailureReason { AttemptsExhausted, AuthRejected, BudgetExpired, SourceUnavailable };
enum class EndReason { Eof, Error, Unknown };
enum class StallKind { Open, Progress };

struct SupervisorDeadlines {
    std::optional<TimePoint> retry_at;
    std::optional<TimePoint> steady_at;
};

struct SupervisorState {
    std::size_t attempt = 0;
    bool cache_paused = false;
    // The last determinate health level, not an interruption edge. Unknown
    // telemetry does not set or clear it, and a new load starts neutral.
    bool playback_unhealthy = false;
    SupervisorDeadlines deadlines;
    std::optional<DetectionReason> detection;
    std::optional<FailureReason> failure;
    std::optional<TimePoint> first_frame_at;
    Generation generation;
    SupervisorStateName name = SupervisorStateName::Idle;
    std::optional<RecoveryAction> recovery;
    std::optional<TimePoint> recovery_started_at;
    std::optional<RecoveryTransport> transport;
};

struct DeadlineReached {};
struct CacheState { Generation generation; bool paused; };
struct AuthRejected { Generation generation; };
struct DecodeStalled { Generation generation; };
struct FirstFrame { Generation generation; };
struct IpcUnresponsive { Generation generation; };
struct ChannelRequested { Generation generation; };
struct PlaybackHealthObserved { Generation generation; bool unhealthy; };
struct PlaybackStopped { Generation generation; };
// The graphics device the video is presented through went away. mpv's own
// device dies with it, so the stream has to be reopened on a rebuilt one —
// which is what the player-recreation action already does. Routing it here
// rather than recovering beside the supervisor is what gives presentation loss
// the same bounded attempts and the same generation fence as every other fault.
struct PresentationLost { Generation generation; };
struct ProcessExited { Generation generation; };
struct SourceFailed { Generation generation; };
struct PlaybackStalled { Generation generation; StallKind stall; };
struct StreamEnded {
    Generation generation;
    EndReason end_reason;
    std::optional<TransportFailureReason> failure_reason;
};
struct StreamLoadIssued { Generation generation; RecoveryTransport transport; };

using SupervisorEvent = std::variant<
    DeadlineReached, CacheState, AuthRejected, DecodeStalled, FirstFrame,
    IpcUnresponsive, ChannelRequested, PlaybackHealthObserved, PlaybackStopped,
    PresentationLost, ProcessExited, SourceFailed, PlaybackStalled, StreamEnded,
    StreamLoadIssued>;

struct ReopenStream {};
struct ReloadHlsLive {};
struct ReopenProbed {};
struct RecreatePlayer {};
using SupervisorEffectPayload = std::variant<ReopenStream, ReloadHlsLive, ReopenProbed,
                                             RecreatePlayer>;
struct SupervisorEffect {
    Generation generation;
    SupervisorEffectPayload payload;
};

struct SupervisorTransition {
    std::size_t attempt = 0;
    Duration elapsed_budget{};
    SupervisorStateName from = SupervisorStateName::Idle;
    Generation generation;
    std::string_view policy_version;
    std::string reason;
    SupervisorStateName to = SupervisorStateName::Idle;
    std::optional<RecoveryTransport> transport;
    std::string_view transport_policy_version;
};

struct SupervisorReduction {
    std::vector<SupervisorEffect> effects;
    SupervisorState state;
    std::optional<SupervisorTransition> transition;
};

struct SupervisorStatsSnapshot {
    std::size_t attempt = 0;
    std::size_t attempt_ceiling = 0;
    std::optional<Duration> elapsed_budget;
    std::string_view policy_version;
    std::optional<std::string> reason;
    SupervisorStateName state = SupervisorStateName::Idle;
    std::optional<RecoveryTransport> transport;
};

[[nodiscard]] SupervisorState initial_supervisor_state();
[[nodiscard]] bool supervisor_deadlines_valid(const SupervisorState& state);
[[nodiscard]] std::optional<TimePoint> next_deadline_at(const SupervisorState& state);
[[nodiscard]] SupervisorReduction reduce_supervisor_state(
    const SupervisorState& state, const SupervisorEvent& event, TimePoint now,
    const RecoveryPolicy& policy = kDefaultRecoveryPolicy);
[[nodiscard]] SupervisorStatsSnapshot project_supervisor_stats(
    const SupervisorState& state, TimePoint now,
    const RecoveryPolicy& policy = kDefaultRecoveryPolicy);

const char* to_string(SupervisorStateName value);
const char* to_string(RecoveryTransport value);
const char* to_string(RecoveryAction value);
const char* to_string(DetectionReason value);
const char* to_string(FailureReason value);
const char* effect_name(const SupervisorEffectPayload& value);

}  // namespace coax::core
