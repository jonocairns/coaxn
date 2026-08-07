#include <catch2/catch_test_macros.hpp>

#include "util/redact.hpp"

using coax::log::redact_portal_url;
using coax::log::redact_stream_url;

TEST_CASE("a pasted portal link loses the query that carries its credentials") {
    REQUIRE(redact_portal_url("http://host:8080/player_api.php?username=u&password=p") ==
            "http://host:8080/player_api.php?***");
    REQUIRE(redact_portal_url("http://host:8080/get.php?username=u&password=p&type=m3u_plus") ==
            "http://host:8080/get.php?***");
}

TEST_CASE("a bare portal origin survives redaction unchanged") {
    REQUIRE(redact_portal_url("https://provider.example.com") ==
            "https://provider.example.com");
    REQUIRE(redact_portal_url("https://provider.example.com:8080/") ==
            "https://provider.example.com:8080/");
    REQUIRE(redact_portal_url("") == "");
}

TEST_CASE("credentials in the authority are masked") {
    REQUIRE(redact_portal_url("http://user:pass@host:8080/player_api.php") ==
            "http://***@host:8080/player_api.php");
    REQUIRE(redact_portal_url("http://user:pass@host") == "http://***@host");
}

TEST_CASE("an at sign in the path is not mistaken for userinfo") {
    REQUIRE(redact_portal_url("http://host/live/a@b") == "http://host/live/a@b");
}

TEST_CASE("a fragment is dropped alongside the query") {
    REQUIRE(redact_portal_url("http://host/player_api.php#username=u") ==
            "http://host/player_api.php?***");
}

TEST_CASE("stream URLs mask the two segments after the media marker") {
    REQUIRE(redact_stream_url("http://host:8080/live/user/pass/123.ts") ==
            "http://host:8080/live/***/***/123.ts");
    REQUIRE(redact_stream_url("http://host:8080/movie/user/pass/9.mkv") ==
            "http://host:8080/movie/***/***/9.mkv");
    REQUIRE(redact_stream_url("http://host:8080/series/user/pass/9.mkv") ==
            "http://host:8080/series/***/***/9.mkv");
}

// This case used to be called "a stream URL without the expected shape is left
// alone", which was the defect stated as a guarantee: an unrecognized target
// was logged verbatim. Both URLs below survive intact for the narrower and
// still-true reason that neither carries anything to sanitize.
TEST_CASE("a stream URL with nothing to sanitize keeps its shape") {
    REQUIRE(redact_stream_url("http://host:8080/live/onlyonesegment") ==
            "http://host:8080/live/onlyonesegment");
    REQUIRE(redact_stream_url("http://host:8080/index.m3u8") ==
            "http://host:8080/index.m3u8");
}

TEST_CASE("an unrecognized stream URL still loses its query") {
    // The command line accepts an arbitrary direct-media URL, so this reaches
    // the load log without matching any Xtream shape.
    REQUIRE(redact_stream_url("https://example.invalid/stream?token=secret") ==
            "https://example.invalid/stream?***");
    REQUIRE(redact_stream_url("https://cdn.example/hls/index.m3u8?hdnts=exp=1~hmac=deadbeef") ==
            "https://cdn.example/hls/index.m3u8?***");
    REQUIRE(redact_stream_url("https://example.invalid/stream#token=secret") ==
            "https://example.invalid/stream?***");
}

TEST_CASE("stream credentials outside the masked path segments are still hidden") {
    // Userinfo and query live outside the two segments the Xtream mask covers,
    // in URLs that do and do not match that shape.
    REQUIRE(redact_stream_url("http://user:pass@host:8080/live/u/p/123.ts") ==
            "http://***@host:8080/live/***/***/123.ts");
    REQUIRE(redact_stream_url("http://host:8080/live/u/p/123.ts?token=secret") ==
            "http://host:8080/live/***/***/123.ts?***");
    REQUIRE(redact_stream_url("http://user:pass@host:8080/whatever.ts") ==
            "http://***@host:8080/whatever.ts");
}

TEST_CASE("a local media path is not mistaken for a URL to sanitize") {
    // mpv takes file paths as readily as URLs, and there is no credential in
    // one to hide -- but also no scheme, authority or query to trip over.
    REQUIRE(redact_stream_url("C:\\videos\\recording.mkv") == "C:\\videos\\recording.mkv");
    REQUIRE(redact_stream_url("/mnt/media/recording.mkv") == "/mnt/media/recording.mkv");
    REQUIRE(redact_stream_url("") == "");
}
