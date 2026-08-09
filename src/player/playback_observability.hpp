#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/playback_health.hpp"
#include "core/playback_types.hpp"
#include "core/supervisor.hpp"

namespace coax::player {

enum class TimelineClassification {
    Unavailable,
    NormalAdvance,
    ForwardJump,
    ForwardLag,
    Backward,
    NoProgress,
    PausedNoProgress,
    ResumeLag,
};

TimelineClassification classify_timeline(
    const core::TimelineEvidence& evidence,
    const core::HealthPolicy& policy);
const char* to_string(TimelineClassification value);

// Warning messages can contain complete authenticated URLs and request
// headers. Only these closed, provider-independent categories survive the mpv
// event turn; raw text is never returned or stored.
enum class EngineWarningComponent {
    Player,
    Demuxer,
    VideoDecoder,
    AudioDecoder,
    Stream,
    Http,
    Other,
};

enum class EngineWarningCategory {
    Authentication,
    HttpFailure,
    NetworkTimeout,
    HlsPlaylist,
    HlsSegment,
    FormatProbe,
    TimestampDiscontinuity,
    NonMonotonicTimestamp,
    ContinuityError,
    CorruptPacket,
    DecodeError,
    Other,
};

enum class EngineLogSeverity { Warning, Error, Fatal, Other };

struct SanitizedEngineWarning {
    EngineWarningComponent component = EngineWarningComponent::Other;
    EngineWarningCategory category = EngineWarningCategory::Other;
    EngineLogSeverity severity = EngineLogSeverity::Other;

    bool operator==(const SanitizedEngineWarning&) const = default;
};

SanitizedEngineWarning sanitize_engine_warning(std::string_view prefix,
                                                std::string_view text,
                                                std::string_view level);
const char* to_string(EngineWarningComponent value);
const char* to_string(EngineWarningCategory value);
const char* to_string(EngineLogSeverity value);

// mpv log messages carry no request or playlist-entry identity. Attribute one
// to the active entry only after START_FILE and only while the adapter's active
// generation agrees with the current target; the replacement window stays
// explicitly unattributed.
enum class EngineMessageAttribution { ActiveEntry, Unattributed };

EngineMessageAttribution classify_engine_message_attribution(
    bool start_file_seen,
    std::optional<core::Generation> active_generation,
    std::optional<core::Generation> target_generation);
const char* to_string(EngineMessageAttribution value);

// Limits the session log to the first occurrence of each closed diagnostic
// triple per load and attribution class. Counters and latest-message fields are
// updated separately for every event, including suppressed duplicates.
class EngineDiagnosticLogGate {
public:
    bool first_occurrence(EngineMessageAttribution attribution,
                          const SanitizedEngineWarning& warning);
    void reset() { seen_.clear(); }

private:
    struct Entry {
        EngineMessageAttribution attribution;
        SanitizedEngineWarning warning;

        bool operator==(const Entry&) const = default;
    };
    std::vector<Entry> seen_;
};

enum class RequestScheme { Http, Https, LocalFile, Other };
enum class RequestTargetShape { XtreamLive, HlsPlaylist, MediaPath, Opaque };

struct SourceCorrelation {
    // Zero means the load has no provider session, as with direct media.
    std::uint64_t provider_session = 0;
    std::uint64_t channel_session = 0;
};

// This describes only the loadfile command Coax hands to libmpv. It contains no
// host, path, query value, userinfo, credential or header value.
struct SanitizedRequestShape {
    core::LoadIntent intent = core::LoadIntent::FreshSelection;
    core::LoadAttempt load_attempt;
    RequestScheme scheme = RequestScheme::Other;
    RequestTargetShape target = RequestTargetShape::Opaque;
    core::RecoveryTransport transport = core::RecoveryTransport::MpegTs;
    bool query_present = false;
    bool userinfo_present = false;
    bool forced_format = false;
    SourceCorrelation correlation;
};

SanitizedRequestShape inspect_request_shape(
    std::string_view target, core::LoadIntent intent, core::LoadAttempt load_attempt,
    core::RecoveryTransport transport, bool forced_format,
    SourceCorrelation correlation = {});
const char* to_string(RequestScheme value);
const char* to_string(RequestTargetShape value);

// Optional decision-time evidence is already numeric or from closed enums.
// There is no field capable of carrying a URL, host, header, cookie, query
// value, credential, or raw engine message into the retained telemetry line.
struct RecoveryDecisionEvidence {
    bool cache_paused = false;
    std::optional<double> playback_movement_seconds;
    std::optional<double> cache_end_movement_seconds;
    std::optional<SanitizedEngineWarning> engine_warning;
};

std::string format_recovery_telemetry(
    const core::SupervisorTransition& transition,
    const std::optional<SanitizedRequestShape>& request,
    const RecoveryDecisionEvidence& evidence = {});

}  // namespace coax::player
