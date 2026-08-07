#pragma once

#include <string>
#include <string_view>

// Credential masking for anything bound for the session log or the diagnostics
// overlay. Kept free of platform headers so the rules are covered by the native
// core test build rather than only by the Windows app build.
namespace coax::log {

// Makes a playback target safe to log, whatever shape it is in. Userinfo is
// masked and the query dropped for every target, as redact_portal_url does;
// an Xtream path additionally loses its credential segments.
// http://host:port/live/user/pass/123.ts -> http://host:port/live/***/***/123.ts
// https://host/stream?token=secret       -> https://host/stream?***
// Safety does not depend on recognising the shape: a target matching nothing
// known is still sanitized, because the command line accepts an arbitrary
// direct-media URL and a provider base URL can carry credentials outside the
// path. See PRD 7.7 -- full authenticated URLs are prohibited from logs.
std::string redact_stream_url(std::string_view url);

// Masks a portal link as typed by the user. A pasted player_api.php or get.php
// link carries the credentials in its query string, so the query is dropped
// wholesale rather than by parameter name -- providers differ on what they put
// there, and only the origin and path are worth logging.
// http://host:8080/player_api.php?username=u&password=p
//   -> http://host:8080/player_api.php?***
std::string redact_portal_url(std::string_view url);

}  // namespace coax::log
