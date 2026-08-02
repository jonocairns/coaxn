#include "util/http.hpp"

#include <format>
#include <utility>

namespace coax::util::http {
namespace {

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

}  // namespace

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

bool get(std::string_view url, std::string& body, std::string& error,
         const Timeouts& timeouts) {
    CrackedUrl cracked;
    if (!crack(url, cracked)) {
        error = "URL could not be parsed";
        return false;
    }

    // GitHub's API rejects requests without a User-Agent, and providers use it
    // for support diagnostics, so it carries the real build version.
    Handle session(WinHttpOpen(L"coax-native/" COAX_VERSION_W,
                               WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        error = std::format("WinHttpOpen failed ({})", GetLastError());
        return false;
    }

    WinHttpSetTimeouts(session.get(), timeouts.resolve_ms, timeouts.connect_ms,
                       timeouts.send_ms, timeouts.receive_ms);

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
        error = std::format("{} returned HTTP {}", narrow(cracked.host), status);
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

}  // namespace coax::util::http
