#pragma once

#include <windows.h>
#include <winhttp.h>

#include <string>
#include <string_view>

// Thin WinHTTP helpers shared by the provider client and the update check.
// Both need the same blocking GET, so it lives here rather than being written
// twice against a fiddly C API.
namespace coax::util::http {

std::wstring widen(std::string_view text);
std::string  narrow(std::wstring_view text);

struct CrackedUrl {
    std::wstring  host;
    std::wstring  path_and_query;
    INTERNET_PORT port   = 0;
    bool          secure = false;
};

bool crack(std::string_view url, CrackedUrl& out);

// The defaults are generous because providers are often slow. A caller whose
// request is discretionary should ask for less: these bound how long a worker
// thread can still be running when the user closes the window.
struct Timeouts {
    int resolve_ms = 10000;
    int connect_ms = 15000;
    int send_ms    = 30000;
    int receive_ms = 30000;
};

// Blocking GET. Returns false and fills `error` on a transport failure or any
// status other than 200. Call it off the UI thread.
bool get(std::string_view url, std::string& body, std::string& error,
         const Timeouts& timeouts = {});

}  // namespace coax::util::http
