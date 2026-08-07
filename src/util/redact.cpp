#include "util/redact.hpp"

#include <algorithm>

namespace coax::log {
namespace {

// Xtream live URLs are /live/<user>/<pass>/<id>.<ext>; mask the two segments
// after the /live/ (or /movie/, /series/) marker. Nothing else about the URL is
// this function's business -- it recognises one shape, and a URL not in that
// shape comes back untouched, which is why it is not safe on its own.
//
// What it does not require is a *complete* URL of that shape. A target ending
// at the password, or carrying a query where the stream id should be, still has
// two credential segments in it, and masking used to depend on a third segment
// following them -- so http://host/live/user/pass was logged whole. Two
// segments present is the whole condition; what comes after them is preserved
// but does not decide anything.
std::string mask_media_path_segments(std::string_view url) {
    for (std::string_view marker : {"/live/", "/movie/", "/series/"}) {
        const auto pos = url.find(marker);
        if (pos == std::string_view::npos) {
            continue;
        }

        const auto creds_start = pos + marker.size();
        // Segments end where the path does. Everything from a '?' or '#' on is
        // carried through untouched -- by this point redact_portal_url has
        // already replaced any real query with its marker.
        const auto path_end = std::min(url.find_first_of("?#", creds_start), url.size());

        const auto first_sep = url.find('/', creds_start);
        if (first_sep >= path_end) {
            continue;  // one segment at most: a stream name, not a credential pair
        }

        std::string out(url.substr(0, creds_start));
        out += "***/***";

        const auto second_sep = url.find('/', first_sep + 1);
        if (second_sep < path_end) {
            out += '/';
            out += url.substr(second_sep + 1);
        } else {
            // The password runs to the end of the path; keep any query marker.
            out += url.substr(path_end);
        }
        return out;
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
