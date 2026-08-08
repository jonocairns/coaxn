#include "player/playback_observability.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
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
                                                std::string_view text) {
    const std::string component = lower(prefix);
    const std::string message = lower(text);
    SanitizedEngineWarning result;

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
               contains(message, "playlist")) {
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

SanitizedRequestShape inspect_request_shape(
    std::string_view target, LoadRequestIntent intent,
    core::RecoveryTransport transport, bool forced_format) {
    SanitizedRequestShape shape;
    shape.intent = intent;
    shape.transport = transport;
    shape.forced_format = forced_format;
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

const char* to_string(LoadRequestIntent value) {
    switch (value) {
        case LoadRequestIntent::FreshSelection: return "fresh-selection";
        case LoadRequestIntent::RecoveryReopen: return "recovery-reopen";
        case LoadRequestIntent::PlayerRecreation: return "player-recreation";
    }
    return "fresh-selection";
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
