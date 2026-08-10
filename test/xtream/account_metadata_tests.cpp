#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

#include "xtream/account_metadata.hpp"

using namespace coax::xtream;

TEST_CASE("account output formats are normalized without retaining provider metadata") {
    const auto result = parse_account_metadata(R"({
        "user_info": {
            "auth": "1",
            "username": "do-not-retain",
            "allowed_output_formats": [" TS ", "M3U8", "unknown", 7, "mpeg-ts"]
        },
        "server_info": {"url": "provider.example"}
    })");

    REQUIRE(result.status == AccountMetadataStatus::Parsed);
    CHECK(result.transports.advertised);
    CHECK(result.transports.mpeg_ts);
    CHECK(result.transports.hls);
}

TEST_CASE("a TS-only account remains distinguishable from dual transport support") {
    const auto result = parse_account_metadata(
        R"({"user_info":{"auth":1,"allowed_output_formats":["ts"]}})");

    REQUIRE(result.status == AccountMetadataStatus::Parsed);
    CHECK(result.transports.advertised);
    CHECK(supports(result.transports, TransportPreference::MpegTs));
    CHECK_FALSE(supports(result.transports, TransportPreference::Hls));
    CHECK(std::string_view(to_string(TransportPreference::MpegTs)) == "mpeg-ts");
    CHECK(std::string_view(to_string(TransportPreference::Hls)) == "hls");
}

TEST_CASE("providers may advertise a comma-separated output format string") {
    const auto result = parse_account_metadata(
        R"({"user_info":{"auth":true,"allowed_output_formats":" .ts, HLS "}})");

    REQUIRE(result.status == AccountMetadataStatus::Parsed);
    CHECK(result.transports.advertised);
    CHECK(result.transports.mpeg_ts);
    CHECK(result.transports.hls);
}

TEST_CASE("missing and malformed format lists establish no transport capability") {
    const auto missing = parse_account_metadata(R"({"user_info":{"auth":"true"}})");
    REQUIRE(missing.status == AccountMetadataStatus::Parsed);
    CHECK_FALSE(missing.transports.advertised);
    CHECK_FALSE(missing.transports.mpeg_ts);
    CHECK_FALSE(missing.transports.hls);

    const auto malformed =
        parse_account_metadata(R"({"user_info":{"auth":1,"allowed_output_formats":42}})");
    REQUIRE(malformed.status == AccountMetadataStatus::Parsed);
    CHECK_FALSE(malformed.transports.advertised);
    CHECK_FALSE(malformed.transports.mpeg_ts);
    CHECK_FALSE(malformed.transports.hls);
}

TEST_CASE("explicit authentication rejection is terminal and exposes no formats") {
    for (const std::string_view auth : {"0", R"("0")", "false", R"("false")"}) {
        const std::string body = std::string{"{\"user_info\":{\"auth\":"} +
                                 std::string{auth} +
                                 R"(,"allowed_output_formats":["ts","m3u8"]}})";
        const auto result = parse_account_metadata(body);
        CHECK(result.status == AccountMetadataStatus::AuthenticationRejected);
        CHECK_FALSE(result.transports.advertised);
        CHECK_FALSE(result.transports.mpeg_ts);
        CHECK_FALSE(result.transports.hls);
    }
}

TEST_CASE("unreadable account metadata closes to unavailable") {
    CHECK(parse_account_metadata("not json").status == AccountMetadataStatus::Unavailable);
    CHECK(parse_account_metadata("[]").status == AccountMetadataStatus::Unavailable);
    CHECK(parse_account_metadata("{}").status == AccountMetadataStatus::Unavailable);
}
