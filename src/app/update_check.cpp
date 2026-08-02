#include "app/update_check.hpp"

#include <windows.h>
#include <shellapi.h>

#include <nlohmann/json.hpp>

#include "core/version.hpp"
#include "util/http.hpp"
#include "util/log.hpp"

namespace coax::app {
namespace {

using json = nlohmann::json;

// `releases/latest` excludes drafts and pre-releases, so a tagged beta never
// reaches anyone who did not go looking for it.
constexpr const char* kLatestReleaseUrl =
    "https://api.github.com/repos/jonocairns/coaxn/releases/latest";

constexpr const char* kReleasesPageUrl =
    "https://github.com/jonocairns/coaxn/releases/latest";

}  // namespace

std::optional<UpdateInfo> check_for_update() {
    // Deliberately impatient. Nothing depends on this answer, and shutdown
    // joins the thread running it, so a wedged connection must not be able to
    // hold the window open.
    constexpr util::http::Timeouts kTimeouts{
        .resolve_ms = 4000, .connect_ms = 4000, .send_ms = 6000, .receive_ms = 6000};

    std::string body;
    std::string error;
    if (!util::http::get(kLatestReleaseUrl, body, error, kTimeouts)) {
        // Being offline is the common case here, not a fault worth surfacing.
        log::info("Update check skipped: {}", error);
        return std::nullopt;
    }

    std::string tag;
    std::string page;
    try {
        const json release = json::parse(body);
        if (release.contains("tag_name") && release["tag_name"].is_string()) {
            tag = release["tag_name"].get<std::string>();
        }
        if (release.contains("html_url") && release["html_url"].is_string()) {
            page = release["html_url"].get<std::string>();
        }
    } catch (const json::exception& e) {
        log::info("Update check could not read the release feed ({})", e.what());
        return std::nullopt;
    }

    if (!core::is_update_available(tag, COAX_VERSION)) {
        return std::nullopt;
    }

    // The tag parsed cleanly to get here, so dropping a `v` prefix is all that
    // stands between it and a version to show.
    std::string version = tag;
    if (!version.empty() && (version.front() == 'v' || version.front() == 'V')) {
        version.erase(version.begin());
    }

    log::info("Update available: {} (running {})", version, COAX_VERSION);
    return UpdateInfo{.version  = std::move(version),
                      .page_url = page.empty() ? kReleasesPageUrl : std::move(page)};
}

void open_in_browser(std::string_view url) {
    const std::wstring wide = util::http::widen(url);
    ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

}  // namespace coax::app
