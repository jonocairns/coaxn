#include <catch2/catch_test_macros.hpp>

#include "util/redact.hpp"

using coax::log::redact_portal_url;

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
