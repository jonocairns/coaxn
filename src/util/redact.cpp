#include "util/redact.hpp"

namespace coax::log {

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
