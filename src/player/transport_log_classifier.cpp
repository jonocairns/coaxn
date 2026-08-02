#include "player/transport_log_classifier.hpp"

#include <regex>
#include <string>

namespace coax::player {
namespace {

const std::regex kHttp401{R"(\bHTTP error 401(?:\s|$))", std::regex::icase};
const std::regex kHttpCode401{R"(\bhttp_code=401\b)", std::regex::icase};
const std::regex kTimeout{R"(\b(?:connection timed out|operation timed out|request timed out)\b)",
                          std::regex::icase};
const std::regex kHttpRead138{
    R"(\bError reading HTTP response: Error number -138 occurred\b)", std::regex::icase};
const std::regex kHttpRead10054{
    R"(\bError reading HTTP response: Error number -10054 occurred\b)", std::regex::icase};
const std::regex kHttp503{R"(\bHTTP error 503(?:\s|$))", std::regex::icase};
const std::regex kHlsSegmentOpen{
    R"(\b(?:failed to open|unable to open)\b.*\bsegment\b)", std::regex::icase};
const std::regex kHlsKeepaliveOpen{
    R"(\bhls: keepalive request failed\b.*\bwhen opening url\b)", std::regex::icase};
const std::regex kHlsKeepalivePlaylist{
    R"(\bhls: keepalive request failed\b.*\bwhen parsing playlist\b)", std::regex::icase};
const std::regex kUnrecognizedFormat{R"(\bFailed to recognize file format\b)",
                                     std::regex::icase};
const std::regex kUndeterminedFormat{
    R"(\bCould not (?:determine|find) (?:the )?(?:input )?format\b)", std::regex::icase};
const std::regex kInvalidData{R"(\bInvalid data found when processing input\b)",
                              std::regex::icase};

bool matches(std::string_view text, const std::regex& pattern) {
    return std::regex_search(text.begin(), text.end(), pattern);
}

}  // namespace

std::optional<TransportLogClassification> classify_transport_log(
    std::string_view text, core::RecoveryTransport transport,
    bool file_loaded, bool probed_format_forced) {
    if (matches(text, kHttp401) || matches(text, kHttpCode401)) {
        return AuthenticationRejected{};
    }
    if (matches(text, kTimeout) || matches(text, kHttpRead138)) {
        return core::TransportFailureReason::HttpRequestTimeout;
    }
    if (transport == core::RecoveryTransport::Hls &&
        (matches(text, kHttpRead10054) || matches(text, kHttp503))) {
        return core::TransportFailureReason::HlsPlaylistFailed;
    }
    if (transport == core::RecoveryTransport::Hls &&
        (matches(text, kHlsSegmentOpen) || matches(text, kHlsKeepaliveOpen))) {
        return core::TransportFailureReason::HlsSegmentUnavailable;
    }
    if (transport == core::RecoveryTransport::Hls &&
        matches(text, kHlsKeepalivePlaylist)) {
        return core::TransportFailureReason::HlsPlaylistFailed;
    }
    if (!probed_format_forced &&
        (matches(text, kUnrecognizedFormat) || matches(text, kUndeterminedFormat) ||
         (!file_loaded && matches(text, kInvalidData)))) {
        return core::TransportFailureReason::FormatProbeRequired;
    }
    return std::nullopt;
}

}  // namespace coax::player
