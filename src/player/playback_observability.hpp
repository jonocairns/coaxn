#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "core/playback_health.hpp"
#include "core/playback_types.hpp"

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
};

SanitizedEngineWarning sanitize_engine_warning(std::string_view prefix,
                                                std::string_view text,
                                                std::string_view level);
const char* to_string(EngineWarningComponent value);
const char* to_string(EngineWarningCategory value);
const char* to_string(EngineLogSeverity value);

enum class LoadRequestIntent { FreshSelection, RecoveryReopen, PlayerRecreation };
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
    LoadRequestIntent intent = LoadRequestIntent::FreshSelection;
    RequestScheme scheme = RequestScheme::Other;
    RequestTargetShape target = RequestTargetShape::Opaque;
    core::RecoveryTransport transport = core::RecoveryTransport::MpegTs;
    bool query_present = false;
    bool userinfo_present = false;
    bool forced_format = false;
    SourceCorrelation correlation;
};

SanitizedRequestShape inspect_request_shape(
    std::string_view target, LoadRequestIntent intent,
    core::RecoveryTransport transport, bool forced_format,
    SourceCorrelation correlation = {});
const char* to_string(LoadRequestIntent value);
const char* to_string(RequestScheme value);
const char* to_string(RequestTargetShape value);

}  // namespace coax::player
