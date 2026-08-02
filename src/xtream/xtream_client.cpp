#include "xtream/xtream_client.hpp"

#include <windows.h>
#include <winhttp.h>

#include <nlohmann/json.hpp>

#include <array>
#include <cstdio>
#include <format>
#include <utility>

#include "util/http.hpp"
#include "util/log.hpp"

namespace coax::xtream {
namespace {

using json = nlohmann::json;

using util::http::narrow;

std::string url_encode(std::string_view text) {
    static constexpr std::string_view kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size());
    for (const unsigned char ch : text) {
        const bool unreserved = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                                (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' ||
                                ch == '.' || ch == '~';
        if (unreserved) {
            out.push_back(static_cast<char>(ch));
        } else {
            out.push_back('%');
            out.push_back(kHex[ch >> 4]);
            out.push_back(kHex[ch & 0x0F]);
        }
    }
    return out;
}

// Reads a JSON value that providers render inconsistently as string or number.
std::string as_text(const json& value) {
    if (value.is_string())            return value.get<std::string>();
    if (value.is_number_integer())    return std::to_string(value.get<long long>());
    if (value.is_number_unsigned())   return std::to_string(value.get<unsigned long long>());
    if (value.is_number_float())      return std::format("{:.0f}", value.get<double>());
    return {};
}

std::string field_text(const json& object, std::string_view key) {
    const auto it = object.find(key);
    return it == object.end() ? std::string{} : as_text(*it);
}

int field_int(const json& object, std::string_view key) {
    const std::string text = field_text(object, key);
    if (text.empty()) {
        return 0;
    }
    try {
        return std::stoi(text);
    } catch (const std::exception&) {
        return 0;
    }
}

// Extracts one query parameter from a raw ?a=b&c=d string.
std::string query_param(std::wstring_view query, std::wstring_view key) {
    std::size_t cursor = 0;
    while (cursor < query.size()) {
        const auto amp   = query.find(L'&', cursor);
        const auto piece = query.substr(cursor, amp == std::wstring_view::npos
                                                    ? std::wstring_view::npos
                                                    : amp - cursor);
        const auto eq = piece.find(L'=');
        if (eq != std::wstring_view::npos && piece.substr(0, eq) == key) {
            return narrow(piece.substr(eq + 1));
        }
        if (amp == std::wstring_view::npos) {
            break;
        }
        cursor = amp + 1;
    }
    return {};
}

}  // namespace

bool parse_portal_url(std::string_view url, Credentials& out) {
    util::http::CrackedUrl cracked;
    if (!util::http::crack(url, cracked)) {
        return false;
    }

    const auto question = cracked.path_and_query.find(L'?');
    if (question == std::wstring::npos) {
        return false;
    }
    const std::wstring_view query{cracked.path_and_query.data() + question + 1};

    const std::string username = query_param(query, L"username");
    const std::string password = query_param(query, L"password");
    if (username.empty() || password.empty()) {
        return false;
    }

    const bool default_port =
        (cracked.secure && cracked.port == 443) || (!cracked.secure && cracked.port == 80);

    out.base_url = std::format("{}://{}{}",
                               cracked.secure ? "https" : "http",
                               narrow(cracked.host),
                               default_port ? std::string{} : std::format(":{}", cracked.port));
    out.username = username;
    out.password = password;
    return true;
}

bool Client::get(std::string_view query, std::string& body, std::string& error) const {
    const std::string url = std::format(
        "{}/player_api.php?username={}&password={}&{}",
        creds_.base_url, url_encode(creds_.username), url_encode(creds_.password), query);

    return util::http::get(url, body, error);
}

bool Client::fetch_catalog(Catalog& out, std::string& error) const {
    std::string body;

    if (!get("action=get_live_categories", body, error)) {
        return false;
    }

    json categories_json;
    try {
        categories_json = json::parse(body);
    } catch (const json::exception& e) {
        // A provider that rejects the credentials usually replies with a
        // non-JSON body or a bare {"user_info":{"auth":0}}.
        error = std::format("Provider sent an unreadable category list ({})", e.what());
        return false;
    }

    if (!categories_json.is_array()) {
        error = "Login rejected by provider";
        return false;
    }

    out.categories.clear();
    for (const auto& entry : categories_json) {
        core::Category category;
        category.id   = field_text(entry, "category_id");
        category.name = field_text(entry, "category_name");
        if (!category.id.empty()) {
            out.categories.push_back(std::move(category));
        }
    }

    if (!get("action=get_live_streams", body, error)) {
        return false;
    }

    json streams_json;
    try {
        streams_json = json::parse(body);
    } catch (const json::exception& e) {
        error = std::format("Provider sent an unreadable channel list ({})", e.what());
        return false;
    }

    if (!streams_json.is_array()) {
        error = "Provider returned no channel list";
        return false;
    }

    out.channels.clear();
    out.channels.reserve(streams_json.size());
    for (const auto& entry : streams_json) {
        core::Channel channel;
        channel.id          = field_text(entry, "stream_id");
        channel.name        = field_text(entry, "name");
        channel.category_id = field_text(entry, "category_id");
        channel.logo_url    = field_text(entry, "stream_icon");
        channel.number      = field_int(entry, "num");
        if (!channel.id.empty()) {
            out.channels.push_back(std::move(channel));
        }
    }

    log::info("Loaded {} channels across {} categories",
              out.channels.size(), out.categories.size());
    return true;
}

std::string Client::stream_url(const core::Channel& channel) const {
    return std::format("{}/live/{}/{}/{}.ts", creds_.base_url,
                       url_encode(creds_.username), url_encode(creds_.password), channel.id);
}

}  // namespace coax::xtream
