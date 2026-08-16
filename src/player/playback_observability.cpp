#include "player/playback_observability.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>

namespace coax::player {
namespace {

std::string lower(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return result;
}

bool contains(const std::string& value, std::string_view needle) {
    return value.find(needle) != std::string::npos;
}

bool ends_with(const std::string& value, std::string_view suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

}  // namespace

TimelineClassification classify_timeline(const core::TimelineEvidence& evidence,
                                           const core::HealthPolicy& policy) {
    if (!evidence.playback_movement_seconds ||
        !evidence.playback_deviation_seconds) {
        return TimelineClassification::Unavailable;
    }
    const double movement = *evidence.playback_movement_seconds;
    const double deviation = *evidence.playback_deviation_seconds;
    if (movement < -policy.progress_epsilon_seconds) {
        return TimelineClassification::Backward;
    }
    if (std::abs(movement) <= policy.progress_epsilon_seconds) {
        return evidence.cache_paused ? TimelineClassification::PausedNoProgress
                                     : TimelineClassification::NoProgress;
    }
    if (deviation > policy.discontinuity_jump_seconds) {
        return TimelineClassification::ForwardJump;
    }
    if (deviation < -policy.discontinuity_jump_seconds) {
        return evidence.previous_cache_paused && *evidence.previous_cache_paused &&
                       !evidence.cache_paused
                   ? TimelineClassification::ResumeLag
                   : TimelineClassification::ForwardLag;
    }
    return TimelineClassification::NormalAdvance;
}

const char* to_string(TimelineClassification value) {
    switch (value) {
        case TimelineClassification::Unavailable: return "unavailable";
        case TimelineClassification::NormalAdvance: return "normal-advance";
        case TimelineClassification::ForwardJump: return "forward-jump";
        case TimelineClassification::ForwardLag: return "forward-lag";
        case TimelineClassification::Backward: return "backward";
        case TimelineClassification::NoProgress: return "no-progress";
        case TimelineClassification::PausedNoProgress: return "paused-no-progress";
        case TimelineClassification::ResumeLag: return "resume-lag";
    }
    return "unavailable";
}

SanitizedEngineWarning sanitize_engine_warning(std::string_view prefix,
                                                std::string_view text,
                                                std::string_view level) {
    const std::string component = lower(prefix);
    const std::string message = lower(text);
    SanitizedEngineWarning result;
    const std::string severity = lower(level);

    if (severity == "warn") result.severity = EngineLogSeverity::Warning;
    else if (severity == "error") result.severity = EngineLogSeverity::Error;
    else if (severity == "fatal") result.severity = EngineLogSeverity::Fatal;

    if (contains(component, "cplayer")) result.component = EngineWarningComponent::Player;
    else if (contains(component, "demux")) result.component = EngineWarningComponent::Demuxer;
    else if (contains(component, "video") || component == "vd")
        result.component = EngineWarningComponent::VideoDecoder;
    else if (contains(component, "audio") || component == "ad")
        result.component = EngineWarningComponent::AudioDecoder;
    else if (contains(component, "http")) result.component = EngineWarningComponent::Http;
    else if (contains(component, "stream")) result.component = EngineWarningComponent::Stream;

    if (contains(message, "non-monoton")) {
        result.category = EngineWarningCategory::NonMonotonicTimestamp;
    } else if (contains(message, "timestamp discontinu") ||
               contains(message, "timestamp jump") || contains(message, "dts out of order")) {
        result.category = EngineWarningCategory::TimestampDiscontinuity;
    } else if (contains(message, "continuity check failed")) {
        result.category = EngineWarningCategory::ContinuityError;
    } else if (contains(message, "packet corrupt") ||
               contains(message, "corrupt input packet") ||
               contains(message, "pes packet size mismatch")) {
        result.category = EngineWarningCategory::CorruptPacket;
    } else if (contains(message, "error while decoding") ||
               contains(message, "decode_slice_header error") ||
               contains(message, "invalid nal unit")) {
        result.category = EngineWarningCategory::DecodeError;
    } else if (contains(message, "http error 401") || contains(message, "http_code=401") ||
               contains(message, "authorization:") || contains(message, "www-authenticate:")) {
        result.category = EngineWarningCategory::Authentication;
    } else if (contains(message, "timed out") || contains(message, "error number -138")) {
        result.category = EngineWarningCategory::NetworkTimeout;
    } else if (contains(message, "hls: keepalive request failed") &&
               contains(message, "when parsing playlist")) {
        result.category = EngineWarningCategory::HlsPlaylist;
    } else if (((contains(message, "failed to open") || contains(message, "unable to open")) &&
                contains(message, "segment")) ||
               (contains(message, "hls: keepalive request failed") &&
                contains(message, "when opening url"))) {
        result.category = EngineWarningCategory::HlsSegment;
    } else if (contains(message, "failed to recognize file format") ||
               contains(message, "could not determine the input format") ||
               contains(message, "could not find format")) {
        result.category = EngineWarningCategory::FormatProbe;
    } else if (contains(message, "http error ") ||
               contains(message, "error reading http response")) {
        result.category = EngineWarningCategory::HttpFailure;
    }
    return result;
}

const char* to_string(EngineWarningComponent value) {
    switch (value) {
        case EngineWarningComponent::Player: return "player";
        case EngineWarningComponent::Demuxer: return "demuxer";
        case EngineWarningComponent::VideoDecoder: return "video-decoder";
        case EngineWarningComponent::AudioDecoder: return "audio-decoder";
        case EngineWarningComponent::Stream: return "stream";
        case EngineWarningComponent::Http: return "http";
        case EngineWarningComponent::Other: return "other";
    }
    return "other";
}

const char* to_string(EngineWarningCategory value) {
    switch (value) {
        case EngineWarningCategory::Authentication: return "authentication";
        case EngineWarningCategory::HttpFailure: return "http-failure";
        case EngineWarningCategory::NetworkTimeout: return "network-timeout";
        case EngineWarningCategory::HlsPlaylist: return "hls-playlist";
        case EngineWarningCategory::HlsSegment: return "hls-segment";
        case EngineWarningCategory::FormatProbe: return "format-probe";
        case EngineWarningCategory::TimestampDiscontinuity: return "timestamp-discontinuity";
        case EngineWarningCategory::NonMonotonicTimestamp: return "non-monotonic-timestamp";
        case EngineWarningCategory::ContinuityError: return "continuity-error";
        case EngineWarningCategory::CorruptPacket: return "corrupt-packet";
        case EngineWarningCategory::DecodeError: return "decode-error";
        case EngineWarningCategory::Other: return "other";
    }
    return "other";
}

const char* to_string(EngineLogSeverity value) {
    switch (value) {
        case EngineLogSeverity::Warning: return "warning";
        case EngineLogSeverity::Error: return "error";
        case EngineLogSeverity::Fatal: return "fatal";
        case EngineLogSeverity::Other: return "other";
    }
    return "other";
}

EngineMessageAttribution classify_engine_message_attribution(
    bool start_file_seen,
    std::optional<core::Generation> active_generation,
    std::optional<core::Generation> target_generation) {
    return start_file_seen && active_generation && target_generation &&
                   *active_generation == *target_generation
               ? EngineMessageAttribution::ActiveEntry
               : EngineMessageAttribution::Unattributed;
}

const char* to_string(EngineMessageAttribution value) {
    switch (value) {
        case EngineMessageAttribution::ActiveEntry: return "active-entry";
        case EngineMessageAttribution::Unattributed: return "unattributed";
    }
    return "unattributed";
}

bool EngineDiagnosticLogGate::first_occurrence(
    EngineMessageAttribution attribution,
    const SanitizedEngineWarning& warning) {
    const Entry entry{attribution, warning};
    if (std::find(seen_.begin(), seen_.end(), entry) != seen_.end()) return false;
    seen_.push_back(entry);
    return true;
}

SanitizedRequestShape inspect_request_shape(
    std::string_view target, core::LoadIntent intent, core::LoadAttempt load_attempt,
    core::RecoveryTransport transport, bool forced_format,
    SourceCorrelation correlation) {
    SanitizedRequestShape shape;
    shape.intent = intent;
    shape.load_attempt = load_attempt;
    shape.transport = transport;
    shape.forced_format = forced_format;
    shape.correlation = correlation;
    shape.query_present = target.find_first_of("?#") != std::string_view::npos;

    const std::string folded = lower(target);
    std::size_t authority = std::string::npos;
    if (folded.starts_with("http://")) {
        shape.scheme = RequestScheme::Http;
        authority = 7;
    } else if (folded.starts_with("https://")) {
        shape.scheme = RequestScheme::Https;
        authority = 8;
    } else if (folded.starts_with("file://") ||
               (target.size() >= 3 && std::isalpha(static_cast<unsigned char>(target[0])) &&
                target[1] == ':' && (target[2] == '\\' || target[2] == '/')) ||
               target.starts_with('/')) {
        shape.scheme = RequestScheme::LocalFile;
    }
    if (authority != std::string::npos) {
        const auto authority_end = target.find_first_of("/?#", authority);
        const auto at = target.find('@', authority);
        shape.userinfo_present = at != std::string_view::npos &&
            (authority_end == std::string_view::npos || at < authority_end);
    }

    const auto query = folded.find_first_of("?#");
    const std::string path = folded.substr(0, query);
    if (contains(path, "/live/")) shape.target = RequestTargetShape::XtreamLive;
    else if (ends_with(path, ".m3u8")) shape.target = RequestTargetShape::HlsPlaylist;
    else if (shape.scheme != RequestScheme::Other) shape.target = RequestTargetShape::MediaPath;
    return shape;
}

const char* to_string(RequestScheme value) {
    switch (value) {
        case RequestScheme::Http: return "http";
        case RequestScheme::Https: return "https";
        case RequestScheme::LocalFile: return "local-file";
        case RequestScheme::Other: return "other";
    }
    return "other";
}

std::string format_recovery_telemetry(
    const core::SupervisorTransition& transition,
    const std::optional<SanitizedRequestShape>& request,
    const RecoveryDecisionEvidence& evidence) {
    const auto duration = [](std::optional<core::Duration> value) {
        if (!value) return std::string("unavailable");
        std::ostringstream out;
        out << std::fixed << std::setprecision(0) << value->count() * 1000.0 << "ms";
        return out.str();
    };
    const auto movement = [](std::optional<double> value) {
        if (!value) return std::string("unavailable");
        std::ostringstream out;
        out << std::showpos << std::fixed << std::setprecision(3) << *value << "s";
        return out.str();
    };
    const auto correlation = request ? request->correlation : SourceCorrelation{};
    const auto warning_severity = evidence.engine_warning
        ? to_string(evidence.engine_warning->severity) : "none";
    const auto warning_component = evidence.engine_warning
        ? to_string(evidence.engine_warning->component) : "none";
    const auto warning_category = evidence.engine_warning
        ? to_string(evidence.engine_warning->category) : "none";
    const auto* recovery_plan = transition.recovery_plan
        ? &*transition.recovery_plan : nullptr;
    const auto* recreation = recovery_plan && recovery_plan->recreation
        ? &*recovery_plan->recreation : nullptr;
    const auto recovery_mechanism = recovery_plan
        ? core::to_string(recovery_plan->mechanism) : "none";
    const auto recreation_authority = recreation
        ? core::to_string(recreation->authority) : "none";
    const auto recreation_provenance = recreation
        ? core::to_string(recreation->provenance) : "none";
    const auto recovery_effect_status = recovery_plan
        ? core::to_string(recovery_plan->status) : "none";

    std::ostringstream out;
    out << "Recovery telemetry provider-session=" << correlation.provider_session
        << " channel-session=" << correlation.channel_session
        << " generation=" << transition.generation.value()
        << " load-attempt=" << transition.load_attempt.value()
        << " intent=" << core::to_string(transition.load_intent)
        << " supervisor-attempt=" << transition.attempt
        << " mechanism=" << recovery_mechanism
        << " authority=" << recreation_authority
        << " provenance=" << recreation_provenance
        << " effect-status=" << recovery_effect_status
        << " escalation=" << core::to_string(transition.escalation)
        << " outcome=" << core::to_string(transition.outcome)
        << " last-progress-to-decision="
        << duration(transition.last_progress_to_decision)
        << " decision-to-command=" << duration(transition.decision_to_command)
        << " command-to-first-frame=" << duration(transition.command_to_first_frame)
        << " first-frame-to-outcome=" << duration(transition.first_frame_to_outcome)
        << " recovered-load-lifetime=" << duration(transition.recovered_load_lifetime)
        << " cache-paused=" << (evidence.cache_paused ? "yes" : "no")
        << " playback-move=" << movement(evidence.playback_movement_seconds)
        << " cache-end-move=" << movement(evidence.cache_end_movement_seconds)
        << " warning-severity=" << warning_severity
        << " warning-component=" << warning_component
        << " warning-category=" << warning_category;
    return out.str();
}

const char* to_string(RequestTargetShape value) {
    switch (value) {
        case RequestTargetShape::XtreamLive: return "xtream-live";
        case RequestTargetShape::HlsPlaylist: return "hls-playlist";
        case RequestTargetShape::MediaPath: return "media-path";
        case RequestTargetShape::Opaque: return "opaque";
    }
    return "opaque";
}

}  // namespace coax::player
