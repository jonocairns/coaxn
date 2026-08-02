#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace coax::app {

struct UpdateInfo {
    std::string version;   // Release tag with its `v` prefix stripped.
    std::string page_url;  // Release page to send the user to.
};

// Asks GitHub whether a release newer than this build exists. Blocking and
// network-bound: call it off the UI thread.
//
// Returns nothing for every uninteresting answer — offline, rate-limited, no
// releases yet, an unparseable tag, or already up to date. A failed check is
// not worth reporting to someone who is trying to watch television, so the
// only outcome the caller can act on is a genuine newer release.
std::optional<UpdateInfo> check_for_update();

// Opens a URL in the user's default browser.
void open_in_browser(std::string_view url);

}  // namespace coax::app
