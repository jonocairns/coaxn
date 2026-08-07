#include "util/redact.hpp"

namespace coax::log {
namespace {

// Xtream live URLs are /live/<user>/<pass>/<id>.<ext>; mask the two segments
// after the /live/ (or /movie/, /series/) marker. Nothing else about the URL is
// this function's business -- it recognises one shape, and a URL not in that
// shape comes back untouched, which is why it is not safe on its own.
std::string mask_media_path_segments(std::string_view url) {
    for (std::string_view marker : {"/live/", "/movie/", "/series/"}) {
        const auto pos = url.find(marker);
        if (pos == std::string_view::npos) {
            continue;
        }

        const auto creds_start = pos + marker.size();
        auto       cursor      = creds_start;
        int        segments    = 0;
        while (segments < 2 && cursor < url.size()) {
            const auto slash = url.find('/', cursor);
            if (slash == std::string_view::npos) {
                break;
            }
            cursor = slash + 1;
            ++segments;
        }

        if (segments == 2) {
            std::string out(url.substr(0, creds_start));
            out += "***/***/";
            out += url.substr(cursor);
            return out;
        }
    }
    return std::string(url);
}

}  // namespace

std::string redact_stream_url(std::string_view url) {
    // A playback target is a URL before it is an Xtream URL, and it does not
    // have to be an Xtream URL at all: the command line accepts an arbitrary
    // direct-media argument, and a provider base URL can carry credentials in
    // its authority or query, neither of which masking two path segments
    // touches. So the general sanitizer runs first and unconditionally, and the
    // Xtream mask only refines what survives it. Recognising the shape then
    // decides how much more is hidden -- never whether anything is.
    return mask_media_path_segments(redact_portal_url(url));
}

std::string redact_portal_url(std::string_view url) {
    const auto  query_start = url.find_first_of("?#");
    std::string out(url.substr(0, query_start));

    // scheme://user:pass@host -> scheme://***@host. The authority ends at the
    // first '/', so an '@' later in the path is not userinfo.
    if (const auto scheme = out.find("://"); scheme != std::string::npos) {
        const auto authority = scheme + 3;
        const auto path      = out.find('/', authority);
        if (const auto at = out.rfind('@', path); at != std::string::npos && at >= authority) {
            out.replace(authority, at - authority, "***");
        }
    }

    if (query_start != std::string_view::npos) {
        // Kept as a marker so a truncated link is not mistaken for one that
        // never carried a query.
        out += "?***";
    }
    return out;
}

}  // namespace coax::log
