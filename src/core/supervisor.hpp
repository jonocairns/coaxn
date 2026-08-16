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
enum class RecreationAuthority { HeuristicShortLoad, Mandatory };
enum class RecreationProvenance { HeuristicShortLoad, MandatoryEvidence };
enum class RecoveryEffectStatus { Scheduled, Issued };
enum class RecoveryEscalation { None, SourceReopen, PlayerRecreation };

struct RecreationDetails {
    RecreationAuthority authority;
    RecreationProvenance provenance;
};

struct RecoveryPlan {
    RecoveryAction mechanism;
    std::optional<RecreationDetails> recreation;
    RecoveryEffectStatus status;
};
enum class RecoveryOutcome {
    None,
    FaultDecided,
    CommandIssued,
    FirstFrame,
    LateFirstFrame,
    CleanProbation,
    RenewedStall,
    RenewedEof,
    TerminalFailure,
};
enum class TransportFailureReason {
    FormatProbeRequired,
    HlsPlaylistFailed,
    HlsSegmentUnavailable,
    HttpRequestTimeout,
};
enum class DetectionReason {
    CacheStall,
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
    TimelineRegression,
    StreamEndedEof,
    StreamEndedError,
    StreamEndedUnknown,
};
enum class FailureReason { AttemptsExhausted, AuthRejected, BudgetExpired, SourceUnavailable };
enum class EndReason { Eof, Error, Unknown };
enum class StallKind { Cache, Open, Progress };

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
    LoadAttempt load_attempt;
    LoadIntent load_intent = LoadIntent::FreshSelection;
    std::optional<TimePoint> load_command_at;
    std::optional<TimePoint> last_forward_progress_at;
    SupervisorStateName name = SupervisorStateName::Idle;
    std::optional<LoadAttempt> pending_load_attempt;
    std::optional<LoadIntent> pending_load_intent;
    // Mechanism, recreation justification and plan-local lifecycle move as one
    // value. Recreation metadata is present only for recreation; authority may
    // upgrade while provenance remains immutable. Status changes from
    // Scheduled to Issued at the effect-emission boundary.
    std::optional<RecoveryPlan> recovery_plan;
    std::optional<TimePoint> recovery_started_at;
    std::optional<TimePoint> fault_decided_at;
    std::size_t short_recovery_load_failures = 0;
    std::optional<LoadAttempt> last_short_recovery_failure_attempt;
    bool short_load_recreation_used = false;
    // Command exhaustion may leave the exact current load running. Only an
    // opening-stalled load that has not produced a frame can consume this
    // one-shot admission back into probation.
    bool late_completion_available = false;
    bool late_completion_probation = false;
    std::optional<RecoveryTransport> transport;
};

struct DeadlineReached {};
struct CacheState { Generation generation; LoadAttempt load_attempt; bool paused; };
struct AuthRejected { Generation generation; LoadAttempt load_attempt; };
struct DecodeStalled { Generation generation; LoadAttempt load_attempt; };
struct FirstFrame { Generation generation; LoadAttempt load_attempt; };
struct IpcUnresponsive { Generation generation; LoadAttempt load_attempt; };
struct ChannelRequested { Generation generation; };
struct PlaybackHealthObserved { Generation generation; LoadAttempt load_attempt; bool unhealthy; };
struct ForwardProgressObserved { Generation generation; LoadAttempt load_attempt; };
struct PlaybackStopped { Generation generation; };
// The graphics device the video is presented through went away. mpv's own
// device dies with it, so the stream has to be reopened on a rebuilt one —
// which is what the player-recreation action already does. Routing it here
// rather than recovering beside the supervisor is what gives presentation loss
// the same bounded attempts and the same generation fence as every other fault.
struct PresentationLost { Generation generation; };
struct ProcessExited { Generation generation; LoadAttempt load_attempt; };
struct SourceFailed { Generation generation; LoadAttempt load_attempt; };
struct PlaybackStalled { Generation generation; LoadAttempt load_attempt; StallKind stall; };
struct TimelineRegressed { Generation generation; LoadAttempt load_attempt; };
struct StreamEnded {
    Generation generation;
    LoadAttempt load_attempt;
    EndReason end_reason;
    std::optional<TransportFailureReason> failure_reason;
};
struct StreamLoadIssued {
    Generation generation;
    LoadAttempt load_attempt;
    LoadIntent intent;
    RecoveryTransport transport;
};

using SupervisorEvent = std::variant<
    DeadlineReached, CacheState, AuthRejected, DecodeStalled, FirstFrame,
    IpcUnresponsive, ChannelRequested, PlaybackHealthObserved, ForwardProgressObserved,
    PlaybackStopped,
    PresentationLost, ProcessExited, SourceFailed, PlaybackStalled, TimelineRegressed, StreamEnded,
    StreamLoadIssued>;

struct ReopenStream {};
struct ReloadHlsLive {};
struct ReopenProbed {};
struct RecreatePlayer {};
using SupervisorEffectPayload = std::variant<ReopenStream, ReloadHlsLive, ReopenProbed,
                                             RecreatePlayer>;
struct SupervisorEffect {
    Generation generation;
    LoadAttempt load_attempt;
    SupervisorEffectPayload payload;
};

struct SupervisorTransition {
    std::size_t attempt = 0;
    Duration elapsed_budget{};
    SupervisorStateName from = SupervisorStateName::Idle;
    Generation generation;
    LoadAttempt load_attempt;
    LoadIntent load_intent = LoadIntent::FreshSelection;
    RecoveryEscalation escalation = RecoveryEscalation::None;
    RecoveryOutcome outcome = RecoveryOutcome::None;
    std::optional<RecoveryPlan> recovery_plan;
    std::optional<Duration> last_progress_to_decision;
    std::optional<Duration> decision_to_command;
    std::optional<Duration> command_to_first_frame;
    std::optional<Duration> first_frame_to_outcome;
    std::optional<Duration> recovered_load_lifetime;
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
    LoadAttempt load_attempt;
    LoadIntent load_intent = LoadIntent::FreshSelection;
    bool short_load_recreation_used = false;
};

[[nodiscard]] SupervisorState initial_supervisor_state();
[[nodiscard]] bool supervisor_deadlines_valid(const SupervisorState& state);
[[nodiscard]] bool supervisor_health_supervision_enabled(
    SupervisorStateName state);
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
const char* to_string(RecreationAuthority value);
const char* to_string(RecreationProvenance value);
const char* to_string(RecoveryEffectStatus value);
const char* to_string(RecoveryEscalation value);
const char* to_string(RecoveryOutcome value);
const char* to_string(LoadIntent value);
const char* to_string(DetectionReason value);
const char* to_string(FailureReason value);
const char* effect_name(const SupervisorEffectPayload& value);

}  // namespace coax::core
