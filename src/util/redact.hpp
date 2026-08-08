#pragma once

#include <string>
#include <string_view>

// Credential masking for anything bound for the session log or the diagnostics
// overlay. Kept free of platform headers so the rules are covered by the native
// core test build rather than only by the Windows app build.
namespace coax::log {

// Masks a portal link as typed by the user. A pasted player_api.php or get.php
// link carries the credentials in its query string, so the query is dropped
// wholesale rather than by parameter name -- providers differ on what they put
// there, and only the origin and path are worth logging.
// http://host:8080/player_api.php?username=u&password=p
//   -> http://host:8080/player_api.php?***
std::string redact_portal_url(std::string_view url);

}  // namespace coax::log
