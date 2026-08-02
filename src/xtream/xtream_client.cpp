#include "xtream/xtream_client.hpp"

#include <windows.h>
#include <winhttp.h>

#include <nlohmann/json.hpp>

#include <array>
#include <cstdio>
#include <format>
#include <utility>

#include "util/log.hpp"

namespace coax::xtream {
namespace {

using json = nlohmann::json;

// --- small Win32 helpers ---------------------------------------------------

std::wstring widen(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        out.data(), needed);
    return out;
}

std::string narrow(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        out.data(), needed, nullptr, nullptr);
    return out;
}

// Closes a WinHTTP handle on scope exit.
class Handle {
public:
    Handle() = default;
    explicit Handle(HINTERNET h) : h_(h) {}
    ~Handle() { if (h_) WinHttpCloseHandle(h_); }

    Handle(const Handle&)            = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : h_(std::exchange(other.h_, nullptr)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            if (h_) WinHttpCloseHandle(h_);
            h_ = std::exchange(other.h_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] HINTERNET get() const { return h_; }
    explicit operator bool() const { return h_ != nullptr; }

private:
    HINTERNET h_ = nullptr;
};

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

struct CrackedUrl {
    std::wstring host;
    std::wstring path_and_query;
    INTERNET_PORT port   = 0;
    bool          secure = false;
};

bool crack(std::string_view url, CrackedUrl& out) {
    const std::wstring wide = widen(url);

    URL_COMPONENTS parts{};
    parts.dwStructSize      = sizeof(parts);
    parts.dwSchemeLength    = static_cast<DWORD>(-1);
    parts.dwHostNameLength  = static_cast<DWORD>(-1);
    parts.dwUrlPathLength   = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);

    if (!WinHttpCrackUrl(wide.c_str(), static_cast<DWORD>(wide.size()), 0, &parts)) {
        return false;
    }

    out.host   = std::wstring(parts.lpszHostName, parts.dwHostNameLength);
    out.port   = parts.nPort;
    out.secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    out.path_and_query =
        std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength) +
        std::wstring(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    return true;
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
    CrackedUrl cracked;
    if (!crack(url, cracked)) {
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

    CrackedUrl cracked;
    if (!crack(url, cracked)) {
        error = "Portal URL could not be parsed";
        return false;
    }

    Handle session(WinHttpOpen(L"coax-native/0.1",
                               WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        error = std::format("WinHttpOpen failed ({})", GetLastError());
        return false;
    }

    // Providers are often slow; these are generous but bounded.
    WinHttpSetTimeouts(session.get(), 10000, 15000, 30000, 30000);

    Handle connect(WinHttpConnect(session.get(), cracked.host.c_str(), cracked.port, 0));
    if (!connect) {
        error = std::format("Cannot reach {} ({})", narrow(cracked.host), GetLastError());
        return false;
    }

    Handle request(WinHttpOpenRequest(
        connect.get(), L"GET", cracked.path_and_query.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        cracked.secure ? WINHTTP_FLAG_SECURE : 0));
    if (!request) {
        error = std::format("WinHttpOpenRequest failed ({})", GetLastError());
        return false;
    }

    if (!WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.get(), nullptr)) {
        error = std::format("Request failed ({})", GetLastError());
        return false;
    }

    DWORD status = 0;
    DWORD size   = sizeof(status);
    WinHttpQueryHeaders(request.get(),
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        error = std::format("Provider returned HTTP {}", status);
        return false;
    }

    body.clear();
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.get(), &available) || available == 0) {
            break;
        }
        const std::size_t offset = body.size();
        body.resize(offset + available);
        DWORD read = 0;
        if (!WinHttpReadData(request.get(), body.data() + offset, available, &read)) {
            error = std::format("Read failed ({})", GetLastError());
            return false;
        }
        body.resize(offset + read);
    }
    return true;
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
